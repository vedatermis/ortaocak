// Zone, part 2: monsters and combat (docs/PROTOCOL_COMBAT.md, docs/SERVER_WORLD.md §8b). Everything
// runs on the world thread inside Zone::combat() (requests) and Zone::update() (per tick), with the
// tick's scheduled time m_now as the only clock, so tests can drive it with a fake clock.

#include "server/world/Zone.h"

#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Movement.h"
#include "shared/game/TerrainWalk.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace vo::server::world {

namespace vn = vo::net;
namespace vg = vo::game;
using Ms = std::chrono::milliseconds;

namespace {

constexpr auto kSelfStatsInterval = Ms(250);   // SELF_STATS <= 4 Hz (§3.3)
constexpr auto kEntityStatsInterval = Ms(500); // ENTITY_STATS <= 2 Hz per entity
constexpr auto kRegenStep = Ms(1000);
constexpr auto kAggroScanInterval = Ms(200);
constexpr auto kRepathInterval = Ms(300);
constexpr auto kSearchInterval = Ms(1000); // grid path searches per monster
constexpr auto kGiveUpAfter = Ms(6000);    // no progress towards the target: return home
constexpr auto kStuckHomeAfter = Ms(10000); // returning without progress: put it home
constexpr auto kFirstAttackDelay = Ms(400);
constexpr float kChaseInside = 0.4f; // a chasing monster stops this far inside its attack range
constexpr float kHomeEpsilon = 0.5f;
constexpr int kSearchCells = 6000; // bounded grid search (about a 40 m radius)

core::MonoClock::duration msDuration(std::uint32_t ms)
{
    return std::chrono::duration_cast<core::MonoClock::duration>(Ms(ms));
}

float dist(float ax, float az, float bx, float bz)
{
    return vg::horizontalDistance(ax, az, bx, bz);
}

} // namespace

// ---- Broadcast (§3.3; Zone::sendAround is defined in Zone.h) ----

void Zone::actionFail(Player& p, vn::CombatAction action, std::uint16_t skill, vn::ActionFailReason reason)
{
    p.out.appendMessage(vn::ActionFail{action, skill, reason}, m_scratch);
}

// ---- Clock, state ----

void Zone::beginTick(core::MonoTime logicalNow, std::uint32_t logicalMs)
{
    m_now = logicalNow;
    m_nowMs = logicalMs;
}

bool Zone::inSafeZone(float x, float z) const noexcept
{
    for (const vg::SafeZone& s : m_safeZones)
        if (dist(x, z, s.x, s.z) <= s.radius)
            return true;
    return false;
}

auth::CharacterProgress Zone::progressOf(const Player& p) noexcept
{
    const auto& c = p.combat;
    return {p.character.level, c.xp, c.dead ? 0u : c.hp, c.mp};
}

bool Zone::inCombat(const Player& p) const noexcept
{
    return p.combat.everFought && m_now - p.combat.lastCombatAt < vg::kOutOfCombatAfter;
}

std::uint8_t Zone::stateOf(const Player& p) const noexcept
{
    if (p.combat.dead)
        return vn::kEntityStateDead;
    std::uint8_t s = 0;
    if (inCombat(p))
        s |= vn::kEntityStateInCombat;
    if (p.combat.sitting)
        s |= vn::kEntityStateSitting;
    return s;
}

std::uint8_t Zone::stateOf(const Monster& m) noexcept
{
    switch (m.state)
    {
    case Monster::State::Dead:
    case Monster::State::Gone: return vn::kEntityStateDead;
    case Monster::State::Chase: return vn::kEntityStateInCombat;
    default: return 0;
    }
}

// ---- Players: setup ----

void Zone::initCombat(Player& p, const auth::CharacterInfo& info, vg::EquipBonus equip, bool& leftDead)
{
    auto& c = p.combat;
    c = {};
    c.equip = equip;
    c.allocated = info.statPoints;
    c.skillRanks = vg::normalizeSkillRanks(vg::skills(), info.charClass, info.skillRanks);
    const std::uint16_t level = std::clamp<std::uint16_t>(info.level, 1, vg::kLevelCap);
    p.character.level = level;
    c.baseStats = vg::deriveStats(info.charClass, level, c.allocated, equip);
    c.stats = c.baseStats;
    const std::uint32_t need = vg::xpToNext(level);
    c.xp = need == 0 ? 0u : std::min(info.xp, need - 1);
    leftDead = info.hp && *info.hp == 0;
    if (leftDead)
    {
        // §3.5: logged out dead -> race spawn with 50 % hp/mp.
        c.hp = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(c.stats.maxHp * vg::kRespawnHpFraction)));
        c.mp = static_cast<std::uint32_t>(std::lround(c.stats.maxMp * vg::kRespawnHpFraction));
    }
    else
    {
        c.hp = std::clamp<std::uint32_t>(info.hp.value_or(c.stats.maxHp), 1, c.stats.maxHp);
        c.mp = std::min<std::uint32_t>(info.mp.value_or(c.stats.maxMp), c.stats.maxMp);
    }
    c.nextRegenAt = m_now + kRegenStep;
}

void Zone::recomputeStats(Player& p)
{
    auto& c = p.combat;
    c.baseStats = vg::deriveStats(p.character.charClass, p.character.level, c.allocated, c.equip);
    c.stats = vg::applyEffectModifiers(c.baseStats, modifiersOf(p));
    c.hp = std::min(c.hp, c.stats.maxHp);
    c.mp = std::min(c.mp, c.stats.maxMp);
}

void Zone::setSkillRanks(SessionKey session, const vg::SkillRanks& ranks)
{
    Player* p = find(session);
    if (!p)
        return;
    const vg::SkillRanks normalized = vg::normalizeSkillRanks(vg::skills(), p->character.charClass, ranks);
    if (normalized == p->combat.skillRanks)
        return;
    const bool healChanged = normalized[vg::kSkillPaleSun] != p->combat.skillRanks[vg::kSkillPaleSun];
    p->combat.skillRanks = normalized;
    if (healChanged)
        p->combat.charStatsDirty = true; // CHAR_STATS.heal_permille follows the rank (PROTOCOL_SKILLS.md §3.3)
}

void Zone::setAllocation(SessionKey session, const vg::Attributes& allocated)
{
    Player* p = find(session);
    if (!p || p->combat.allocated == allocated)
        return;
    auto& c = p->combat;
    const vg::Stats before = c.stats;
    c.allocated = allocated;
    recomputeStats(*p); // hp / mp stay (clamped if a maximum shrank, after a reset)
    c.charStatsDirty = true;
    if (before.maxHp != c.stats.maxHp || before.maxMp != c.stats.maxMp || before.attack != c.stats.attack ||
        before.defense != c.stats.defense)
    {
        c.selfStatsDirty = true;
        if (before.maxHp != c.stats.maxHp)
            c.entityStatsDirty = true; // observers' hp bars
    }
}

void Zone::sendCharStats(Player& p)
{
    const auto& c = p.combat;
    const vg::Stats& s = c.stats;
    vn::CharStats m;
    m.level = p.character.level;
    for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
        m.attributes[i] = {std::min(s.base[i], vg::kAttributeCap), s.bonus[i]};
    m.unspent = s.unspent;
    m.maxHp = s.maxHp;
    m.maxMp = s.maxMp;
    m.attack = s.attack;
    m.defense = s.defense;
    m.critPermille = s.critPermille;
    m.dodgePermille = s.dodgePermille;
    const auto tenths = [](float perSecond) {
        return static_cast<std::uint16_t>(std::clamp(std::lround(perSecond * 10.0f), 0L, 65535L));
    };
    m.hpRegenTenths = tenths(s.hpRegen);
    m.mpRegenTenths = tenths(s.mpRegen);
    m.attackIntervalMs = static_cast<std::uint16_t>(std::min<std::uint32_t>(s.attackIntervalMs, 65535));
    const vg::SkillDef* heal = vg::findSkill(vg::kSkillPaleSun);
    m.healPermille =
        heal ? vg::healPermille(*heal, s.healMultiplier, std::max<std::uint8_t>(c.skillRanks[vg::kSkillPaleSun], 1)) : 0;
    p.out.appendMessage(m, m_scratch);
}

void Zone::setVitalsForTesting(SessionKey session, std::uint32_t hp, std::uint32_t mp)
{
    if (Player* p = find(session))
    {
        p->combat.hp = std::min(hp, p->combat.stats.maxHp);
        p->combat.mp = std::min(mp, p->combat.stats.maxMp);
        p->combat.selfStatsDirty = true;
    }
}

void Zone::killPlayerForTesting(SessionKey session)
{
    if (Player* p = find(session); p && !p->combat.dead)
    {
        p->combat.hp = 0;
        killPlayer(*p, 0);
    }
}

void Zone::standUp(Player& p)
{
    p.combat.sitting = false; // the state flush tells everyone
}

void Zone::markInCombat(Player& p)
{
    p.combat.everFought = true;
    p.combat.lastCombatAt = m_now;
}

// ---- Monsters: setup and visibility ----

bool Zone::installMonsters(std::shared_ptr<const vg::MonsterData> data, const std::function<EntityId()>& allocate,
                           core::MonoTime now, std::string& error)
{
    if (!data)
        return true;
    std::vector<vg::SafeZone> safe;
    for (const vg::SafeZone& s : data->safeZones())
        if (s.zone == m_id)
            safe.push_back(s);
    for (const vg::MonsterSpawn& s : data->spawns())
    {
        if (s.zone != m_id)
            continue;
        if (!m_terrain.isWalkable(s.x, s.z))
        {
            error = std::format("monster spawn of monster {} at ({:.1f}, {:.1f}) in zone {} is not walkable", s.monster,
                                s.x, s.z, m_id);
            return false;
        }
        for (const vg::SafeZone& z : safe)
            if (dist(s.x, s.z, z.x, z.z) <= z.radius + s.radius)
            {
                error = std::format("monster spawn at ({:.1f}, {:.1f}) reaches into a safe zone of zone {}", s.x, s.z, m_id);
                return false;
            }
    }
    m_monsterData = std::move(data);
    m_safeZones = std::move(safe);
    if (m_now == core::MonoTime{})
        m_now = now;
    for (const vg::MonsterSpawn& s : m_monsterData->spawns())
    {
        if (s.zone != m_id)
            continue;
        const vg::MonsterTemplate* t = m_monsterData->find(s.monster);
        for (std::uint16_t k = 0; k < s.count; ++k)
        {
            auto owned = std::make_unique<Monster>();
            Monster& m = *owned;
            m.id = allocate();
            m.tmpl = t;
            m.homeX = s.x;
            m.homeZ = s.z;
            m.radius = s.radius;
            m_monsters.emplace(m.id, std::move(owned));
            spawnMonster(m, false);
        }
    }
    return true;
}

void Zone::monsterSeen(Player& p, Monster& m)
{
    if (!m.inGrid)
        return;
    if (p.known.insert(m.id).second)
    {
        m.observers.insert(p.id);
        sendMonsterSpawn(p, m);
    }
}

void Zone::monsterUnseen(Player& p, Monster& m)
{
    if (p.known.erase(m.id) > 0)
    {
        m.observers.erase(p.id);
        sendDespawn(p, m.id);
    }
}

void Zone::sendMonsterSpawn(Player& to, const Monster& m)
{
    vn::EntitySpawn s;
    s.entityId = m.id;
    s.entityType = vn::kEntityTypeMonster;
    s.name = m.tmpl->name;
    s.level = m.tmpl->level;
    s.x = m.x;
    s.y = m.y;
    s.z = m.z;
    s.yaw = m.yaw;
    s.flags = m.flags;
    s.hp = m.hp;
    s.maxHp = m.tmpl->hp;
    s.templateId = m.tmpl->id;
    s.state = m.sentState;
    to.out.appendMessage(s, m_scratch);
    ++m_counters.spawns;
    if (!m.effects.empty())
        sendEffectList(to, m.id, m.effects); // PROTOCOL_SKILLS.md §3.7
}

void Zone::spawnMonster(Monster& m, bool atHome)
{
    float x = m.homeX, z = m.homeZ;
    if (!atHome && m.radius > 0.5f)
    {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const float a = u(m_rng) * 2.0f * std::numbers::pi_v<float>;
            const float r = m.radius * std::sqrt(u(m_rng));
            const float cx = m.homeX + r * std::sin(a), cz = m.homeZ + r * std::cos(a);
            if (!inSafeZone(cx, cz) && vg::isSegmentWalkable(m_terrain, m.homeX, m.homeZ, cx, cz))
            {
                x = cx;
                z = cz;
                break;
            }
        }
    }
    std::uniform_real_distribution<float> yaw(-std::numbers::pi_v<float>, std::numbers::pi_v<float>);
    std::uniform_int_distribution<int> delay(1000, 6000);
    m.x = x;
    m.z = z;
    m.y = m_terrain.heightAt(x, z);
    m.yaw = yaw(m_rng);
    m.flags = 0;
    m.hp = m.tmpl->hp;
    m.state = Monster::State::Idle;
    m.target = 0;
    m.threats.clear();
    m.path.clear();
    m.pathIndex = 0;
    m.nextWanderAt = m_now + Ms(delay(m_rng));
    m.nextAttackAt = m_now;
    m.nextAggroScanAt = m_now;
    m.lastProgressAt = m_now;
    m.moveTimeMs = m_nowMs;
    m.relayDirty = false;
    m.everRelayed = false;
    m.sentState = 0;
    m.statsDirty = false;
    m.cell = m_grid.cellOf(x, z);
    m.inGrid = true;
    m_grid.forEachInView(m.cell, [&](EntityId other) {
        if (Player* p = findById(other))
            monsterSeen(*p, m);
    });
    m_grid.add(m.id, m.cell);
}

void Zone::despawnMonster(Monster& m)
{
    for (const EntityId o : m.observers)
        if (Player* p = findById(o); p && p->known.erase(m.id) > 0)
            sendDespawn(*p, m.id);
    m.observers.clear();
    if (m.inGrid)
        m_grid.remove(m.id, m.cell);
    m.inGrid = false;
    m.state = Monster::State::Gone;
    m.respawnAt = m.diedAt + msDuration(m.tmpl->respawnMs);
    m.path.clear();
    m.flags = 0;
    m.effects.clear();
}

void Zone::setMonsterPosition(Monster& m, float x, float z)
{
    m.x = x;
    m.z = z;
    m.y = m_terrain.heightAt(x, z);
    m.relayDirty = true;
    m.moveTimeMs = m_nowMs;
    const AoiCell cell = m_grid.cellOf(x, z);
    if (cell == m.cell || !m.inGrid)
    {
        m.cell = cell;
        return;
    }
    const AoiCell from = m.cell;
    m_grid.move(m.id, from, cell);
    m.cell = cell;
    m_grid.forEachViewChange(
        from, cell,
        [&](EntityId other) {
            if (Player* p = findById(other))
                monsterUnseen(*p, m);
        },
        [&](EntityId other) {
            if (Player* p = findById(other))
                monsterSeen(*p, m);
        });
}

void Zone::placeMonsterForTesting(EntityId id, float x, float z, std::optional<std::uint32_t> hp)
{
    Monster* m = monsterById(id);
    if (!m || !m->inGrid)
        return;
    m->path.clear();
    m->pathIndex = 0;
    m->flags = 0;
    setMonsterPosition(*m, x, z);
    if (hp)
        m->hp = std::clamp<std::uint32_t>(*hp, 1, m->tmpl->hp);
}

void Zone::relayMonster(Monster& m, core::MonoTime logicalNow)
{
    if (m.everRelayed && logicalNow - m.lastRelay < m_settings.relayInterval)
        return; // stays dirty: the latest state goes out at the next allowed tick
    m.relayDirty = false;
    if (m.everRelayed && m.x == m.relayedX && m.y == m.relayedY && m.z == m.relayedZ && m.yaw == m.relayedYaw &&
        m.flags == m.relayedFlags)
        return;
    m.everRelayed = true;
    m.lastRelay = logicalNow;
    m.relayedX = m.x;
    m.relayedY = m.y;
    m.relayedZ = m.z;
    m.relayedYaw = m.yaw;
    m.relayedFlags = m.flags;
    if (m.observers.empty())
        return;
    vn::EntityMove msg{m.id, m.flags, m.x, m.y, m.z, m.yaw, m.moveTimeMs};
    m_scratch.clear();
    msg.write(m_scratch);
    const auto payload = m_scratch.data();
    for (const EntityId o : m.observers)
    {
        if (Player* p = findById(o))
        {
            p->out.append(static_cast<std::uint16_t>(vn::Opcode::EntityMove), payload);
            ++m_counters.relays;
        }
    }
}

// ---- Monsters: AI (§1.3) ----

Zone::Monster::Threat* Zone::threatOf(Monster& m, EntityId player)
{
    for (auto& t : m.threats)
        if (t.player == player)
            return &t;
    return nullptr;
}

bool Zone::validMonsterTarget(const Monster& m, const Player& p) const
{
    return !p.combat.dead && !p.loading && m.observers.contains(p.id) && !inSafeZone(p.move.x, p.move.z) &&
           dist(p.move.x, p.move.z, m.homeX, m.homeZ) <= m.tmpl->leashRadius;
}

void Zone::stopMonster(Monster& m)
{
    m.path.clear();
    m.pathIndex = 0;
    if (m.flags != 0)
    {
        m.flags = 0;
        m.relayDirty = true;
        m.moveTimeMs = m_nowMs;
    }
}

bool Zone::pathMonsterTo(Monster& m, float x, float z, bool allowSearch)
{
    m.nextRepathAt = m_now + kRepathInterval;
    if (vg::isSegmentWalkable(m_terrain, m.x, m.z, x, z))
    {
        m.path.assign(1, vg::PathPoint{x, z});
        m.pathIndex = 0;
        return true;
    }
    if (!allowSearch || m_now < m.nextSearchAt)
        return false;
    m.nextSearchAt = m_now + kSearchInterval;
    ++m_counters.pathSearches;
    auto found = vg::findWalkPath(m_terrain, {m.x, m.z}, {x, z}, kSearchCells);
    if (!found)
        return false;
    m.path = std::move(*found);
    m.pathIndex = 0;
    return true;
}

bool Zone::walkMonster(Monster& m, float speed, bool running)
{
    float budget = speed * m_dt;
    float x = m.x, z = m.z;
    while (budget > 1e-5f && m.pathIndex < m.path.size())
    {
        const vg::PathPoint target = m.path[m.pathIndex];
        const float d = dist(x, z, target.x, target.z);
        float nx = target.x, nz = target.z;
        if (d > budget)
        {
            nx = x + (target.x - x) * budget / d;
            nz = z + (target.z - z) * budget / d;
            budget = 0.0f;
        }
        else
        {
            budget -= d;
            ++m.pathIndex;
        }
        if (inSafeZone(nx, nz) || !m_terrain.isWalkable(nx, nz))
        {
            // Monsters never enter a safe zone (and paths only hold walkable segments).
            m.path.clear();
            m.pathIndex = 0;
            break;
        }
        x = nx;
        z = nz;
    }
    if (x != m.x || z != m.z)
    {
        const float dx = x - m.x, dz = z - m.z;
        m.yaw = game::yawFromDirection(dx, dz);
        m.flags = static_cast<std::uint8_t>(game::kMoveMoving | (running ? game::kMoveRunning : 0));
        m.lastProgressAt = m_now;
        setMonsterPosition(m, x, z);
    }
    if (m.pathIndex >= m.path.size())
    {
        stopMonster(m);
        return true;
    }
    return false;
}

void Zone::startChase(Monster& m, Player& p)
{
    if (!threatOf(m, p.id))
        m.threats.push_back({p.id, 0.0f, 0});
    m.state = Monster::State::Chase;
    m.target = p.id;
    m.path.clear();
    m.pathIndex = 0;
    m.nextRepathAt = m_now;
    m.nextAttackAt = std::max(m.nextAttackAt, m_now + kFirstAttackDelay);
    m.lastProgressAt = m_now;
}

void Zone::startReturn(Monster& m)
{
    m.state = Monster::State::Return;
    m.threats.clear();
    m.target = 0;
    clearMonsterEffects(m, true); // PROTOCOL_SKILLS.md §1.5: Cleared
    m.path.clear();
    m.pathIndex = 0;
    m.lastProgressAt = m_now;
    if (m.hp != m.tmpl->hp)
    {
        m.hp = m.tmpl->hp; // §1.3: heal to full, ignore damage while returning
        m.statsDirty = true;
    }
    (void)pathMonsterTo(m, m.homeX, m.homeZ, true);
}

void Zone::updateMonster(Monster& m)
{
    switch (m.state)
    {
    case Monster::State::Gone:
        if (m_now >= m.respawnAt)
        {
            spawnMonster(m, false);
            ++m_counters.monsterRespawns;
        }
        return;
    case Monster::State::Dead:
        if (m_now - m.diedAt >= vg::kMonsterCorpseTime)
            despawnMonster(m);
        return;
    default: break;
    }
    if (!m.effects.empty())
    {
        updateMonsterEffects(m); // DoT ticks may kill it
        if (m.state == Monster::State::Dead)
            return;
    }
    switch (m.state)
    {
    case Monster::State::Idle: monsterIdle(m); return;
    case Monster::State::Chase: monsterChase(m); return;
    case Monster::State::Return: monsterReturn(m); return;
    default: return;
    }
}

void Zone::monsterIdle(Monster& m)
{
    if (hasEffect(m.effects, vg::EffectKind::Stun))
    {
        stopMonster(m);
        return;
    }
    if (m.observers.empty())
    {
        stopMonster(m); // nobody sees it: no wandering, no aggro (keeps the tick cheap)
        return;
    }
    if (m_now >= m.nextAggroScanAt)
    {
        m.nextAggroScanAt = m_now + kAggroScanInterval;
        Player* best = nullptr;
        float bestDistance = m.tmpl->aggroRadius;
        for (const EntityId o : m.observers)
        {
            Player* p = findById(o);
            if (!p)
                continue;
            const float d = dist(p->move.x, p->move.z, m.x, m.z);
            if (d <= bestDistance && validMonsterTarget(m, *p))
            {
                best = p;
                bestDistance = d;
            }
        }
        if (best)
        {
            startChase(m, *best);
            return;
        }
    }
    if (hasEffect(m.effects, vg::EffectKind::Root))
    {
        stopMonster(m);
        return;
    }
    if (m.pathIndex < m.path.size())
    {
        if (walkMonster(m, m.tmpl->wanderSpeed * (1.0f - strongest(m.effects, vg::EffectKind::Slow) / 100.0f), false))
            m.nextWanderAt = m_now + Ms(std::uniform_int_distribution<int>(4000, 9000)(m_rng));
        return;
    }
    if (m_now < m.nextWanderAt || m.radius <= 0.5f)
        return;
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const float a = u(m_rng) * 2.0f * std::numbers::pi_v<float>;
        const float r = m.radius * std::sqrt(u(m_rng));
        const float x = m.homeX + r * std::sin(a), z = m.homeZ + r * std::cos(a);
        if (dist(x, z, m.x, m.z) < 1.0f || inSafeZone(x, z) || !vg::isSegmentWalkable(m_terrain, m.x, m.z, x, z))
            continue;
        m.path.assign(1, vg::PathPoint{x, z});
        m.pathIndex = 0;
        return;
    }
    m.nextWanderAt = m_now + Ms(2000);
}

void Zone::monsterChase(Monster& m)
{
    // Drop players that are no longer valid targets (gone, dead, safe, out of view or of the leash).
    std::erase_if(m.threats, [&](const Monster::Threat& t) {
        const Player* p = findById(t.player);
        return !p || !validMonsterTarget(m, *p);
    });
    Player* target = nullptr;
    float best = -1.0f;
    for (const auto& t : m.threats)
    {
        if (t.threat > best)
        {
            best = t.threat;
            target = findById(t.player);
        }
    }
    // PROTOCOL_SKILLS.md §1.6: a taunt forces the target while its source is a valid one.
    if (const EntityId taunter = tauntSource(m); taunter != 0)
        if (Player* t = findById(taunter); t && threatOf(m, taunter) && validMonsterTarget(m, *t))
            target = t;
    const bool stunned = hasEffect(m.effects, vg::EffectKind::Stun);
    const bool rooted = hasEffect(m.effects, vg::EffectKind::Root);
    if (stunned || rooted)
        m.lastProgressAt = m_now; // the leash timer pauses (§1.5)
    if (!target || dist(m.x, m.z, m.homeX, m.homeZ) > m.tmpl->leashRadius + m.tmpl->attackRange + 1.0f ||
        m_now - m.lastProgressAt > kGiveUpAfter)
    {
        startReturn(m);
        return;
    }
    if (stunned)
    {
        stopMonster(m);
        return;
    }
    if (m.target != target->id)
    {
        m.target = target->id;
        m.path.clear();
        m.pathIndex = 0;
    }
    const float tx = target->move.x, tz = target->move.z;
    const float d = dist(m.x, m.z, tx, tz);
    if (vg::inRange(d, m.tmpl->attackRange))
    {
        stopMonster(m);
        const float yaw = game::yawFromDirection(tx - m.x, tz - m.z);
        if (d > 0.05f && std::abs(game::normalizeYaw(yaw - m.yaw)) > 0.05f)
        {
            m.yaw = yaw;
            m.relayDirty = true;
            m.moveTimeMs = m_nowMs;
        }
        m.lastProgressAt = m_now;
        if (m_now >= m.nextAttackAt)
            monsterAttack(m, *target);
        return;
    }
    if (rooted)
    {
        stopMonster(m);
        return;
    }
    const bool pathDone = m.pathIndex >= m.path.size();
    const bool targetMoved =
        !m.path.empty() && dist(m.path.back().x, m.path.back().z, tx, tz) > 1.0f && m_now >= m.nextRepathAt;
    if (pathDone || targetMoved)
        (void)pathMonsterTo(m, tx, tz, true);
    (void)walkMonster(m, m.tmpl->moveSpeed * (1.0f - strongest(m.effects, vg::EffectKind::Slow) / 100.0f), true);
    if (dist(m.x, m.z, tx, tz) <= std::max(m.tmpl->attackRange - kChaseInside, 0.5f))
        stopMonster(m);
}

void Zone::monsterReturn(Monster& m)
{
    if (dist(m.x, m.z, m.homeX, m.homeZ) <= kHomeEpsilon)
    {
        stopMonster(m);
        m.state = Monster::State::Idle;
        m.nextWanderAt = m_now + Ms(std::uniform_int_distribution<int>(2000, 5000)(m_rng));
        m.nextAggroScanAt = m_now + Ms(1000);
        return;
    }
    if (m.pathIndex >= m.path.size() && !pathMonsterTo(m, m.homeX, m.homeZ, true) &&
        m_now - m.lastProgressAt > kStuckHomeAfter)
    {
        VO_LOG_DEBUG("zone {}: monster {} cannot walk home, placing it there", m_id, m.id);
        m.yaw = 0.0f;
        setMonsterPosition(m, m.homeX, m.homeZ);
        return;
    }
    (void)walkMonster(m, m.tmpl->moveSpeed, true);
}

void Zone::monsterAttack(Monster& m, Player& p)
{
    // PROTOCOL_SKILLS.md §1.5: a slow also lengthens the attack interval by half its value.
    const float slow = strongest(m.effects, vg::EffectKind::Slow);
    m.nextAttackAt = m_now + msDuration(static_cast<std::uint32_t>(
                                 std::lround(static_cast<float>(m.tmpl->attackIntervalMs) * (1.0f + slow / 200.0f))));
    // PROTOCOL_STATS.md §2.1: the player's dodge chance first; a dodge deals 0 and is a Miss.
    if (p.combat.stats.dodgeChance > 0.0f &&
        vg::dodged(p.combat.stats.dodgeChance, std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng)))
    {
        missPlayer(m, p);
        return;
    }
    const float weaken = std::min(summed(m.effects, vg::EffectKind::Weaken), 75.0f);
    const vg::Hit hit = vg::rollDamage(m.tmpl->attack, 1.0f - weaken / 100.0f, p.combat.stats.defense, 0.05f, m_rng);
    damagePlayer(m, p, hit);
}

void Zone::missPlayer(Monster& m, Player& p)
{
    markInCombat(p);
    standUp(p);
    ++m_counters.combatEvents;
    ++m_counters.dodges;
    sendAround(vn::CombatEvent{m.id, p.id, 0, vn::kCombatKindMiss, 0, 0, p.combat.hp, m_nowMs}, m.id, p.id);
}

void Zone::damagePlayer(Monster& m, Player& p, vg::Hit hit)
{
    auto& c = p.combat;
    bool absorbed = false;
    const std::uint32_t afterShield = absorbDamage(p, hit.amount, absorbed); // PROTOCOL_SKILLS.md §1.3
    const std::uint32_t applied = std::min(afterShield, c.hp);
    c.hp -= applied;
    markInCombat(p);
    standUp(p); // §3.6: taking damage stands the player up
    c.selfStatsDirty = true;
    p.saveDirty = true;
    ++m_counters.combatEvents;
    const std::uint8_t flags = static_cast<std::uint8_t>((hit.critical ? vn::kCombatFlagCritical : 0) |
                                                         (c.hp == 0 ? vn::kCombatFlagKillingBlow : 0) |
                                                         (absorbed ? vn::kCombatFlagAbsorbed : 0));
    sendAround(vn::CombatEvent{m.id, p.id, 0, vn::kCombatKindDamage, flags, applied, c.hp, m_nowMs}, m.id, p.id);
    if (c.hp == 0)
        killPlayer(p, m.id);
}

void Zone::killMonster(Monster& m, EntityId killer)
{
    m.state = Monster::State::Dead;
    m.diedAt = m_now;
    stopMonster(m);
    clearMonsterEffects(m, false); // PROTOCOL_SKILLS.md §1.5: death clears silently
    sendAround(vn::EntityDeath{m.id, killer}, m.id, killer);
    for (const EntityId o : m.observers) // §3.1: auto-attacks on it end with its death
        if (Player* p = findById(o); p && p->combat.attackTarget == m.id)
            p->combat.attackTarget = 0;
    // §1.1: the experience goes to the player who dealt the most damage.
    const Monster::Threat* top = nullptr;
    for (const auto& t : m.threats)
        if (!top || t.damage > top->damage)
            top = &t;
    Player* winner = top && top->damage > 0 ? findById(top->player) : nullptr;
    m.threats.clear();
    m.target = 0;
    ++m_counters.monsterKills;
    if (winner)
    {
        const std::uint32_t xp = vg::xpForKill(m.tmpl->xp, winner->character.level, m.tmpl->level);
        if (xp > 0)
            gainExperience(*winner, xp, m.id);
    }
    // PROTOCOL_LOOT.md §1.3: the container belongs to the experience winner (ties: the first to
    // damage it, as the threat list keeps that order), else to the killing blow's player.
    const Player* owner = winner ? winner : findById(killer);
    dropLoot(m, owner ? owner->character.id : auth::CharacterId{0});
}

// ---- Players: combat ----

void Zone::damageMonster(Player* p, EntityId source, Monster& m, std::uint16_t skillId, vg::Hit hit,
                         std::uint8_t extraFlags)
{
    const std::uint32_t applied = std::min(hit.amount, m.hp);
    m.hp -= applied;
    if (p)
    {
        Monster::Threat* t = threatOf(m, p->id);
        if (!t)
        {
            m.threats.push_back({p->id, 0.0f, 0});
            t = &m.threats.back();
        }
        t->threat += static_cast<float>(applied);
        t->damage += applied;
        markInCombat(*p);
        if (m.state == Monster::State::Idle && m.hp > 0)
            startChase(m, *p); // §1.3: damage pulls it
    }
    ++m_counters.combatEvents;
    const std::uint8_t flags = static_cast<std::uint8_t>((hit.critical ? vn::kCombatFlagCritical : 0) |
                                                         (m.hp == 0 ? vn::kCombatFlagKillingBlow : 0) | extraFlags);
    sendAround(vn::CombatEvent{source, m.id, skillId, vn::kCombatKindDamage, flags, applied, m.hp, m_nowMs},
               p ? p->id : 0, m.id);
    if (m.hp == 0)
        killMonster(m, source);
}

void Zone::healPlayer(Player& healer, Player& target, const vg::SkillDef& skill, std::uint8_t rank)
{
    // PROTOCOL_STATS.md §2: the target's max hp, the caster's Zekâ and İrade.
    const std::uint32_t amount =
        vg::healAmount(skill, target.combat.stats.maxHp, healer.combat.stats.healMultiplier, rank);
    healPlayerBy(&healer, healer.id, target, skill.id, amount, 0);
}

void Zone::healPlayerBy(Player* healer, EntityId source, Player& target, std::uint16_t skillId, std::uint32_t amount,
                        std::uint8_t flags)
{
    auto& c = target.combat;
    const std::uint32_t applied = std::min(amount, c.stats.maxHp - std::min(c.hp, c.stats.maxHp));
    c.hp += applied;
    c.selfStatsDirty = true;
    target.saveDirty = true;
    ++m_counters.combatEvents;
    sendAround(vn::CombatEvent{source, target.id, skillId, vn::kCombatKindHeal, flags, applied, c.hp, m_nowMs},
               healer ? healer->id : 0, target.id);
    if (applied == 0 || !healer)
        return;
    Player& h = *healer;
    // §1.3: healing counts x0.5 as threat towards the healer on every monster fighting the target.
    for (auto& [id, owned] : m_monsters)
    {
        Monster& m = *owned;
        if (m.state != Monster::State::Chase || !threatOf(m, target.id))
            continue;
        Monster::Threat* t = threatOf(m, h.id);
        if (!t)
        {
            m.threats.push_back({h.id, 0.0f, 0});
            t = &m.threats.back();
        }
        t->threat += 0.5f * static_cast<float>(applied);
    }
}

void Zone::killPlayer(Player& p, EntityId killer)
{
    auto& c = p.combat;
    c.dead = true;
    c.diedAt = m_now;
    c.attackTarget = 0;
    interruptCast(p, false);
    c.sitting = false;
    clearPlayerEffects(p); // PROTOCOL_SKILLS.md §1.5: silently
    if ((p.move.flags & game::kMoveMoving) != 0)
    {
        p.move.flags = static_cast<std::uint8_t>(p.move.flags & ~game::kMoveMoving);
        p.relayDirty = true;
        p.moveTimeMs = m_nowMs;
    }
    sendAround(vn::EntityDeath{p.id, killer}, p.id, killer);
    p.out.dead = true;
    p.saveDirty = true;
    c.selfStatsDirty = true;
    ++m_counters.playerDeaths;
}

void Zone::respawnPlayer(Player& p)
{
    auto& c = p.combat;
    const auth::CharacterPosition spawn = raceSpawn(p.character.race);
    float x = spawn.x, z = spawn.z;
    spreadAround(x, z);
    c.dead = false;
    c.hp = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(c.stats.maxHp * vg::kRespawnHpFraction)));
    c.mp = static_cast<std::uint32_t>(std::lround(c.stats.maxMp * vg::kRespawnHpFraction));
    c.everFought = false;
    c.hpRegen = c.mpRegen = 0.0f;
    c.nextRegenAt = m_now + kRegenStep;
    // §3.4: observers get ENTITY_STATE (dead cleared), then an ENTITY_MOVE with the new position.
    c.sentState = stateOf(p);
    sendAround(vn::EntityState{p.id, c.sentState}, p.id);
    p.move.flags = 0;
    p.move.yaw = spawn.yaw;
    p.move.credit = 0.0f;
    p.moveTimeMs = m_nowMs;
    movePlayerTo(p, x, z);
    p.out.appendMessage(vn::RespawnRes{vn::RespawnResult::Ok, p.move.x, p.move.y, p.move.z, p.move.yaw, c.hp, c.mp},
                        m_scratch);
    p.out.dead = false;
    c.selfStatsDirty = true;
    c.entityStatsDirty = true;
}

void Zone::gainExperience(Player& p, std::uint32_t amount, EntityId source)
{
    auto& c = p.combat;
    const std::uint16_t before = p.character.level;
    const vg::LevelProgress after = vg::addExperience({before, c.xp}, amount);
    c.xp = after.xp;
    p.saveDirty = true;
    const bool levelled = after.level != before;
    if (levelled)
    {
        p.character.level = after.level;
        recomputeStats(p);
        c.hp = c.stats.maxHp; // a level-up refills hp and mp
        c.mp = c.stats.maxMp;
        c.saveNow = true;     // §3.5: saved on level-up
        p.out.level = after.level;
    }
    p.out.appendMessage(vn::XpGain{amount, c.xp, vg::xpToNext(p.character.level), source}, m_scratch);
    if (levelled)
    {
        sendAround(vn::LevelUp{p.id, p.character.level}, p.id);
        c.selfStatsDirty = true;
        c.entityStatsDirty = true;
        c.charStatsDirty = true; // PROTOCOL_STATS.md §1.2: +3 unspent points per level
        VO_LOG_DEBUG("zone {}: character {} reached level {}", m_id, p.character.id, p.character.level);
    }
}

void Zone::interruptCast(Player& p, bool notify)
{
    auto& c = p.combat;
    if (c.castSkill == 0)
        return;
    const std::uint16_t skill = c.castSkill;
    const EntityId target = c.castTarget;
    c.castSkill = 0; // costs nothing and starts no cooldown (§1.2)
    sendAround(vn::CastEnd{p.id, skill, vn::kCastInterrupted}, p.id, target == p.id ? 0 : target);
    if (notify)
        actionFail(p, vn::CombatAction::Skill, skill, vn::ActionFailReason::Interrupted);
}

std::uint8_t Zone::checkSkillTarget(Player& p, const vg::SkillDef& skill, EntityId target, EntityId& resolved)
{
    using R = vn::ActionFailReason;
    const auto reason = [](R r) { return static_cast<std::uint8_t>(r); };
    const vg::SkillRankData& rank = skill.at(rankOf(p, skill));
    if (skill.target == vg::SkillTarget::Enemy)
    {
        if (target == 0 || !knows(p, target))
            return reason(R::NoTarget);
        if (findById(target))
            return reason(R::NotAttackable); // no PvP in M9
        const Monster* m = monsterById(target);
        if (!m)
            return reason(R::NoTarget);
        if (m->state == Monster::State::Dead || m->state == Monster::State::Gone)
            return reason(R::TargetDead);
        if (m->state == Monster::State::Return)
            return reason(R::NotAttackable); // ignores damage while returning
        if (!vg::inRange(dist(p.move.x, p.move.z, m->x, m->z), rank.range))
            return reason(R::OutOfRange);
        if (inSafeZone(p.move.x, p.move.z) || inSafeZone(m->x, m->z))
            return reason(R::InSafeZone);
        float lx = 0.0f, lz = 0.0f;
        if (skill.leap && !findLeapPoint(p, *m, skill.leapStop, lx, lz))
            return reason(R::NoPath); // PROTOCOL_SKILLS.md §1.4
        resolved = target;
        return 0;
    }
    if (skill.target == vg::SkillTarget::Self)
    {
        // PROTOCOL_SKILLS.md §1.4: an around-self enemy area needs someone to hit.
        if (skill.area == vg::SkillArea::AroundSelf && !skill.affectsAllies)
        {
            if (inSafeZone(p.move.x, p.move.z))
                return reason(R::InSafeZone);
            std::vector<Monster*> found;
            collectEnemies(p, p.move.x, p.move.z, skill.radius, 1, 0, found);
            if (found.empty())
                return reason(R::NoTarget);
        }
        resolved = p.id;
        return 0;
    }
    if (target == 0 || target == p.id)
    {
        resolved = p.id;
        return 0;
    }
    if (!knows(p, target))
        return reason(R::NoTarget);
    if (monsterById(target))
        return reason(R::WrongTargetType);
    Player* t = findById(target);
    if (!t)
        return reason(R::NoTarget);
    if (t->combat.dead)
        return reason(R::TargetDead);
    if (!vg::inRange(dist(p.move.x, p.move.z, t->move.x, t->move.z), rank.range))
        return reason(R::OutOfRange);
    resolved = target;
    return 0;
}

void Zone::completeCast(Player& p)
{
    auto& c = p.combat;
    const std::uint16_t skillId = c.castSkill;
    const EntityId target = c.castTarget;
    c.castSkill = 0;
    const vg::SkillDef* skill = vg::findSkill(skillId);
    EntityId resolved = 0;
    std::uint8_t reason = skill ? checkSkillTarget(p, *skill, target, resolved)
                                : static_cast<std::uint8_t>(vn::ActionFailReason::UnknownSkill);
    if (reason == 0 && rankOf(p, *skill) == 0)
        reason = static_cast<std::uint8_t>(vn::ActionFailReason::NotLearned);
    if (reason == 0 && c.mp < skill->at(rankOf(p, *skill)).mpCost)
        reason = static_cast<std::uint8_t>(vn::ActionFailReason::NotEnoughMp);
    const EntityId other = target == p.id ? 0 : target;
    if (reason != 0)
    {
        sendAround(vn::CastEnd{p.id, skillId, vn::kCastInterrupted}, p.id, other);
        actionFail(p, vn::CombatAction::Skill, skillId, static_cast<vn::ActionFailReason>(reason));
        return;
    }
    sendAround(vn::CastEnd{p.id, skillId, vn::kCastCompleted}, p.id, other);
    executeSkill(p, *skill, resolved);
}
void Zone::trySwing(Player& p)
{
    auto& c = p.combat;
    Monster* m = monsterById(c.attackTarget);
    if (!m || m->state == Monster::State::Dead || m->state == Monster::State::Gone || !knows(p, m->id))
    {
        c.attackTarget = 0; // §3.1: ends on target death / despawn
        return;
    }
    if (c.castSkill != 0)
        return;
    if (!vg::inRange(dist(p.move.x, p.move.z, m->x, m->z), c.stats.attackRange))
    {
        if (!c.rangeFailSent)
        {
            c.rangeFailSent = true; // once per ATTACK_REQ; resumes when back in range
            actionFail(p, vn::CombatAction::Attack, 0, vn::ActionFailReason::OutOfRange);
        }
        return;
    }
    if (inSafeZone(p.move.x, p.move.z) || inSafeZone(m->x, m->z))
    {
        if (!c.safeFailSent)
        {
            c.safeFailSent = true;
            actionFail(p, vn::CombatAction::Attack, 0, vn::ActionFailReason::InSafeZone);
        }
        return;
    }
    if (m->state == Monster::State::Return || m_now < c.nextSwingAt || m_now < c.gcdUntil)
        return;
    c.nextSwingAt = m_now + msDuration(c.stats.attackIntervalMs);
    c.gcdUntil = m_now + vg::kGlobalCooldown;
    standUp(p);
    const vg::Hit hit = vg::rollDamage(c.stats.attack, 1.0f, m->tmpl->defense, c.stats.critChance, m_rng);
    damageMonster(p, *m, 0, hit);
}

void Zone::updatePlayerCombat(Player& p)
{
    auto& c = p.combat;
    if (c.dead)
        return;
    if (!c.effects.empty())
    {
        updatePlayerEffects(p);
        if (c.dead)
            return;
    }
    if (c.castSkill != 0 && m_now >= c.castEndsAt)
        completeCast(p);
    if (c.attackTarget != 0)
        trySwing(p);
    if (c.dead || m_now < c.nextRegenAt)
        return;
    c.nextRegenAt = m_now + kRegenStep;
    if (inCombat(p) || (c.hp >= c.stats.maxHp && c.mp >= c.stats.maxMp))
    {
        c.hpRegen = c.mpRegen = 0.0f;
        return;
    }
    // §1.1: out of combat 2 % max_hp and 3 % max_mp per second (PROTOCOL_STATS.md §2: times
    // 1 + VIT / 100 and 1 + SPI / 100), doubled while sitting.
    c.hpRegen += vg::hpRegenPerSecond(c.stats, c.sitting);
    c.mpRegen += vg::mpRegenPerSecond(c.stats, c.sitting);
    const auto hpGain = static_cast<std::uint32_t>(c.hpRegen);
    const auto mpGain = static_cast<std::uint32_t>(c.mpRegen);
    c.hpRegen -= static_cast<float>(hpGain);
    c.mpRegen -= static_cast<float>(mpGain);
    const std::uint32_t hp = std::min(c.stats.maxHp, c.hp + hpGain);
    const std::uint32_t mp = std::min(c.stats.maxMp, c.mp + mpGain);
    if (hp != c.hp || mp != c.mp)
    {
        if (hp != c.hp)
            c.entityStatsDirty = true;
        c.hp = hp;
        c.mp = mp;
        c.selfStatsDirty = true;
        p.saveDirty = true;
    }
}

void Zone::flushCombatState()
{
    for (auto& [id, owned] : m_players)
    {
        Player& p = *owned;
        auto& c = p.combat;
        const std::uint8_t st = stateOf(p);
        if (st != c.sentState)
        {
            c.sentState = st;
            sendAround(vn::EntityState{p.id, st}, p.id);
        }
        if (c.selfStatsDirty && m_now - c.lastSelfStats >= kSelfStatsInterval)
        {
            c.selfStatsDirty = false;
            c.lastSelfStats = m_now;
            p.out.appendMessage(vn::SelfStats{c.hp, c.stats.maxHp, c.mp, c.stats.maxMp, c.stats.attack, c.stats.defense},
                                m_scratch);
        }
        if (c.charStatsDirty)
        {
            c.charStatsDirty = false;
            sendCharStats(p); // at most one per tick (PROTOCOL_STATS.md §3.2)
        }
        if (c.entityStatsDirty && m_now - c.lastEntityStats >= kEntityStatsInterval)
        {
            c.entityStatsDirty = false;
            c.lastEntityStats = m_now;
            sendAround(vn::EntityStats{p.id, c.hp, c.stats.maxHp, p.character.level}, p.id);
        }
    }
    for (auto& [id, owned] : m_monsters)
    {
        Monster& m = *owned;
        if (!m.inGrid)
            continue;
        const std::uint8_t st = stateOf(m);
        if (st != m.sentState)
        {
            m.sentState = st;
            sendAround(vn::EntityState{m.id, st}, m.id);
        }
        if (m.statsDirty && m_now - m.lastStats >= kEntityStatsInterval)
        {
            m.statsDirty = false;
            m.lastStats = m_now;
            sendAround(vn::EntityStats{m.id, m.hp, m.tmpl->hp, m.tmpl->level}, m.id);
        }
    }
}

// ---- Requests (§3) ----

void Zone::combat(const CombatCommand& cmd)
{
    using K = CombatCommand::Kind;
    using R = vn::ActionFailReason;
    Player* pp = find(cmd.session);
    if (!pp || pp->loading) // PROTOCOL_ZONES.md §3.4: nothing while loading a zone
        return;
    Player& p = *pp;
    auto& c = p.combat;
    switch (cmd.kind)
    {
    case K::Target: c.selected = cmd.target; return;
    case K::AttackStop: c.attackTarget = 0; return;
    case K::Attack:
    {
        const auto fail = [&](R r) { actionFail(p, vn::CombatAction::Attack, 0, r); };
        if (c.dead)
            return fail(R::YouAreDead);
        standUp(p);
        if (cmd.target == 0 || !knows(p, cmd.target))
            return fail(R::NoTarget);
        if (findById(cmd.target))
            return fail(R::NotAttackable); // §1: no PvP in M9
        const Monster* m = monsterById(cmd.target);
        if (!m)
            return fail(R::NoTarget);
        if (m->state == Monster::State::Dead || m->state == Monster::State::Gone)
            return fail(R::TargetDead);
        // Range and the safe zone are checked at every swing (§3.1); a new ATTACK_REQ re-arms the
        // one-time OutOfRange / InSafeZone notice but never shortens the swing interval.
        c.attackTarget = cmd.target;
        c.rangeFailSent = false;
        c.safeFailSent = false;
        return;
    }
    case K::Skill:
    {
        const auto fail = [&](R r) { actionFail(p, vn::CombatAction::Skill, cmd.skill, r); };
        const vg::SkillDef* skill = vg::findSkill(cmd.skill);
        if (!skill || skill->charClass != p.character.charClass)
            return fail(R::UnknownSkill);
        const std::uint8_t rank = rankOf(p, *skill);
        if (rank == 0)
            return fail(R::NotLearned); // PROTOCOL_SKILLS.md §1.8
        const vg::SkillRankData& r = skill->at(rank);
        if (c.dead)
            return fail(R::YouAreDead);
        standUp(p);
        if (c.castSkill != 0)
            return fail(R::Busy);
        if (m_now < c.gcdUntil || m_now < c.skillReadyAt[skill->id])
            return fail(R::OnCooldown);
        if (c.mp < r.mpCost)
            return fail(R::NotEnoughMp);
        EntityId resolved = 0;
        if (const std::uint8_t reason = checkSkillTarget(p, *skill, cmd.target, resolved); reason != 0)
            return fail(static_cast<R>(reason));
        if (r.castMs == 0)
        {
            executeSkill(p, *skill, resolved);
            return;
        }
        c.castSkill = skill->id;
        c.castTarget = resolved;
        c.castEndsAt = m_now + msDuration(r.castMs);
        c.gcdUntil = m_now + vg::kGlobalCooldown;
        sendAround(vn::CastStart{p.id, skill->id, resolved, static_cast<std::uint16_t>(r.castMs)}, p.id,
                   resolved == p.id ? 0 : resolved);
        return;
    }
    case K::Respawn:
    {
        const auto answer = [&](vn::RespawnResult r) {
            p.out.appendMessage(vn::RespawnRes{r, p.move.x, p.move.y, p.move.z, p.move.yaw, c.hp, c.mp}, m_scratch);
        };
        if (!c.dead)
            return answer(vn::RespawnResult::NotDead);
        if (m_now - c.diedAt < vg::kRespawnMinDelay)
            return answer(vn::RespawnResult::TooEarly);
        respawnPlayer(p);
        return;
    }
    case K::Stance:
    {
        if (cmd.stance == vn::kStanceSit)
        {
            if (c.dead || (p.move.flags & game::kMoveMoving) != 0 || c.castSkill != 0 || inCombat(p))
                return actionFail(p, vn::CombatAction::Stance, 0, R::Busy);
            c.sitting = true;
        }
        else
        {
            c.sitting = false;
        }
        return;
    }
    }
}

} // namespace vo::server::world
