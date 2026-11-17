#include "server/world/Zone.h"

#include "shared/core/Assert.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Movement.h"
#include "shared/game/TerrainWalk.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace vo::server::world {

namespace {

auth::CharacterPosition toPosition(std::uint16_t zone, const assets::SpawnPoint& s, const assets::TerrainData& t)
{
    return {zone, s.position.x, t.heightAt(s.position.x, s.position.z), s.position.z, game::normalizeYaw(s.yaw)};
}

} // namespace

std::unique_ptr<Zone> Zone::create(std::uint16_t id, assets::TerrainData terrain, ZoneSettings settings,
                                   std::string& error, ZoneLayout layout)
{
    if (!(terrain.worldSize > 0.0f) || terrain.resolution < 2 || terrain.maskResolution == 0 ||
        terrain.walkMask.size() != static_cast<std::size_t>(terrain.maskResolution) * terrain.maskResolution)
    {
        error = "terrain has no usable heightmap or walk mask";
        return nullptr;
    }
    if ((layout.raceMask & game::kAllRaces) == 0)
    {
        error = "the zone allows no race";
        return nullptr;
    }
    const auto walkableMarker = [&](std::string_view name, std::string_view what) {
        const assets::SpawnPoint* s = terrain.findSpawn(name);
        if (!s)
        {
            error = std::format("terrain has no {} '{}'", what, name);
            return false;
        }
        if (!std::isfinite(s->position.x) || !std::isfinite(s->position.z) ||
            !terrain.isWalkable(s->position.x, s->position.z))
        {
            error = std::format("{} '{}' ({:.1f}, {:.1f}) is not walkable", what, name, s->position.x, s->position.z);
            return false;
        }
        return true;
    };
    for (const auto race : {game::Race::Aydor, game::Race::Kargan})
    {
        const auto r = static_cast<std::uint8_t>(race);
        if ((layout.raceMask & game::raceBit(r)) != 0 && !walkableMarker(game::raceSpawnMarker(r), "spawn point"))
            return nullptr;
    }
    for (const ZoneLayout::Gate& g : layout.gates)
    {
        if (!walkableMarker(g.def.marker, "gate marker"))
            return nullptr;
        if (!(g.def.radius >= game::kMinGateRadius && g.def.radius <= game::kMaxGateRadius))
        {
            error = std::format("gate '{}' has an invalid radius {}", g.def.marker, g.def.radius);
            return nullptr;
        }
    }
    if (settings.rules.bankSeconds <= 0.0f || settings.rules.walkSpeed <= 0.0f || settings.rules.runSpeed <= 0.0f)
    {
        error = "invalid movement rules";
        return nullptr;
    }
    return std::make_unique<Zone>(PrivateTag{}, id, std::move(terrain), std::move(settings), std::move(layout));
}

std::unique_ptr<Zone> Zone::load(std::uint16_t id, const std::filesystem::path& file, ZoneSettings settings,
                                 std::string& error, ZoneLayout layout)
{
    assets::TerrainData terrain;
    if (!assets::loadTerrain(file, terrain, error))
        return nullptr;
    return create(id, std::move(terrain), std::move(settings), error, std::move(layout));
}

Zone::Zone(PrivateTag, std::uint16_t id, assets::TerrainData terrain, ZoneSettings settings, ZoneLayout layout)
    : m_id(id), m_terrain(std::move(terrain)), m_settings(std::move(settings)),
      m_grid(m_terrain.worldSize, game::kAoiCellSize, game::kAoiViewRadiusCells), m_layout(std::move(layout)),
      m_rng(m_settings.seed != 0 ? m_settings.seed + m_id - 1 : std::random_device{}())
{
    const assets::SpawnPoint* aydor = m_terrain.findSpawn(kSpawnAydor);
    const assets::SpawnPoint* kargan = m_terrain.findSpawn(kSpawnKargan);
    // create() made sure the spawns of the allowed races exist; a race that may not be here uses the other.
    if (!aydor || !allowsRace(static_cast<std::uint8_t>(game::Race::Aydor)))
        aydor = kargan && allowsRace(static_cast<std::uint8_t>(game::Race::Kargan)) ? kargan : aydor;
    if (!kargan || !allowsRace(static_cast<std::uint8_t>(game::Race::Kargan)))
        kargan = aydor && allowsRace(static_cast<std::uint8_t>(game::Race::Aydor)) ? aydor : kargan;
    VO_VERIFY(aydor != nullptr && kargan != nullptr);
    m_spawnAydor = toPosition(m_id, *aydor, m_terrain);
    m_spawnKargan = toPosition(m_id, *kargan, m_terrain);
    for (const ZoneLayout::Gate& g : m_layout.gates)
    {
        const assets::SpawnPoint* s = m_terrain.findSpawn(g.def.marker);
        m_gatePoints.push_back({s->position.x, s->position.z, g.def.radius});
    }
}

auth::CharacterPosition Zone::raceSpawn(std::uint8_t race) const noexcept
{
    return race == static_cast<std::uint8_t>(game::Race::Kargan) ? m_spawnKargan : m_spawnAydor;
}

bool Zone::allowsRace(std::uint8_t race) const noexcept
{
    const std::uint8_t bit = game::raceBit(race);
    return bit != 0 && (m_layout.raceMask & bit) != 0;
}

std::optional<auth::CharacterPosition> Zone::marker(std::string_view name) const
{
    const assets::SpawnPoint* s = m_terrain.findSpawn(name);
    if (!s)
        return std::nullopt;
    return toPosition(m_id, *s, m_terrain);
}

std::size_t Zone::gateAt(float x, float z) const noexcept
{
    for (std::size_t i = 0; i < m_gatePoints.size(); ++i)
    {
        const GatePoint& g = m_gatePoints[i];
        if (std::hypot(x - g.x, z - g.z) <= g.radius)
            return i + 1;
    }
    return 0;
}

std::optional<auth::CharacterPosition> Zone::arrivalPosition(std::string_view markerName)
{
    auto pos = marker(markerName);
    if (!pos)
        return std::nullopt;
    spreadAround(pos->x, pos->z);
    pos->y = m_terrain.heightAt(pos->x, pos->z);
    return pos;
}

Zone::Player* Zone::find(SessionKey session)
{
    const auto it = m_bySession.find(session);
    return it == m_bySession.end() ? nullptr : findById(it->second);
}

Zone::Player* Zone::findById(EntityId id)
{
    const auto it = m_players.find(id);
    return it == m_players.end() ? nullptr : it->second.get();
}

const Zone::Player* Zone::findPlayer(SessionKey session) const
{
    return const_cast<Zone*>(this)->find(session);
}

const Zone::Player* Zone::findEntity(EntityId id) const
{
    return const_cast<Zone*>(this)->findById(id);
}

Zone::Monster* Zone::monsterById(EntityId id)
{
    const auto it = m_monsters.find(id);
    return it == m_monsters.end() ? nullptr : it->second.get();
}

const Zone::Monster* Zone::findMonster(EntityId id) const
{
    return const_cast<Zone*>(this)->monsterById(id);
}

bool Zone::hasEntity(EntityId id) const noexcept
{
    return m_players.contains(id) || m_monsters.contains(id) || m_loot.contains(id) || m_npcs.contains(id);
}

std::optional<auth::CharacterPosition> Zone::positionOf(SessionKey session) const
{
    const Player* p = findPlayer(session);
    if (!p)
        return std::nullopt;
    return auth::CharacterPosition{m_id, p->move.x, p->move.y, p->move.z, p->move.yaw};
}

bool Zone::placeable(float x, float z) const
{
    return std::isfinite(x) && std::isfinite(z) && m_terrain.isWalkable(x, z);
}

void Zone::spreadAround(float& x, float& z)
{
    if (!(m_settings.spawnSpread > 0.5f))
        return;
    std::uniform_real_distribution<float> angle(0.0f, 2.0f * std::numbers::pi_v<float>);
    std::uniform_real_distribution<float> radius(0.5f, m_settings.spawnSpread);
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const float a = angle(m_rng);
        const float r = radius(m_rng);
        const float cx = x + r * std::sin(a);
        const float cz = z + r * std::cos(a);
        // Never into a gate trigger (PROTOCOL_ZONES.md §3.3: arrivals and spawns stay outside).
        if (game::isSegmentWalkable(m_terrain, x, z, cx, cz) && gateAt(cx, cz) == 0)
        {
            x = cx;
            z = cz;
            return;
        }
    }
}

// ---- Enter / leave ----

void Zone::enter(EnterParams params, core::MonoTime now, std::uint32_t nowMs)
{
    VO_ASSERT(!m_bySession.contains(params.session) && !m_players.contains(params.entityId));
    auto owned = std::make_unique<Player>();
    Player& p = *owned;
    p.id = params.entityId;
    p.session = params.session;
    p.generation = params.generation;
    p.link = std::move(params.link);
    p.account = params.account;
    p.character = std::move(params.character);
    bool leftDead = false;
    initCombat(p, p.character, params.equip, leftDead);
    if (params.zoneChange && !params.effects.empty())
    {
        // PROTOCOL_ZONES.md §2.3: effects come along with their remaining time; the player's own
        // ones are re-sourced to its new entity, other players' ones count for nobody.
        for (ActiveEffect e : params.effects)
        {
            if (e.endsAt <= now || p.combat.effects.size() >= net::kMaxEffectEntries)
                continue;
            e.source = e.source == params.previousEntityId ? p.id : 0;
            p.combat.effects.push_back(e);
        }
        p.combat.stats = game::applyEffectModifiers(p.combat.baseStats, modifiersOf(p));
        // initCombat clamped to the maxima without effects (e.g. a max hp buff): restore the carried values.
        if (p.character.hp)
            p.combat.hp = std::clamp<std::uint32_t>(*p.character.hp, 1, p.combat.stats.maxHp);
        if (p.character.mp)
            p.combat.mp = std::min(*p.character.mp, p.combat.stats.maxMp);
    }
    if (const std::uint32_t pct = m_settings.testingEnterVitalsPercent; pct > 0 && !params.zoneChange && !p.combat.dead)
    {
        p.combat.hp = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(
                                                     static_cast<std::uint64_t>(p.combat.stats.maxHp) * pct / 100));
        p.combat.mp = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p.combat.stats.maxMp) * pct / 100);
    }
    // PROTOCOL_SKILLS.md §1.7: cooldowns still running from a recent leave continue.
    net::SkillCooldowns running;
    for (const RememberedCooldown& rc : params.cooldowns)
    {
        if (rc.skill == 0 && rc.readyAt > now && rc.totalMs > 0)
        {
            // PROTOCOL_SHOP.md §2.3: the shared consumable cooldown continues too.
            p.combat.potionReadyAt = rc.readyAt;
            p.combat.potionCooldownMs = rc.totalMs;
            continue;
        }
        if (rc.skill == 0 || rc.skill > game::kMaxSkillId || rc.readyAt <= now || rc.totalMs == 0)
            continue;
        p.combat.skillReadyAt[rc.skill] = rc.readyAt;
        p.combat.skillCooldownMs[rc.skill] = rc.totalMs;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(rc.readyAt - now).count();
        const auto remaining = static_cast<std::uint32_t>(std::clamp<long long>(left, 1, rc.totalMs));
        if (running.entries.size() < net::kMaxSkillCooldownEntries)
            running.entries.push_back({rc.skill, remaining, rc.totalMs});
    }

    const auth::CharacterPosition saved = p.character.position;
    const auth::CharacterPosition spawn = raceSpawn(p.character.race);
    float x = saved.x;
    float z = saved.z;
    float yaw = saved.yaw;
    bool atSpawn = false;
    if (leftDead || saved.zoneId != m_id || !placeable(x, z))
    {
        x = spawn.x;
        z = spawn.z;
        yaw = spawn.yaw;
        atSpawn = true;
    }
    else if (x == spawn.x && z == spawn.z)
    {
        // Exactly the spawn point: a new character (created there), never moved. Spread like a
        // fresh spawn. A player who merely walked back near the spawn keeps its position.
        atSpawn = true;
    }
    if (atSpawn)
        spreadAround(x, z);

    p.move.x = x;
    p.move.y = m_terrain.heightAt(x, z);
    p.move.z = z;
    p.move.yaw = game::normalizeYaw(yaw);
    p.move.flags = 0;
    p.move.lastTime = now;
    p.move.credit = 0.0f;
    p.moveTimeMs = nowMs;
    p.cell = m_grid.cellOf(x, z);
    if (m_settings.savePeriodTicks > 0)
        p.saveSlot = static_cast<std::uint32_t>((static_cast<std::uint64_t>(p.character.id) * 2654435761ull) %
                                                m_settings.savePeriodTicks);
    p.character.position = {m_id, p.move.x, p.move.y, p.move.z, p.move.yaw};

    p.out.generation = p.generation;
    EnteredInfo info;
    info.entityId = p.id;
    info.zoneId = m_id;
    info.x = p.move.x;
    info.y = p.move.y;
    info.z = p.move.z;
    info.yaw = p.move.yaw;
    info.walkSpeed = m_settings.rules.walkSpeed;
    info.runSpeed = m_settings.rules.runSpeed;
    info.serverTimeMs = nowMs;
    const auto& c = p.combat;
    info.level = p.character.level;
    info.xp = c.xp;
    info.xpToNext = game::xpToNext(p.character.level);
    info.hp = c.hp;
    info.maxHp = c.stats.maxHp;
    info.mp = c.mp;
    info.maxMp = c.stats.maxMp;
    info.attack = c.stats.attack;
    info.defense = c.stats.defense;
    info.dead = c.dead;
    info.zoneChange = params.zoneChange;
    info.zoneChangeResult = params.zoneChangeResult;
    info.fromZone = params.fromZone;
    p.out.entered = info;
    p.out.level = p.character.level; // the session's item requirements follow the live level
    p.out.dead = c.dead;
    p.combat.sentState = stateOf(p);
    // PROTOCOL_STATS.md §3.2: CHAR_STATS is the first world packet (after the session's
    // ENTER_WORLD_RES, INVENTORY and COINS), before any ENTITY_SPAWN.
    sendCharStats(p);
    if (!running.entries.empty())
        p.out.appendMessage(running, m_scratch); // PROTOCOL_SKILLS.md §4: after CHAR_STATS
    if (c.potionReadyAt > now)
        sendItemCooldown(p, now); // PROTOCOL_SHOP.md §3.4
    if (!c.effects.empty())
        sendEffectList(p, p.id, c.effects); // PROTOCOL_ZONES.md §3.3: the player's own effects

    // Gates (PROTOCOL_ZONES.md §2.1, §3.4).
    p.loading = params.zoneChange;
    p.loadingUntil = now + std::chrono::milliseconds(game::kZoneLoadingTimeoutMs);
    p.gateArmedAt = now + std::chrono::milliseconds(game::kGateRearmMs);
    p.inGate = gateAt(p.move.x, p.move.z);
    p.beenOutsideGates = p.inGate == 0;

    m_players.emplace(p.id, std::move(owned));
    m_bySession.emplace(p.session, p.id);
    m_grid.forEachInView(p.cell, [&](EntityId other) {
        if (Player* o = findById(other); o && o != &p)
            spawnPair(p, *o);
        else if (Monster* m = monsterById(other))
            monsterSeen(p, *m);
        else if (Loot* l = lootById(other))
            lootSeen(p, *l);
        else if (Npc* n = npcById(other))
            npcSeen(p, *n);
    });
    m_grid.add(p.id, p.cell);
}

std::optional<Zone::LeftPlayer> Zone::leave(SessionKey session)
{
    Player* p = find(session);
    if (!p)
        return std::nullopt;
    closeLootView(*p, net::LootClosedReason::Gone, false); // leaving ends a view without a packet
    closeShop(*p, net::ShopClosedReason::OutOfRange, false); // and a shop (PROTOCOL_SHOP.md §2.2)
    for (const EntityId other : p->known)
    {
        if (Monster* m = monsterById(other))
        {
            m->observers.erase(p->id);
            continue;
        }
        if (Loot* l = lootById(other))
        {
            l->observers.erase(p->id);
            continue;
        }
        if (Npc* n = npcById(other))
        {
            n->observers.erase(p->id);
            continue;
        }
        Player* o = findById(other);
        VO_ASSERT(o != nullptr);
        if (o && o->known.erase(p->id) > 0)
            sendDespawn(*o, p->id);
    }
    m_grid.remove(p->id, p->cell);
    LeftPlayer left{p->account, p->character.id, {m_id, p->move.x, p->move.y, p->move.z, p->move.yaw}, progressOf(*p), {}};
    for (std::uint16_t s = 1; s <= game::kMaxSkillId; ++s)
        if (p->combat.skillReadyAt[s] > m_now && p->combat.skillCooldownMs[s] > 0)
            left.cooldowns.push_back({s, p->combat.skillReadyAt[s], p->combat.skillCooldownMs[s]});
    if (p->combat.potionReadyAt > m_now && p->combat.potionCooldownMs > 0)
        left.cooldowns.push_back({0, p->combat.potionReadyAt, p->combat.potionCooldownMs}); // PROTOCOL_SHOP.md §2.3
    const EntityId id = p->id;
    m_bySession.erase(session);
    m_players.erase(id); // drops its pending packets
    return left;
}

std::optional<Zone::Departure> Zone::depart(SessionKey session)
{
    Player* p = find(session);
    if (!p)
        return std::nullopt;
    Departure d;
    d.entityId = p->id;
    d.generation = p->generation;
    d.link = p->link;
    d.character = p->character;
    const auto& c = p->combat;
    d.character.xp = c.xp;
    d.character.hp = c.hp;
    d.character.mp = c.mp;
    d.character.statPoints = c.allocated;
    d.character.skillRanks = c.skillRanks;
    d.equip = c.equip;
    d.effects = c.effects;
    d.pending = std::move(p->out);
    p->out = WorldBatch{};
    auto left = leave(session);
    VO_ASSERT(left.has_value());
    d.left = std::move(*left);
    d.character.position = d.left.position;
    return d;
}

void Zone::setReady(SessionKey session)
{
    Player* p = find(session);
    if (!p || !p->loading)
        return;
    p->loading = false;
    p->gateArmedAt = m_now + std::chrono::milliseconds(game::kGateRearmMs);
}

void Zone::updateGates()
{
    for (auto& [id, owned] : m_players)
    {
        Player& p = *owned;
        if (p.loading)
        {
            if (m_now < p.loadingUntil)
                continue;
            p.loading = false; // §3.4: the protection ends after 20 s without ZONE_LOADED_REQ
            p.gateArmedAt = m_now + std::chrono::milliseconds(game::kGateRearmMs);
        }
        if (m_gatePoints.empty())
            continue;
        const std::size_t at = gateAt(p.move.x, p.move.z);
        if (at == 0)
        {
            p.inGate = 0;
            p.beenOutsideGates = true;
            p.refusedThisStay = false;
            continue;
        }
        if (at != p.inGate)
        {
            p.inGate = at;
            p.refusedThisStay = false;
        }
        const ZoneLayout::Gate& gate = m_layout.gates[at - 1];
        game::GateState state;
        state.alive = !p.combat.dead;
        state.loading = false;
        state.armedByTime = m_now >= p.gateArmedAt;
        state.beenOutside = p.beenOutsideGates;
        switch (game::checkGate(gate.targetRaceMask, p.character.race, state))
        {
        case game::GateCheck::Pass:
            m_gateTriggers.push_back({p.session, at - 1});
            ++m_counters.gateTriggers;
            break;
        case game::GateCheck::RaceForbidden:
            // §2.2: once per stay inside, at least 3 s apart.
            if (!p.refusedThisStay && (p.lastRefusalAt == core::MonoTime{} ||
                                       m_now - p.lastRefusalAt >= std::chrono::milliseconds(game::kGateRefusalIntervalMs)))
            {
                p.refusedThisStay = true;
                p.lastRefusalAt = m_now;
                p.out.appendMessage(net::ZoneGateRefused{gate.def.toZone, net::GateRefusal::RaceForbidden}, m_scratch);
                ++m_counters.gateRefusals;
            }
            break;
        default: break;
        }
    }
}

// ---- Movement ----

std::optional<MoveVerdict> Zone::move(SessionKey session, const net::MoveReq& req, core::MonoTime at,
                                      std::uint32_t atMs)
{
    Player* p = find(session);
    if (!p)
        return std::nullopt;
    if (p->loading)
        return MoveVerdict::Ignored; // PROTOCOL_ZONES.md §3.4: sent before the client knew it changed zone
    p->lastSeq = req.seq;
    if (p->combat.dead)
    {
        // PROTOCOL_COMBAT.md §2: a dead player cannot move.
        ++m_counters.movesRejected;
        p->correctionPending = true;
        return MoveVerdict::Dead;
    }
    const std::uint8_t oldFlags = p->move.flags;
    const float oldX = p->move.x, oldZ = p->move.z;
    const MoveVerdict v = validateMove(m_terrain, m_settings.rules, p->move, req, at);
    if (v == MoveVerdict::Accepted)
    {
        ++m_counters.movesAccepted;
        p->relayDirty = true;
        p->saveDirty = true;
        p->moveTimeMs = atMs;
        const bool movingBit = (p->move.flags & game::kMoveMoving) != 0;
        if (movingBit || std::hypot(p->move.x - oldX, p->move.z - oldZ) > 0.05f)
            interruptCast(*p, true); // §1.2: moving interrupts a cast
        if (movingBit)
            standUp(*p); // §3.6
        const AoiCell cell = m_grid.cellOf(p->move.x, p->move.z);
        if (cell != p->cell)
        {
            const AoiCell from = p->cell;
            m_grid.move(p->id, from, cell);
            p->cell = cell;
            applyCellChange(*p, from, cell);
        }
    }
    else
    {
        ++m_counters.movesRejected;
        VO_LOG_TRACE("zone {}: entity {} move seq {} rejected ({})", m_id, p->id, req.seq, moveVerdictName(v));
        p->correctionPending = true;
        if (p->move.flags != oldFlags)
        {
            // The player now stands at the corrected position: observers must see it stop there.
            p->relayDirty = true;
            p->moveTimeMs = atMs;
        }
    }
    return v;
}

void Zone::setEquipment(SessionKey session, std::uint16_t mainHandTemplate, std::uint16_t offHandTemplate,
                        game::EquipBonus equip)
{
    Player* p = find(session);
    if (!p)
        return;
    if (!(p->combat.equip == equip))
    {
        // PROTOCOL_COMBAT.md §1.1: recompute at once, SELF_STATS (and PROTOCOL_STATS.md §3.2:
        // CHAR_STATS) to the owner.
        p->combat.equip = equip;
        recomputeStats(*p);
        p->combat.selfStatsDirty = true;
        p->combat.charStatsDirty = true;
    }
    if (p->character.mainHandTemplate == mainHandTemplate && p->character.offHandTemplate == offHandTemplate)
        return;
    p->character.mainHandTemplate = mainHandTemplate;
    p->character.offHandTemplate = offHandTemplate;
    const net::EntityEquip msg{p->id, mainHandTemplate, offHandTemplate};
    m_scratch.clear();
    msg.write(m_scratch);
    const auto payload = m_scratch.data();
    const auto op = static_cast<std::uint16_t>(net::Opcode::EntityEquip);
    p->out.append(op, payload);
    ++m_counters.equips;
    for (const EntityId other : p->known)
    {
        if (Player* o = findById(other))
        {
            o->out.append(op, payload);
            ++m_counters.equips;
        }
    }
}

void Zone::placeEntity(SessionKey session, float x, float z)
{
    if (Player* p = find(session))
        movePlayerTo(*p, x, z);
}

void Zone::movePlayerTo(Player& p, float x, float z)
{
    p.move.x = x;
    p.move.z = z;
    p.move.y = m_terrain.heightAt(x, z);
    p.relayDirty = true;
    p.saveDirty = true;
    const AoiCell cell = m_grid.cellOf(x, z);
    if (cell != p.cell)
    {
        const AoiCell from = p.cell;
        m_grid.move(p.id, from, cell);
        p.cell = cell;
        applyCellChange(p, from, cell);
    }
}

// ---- Area of interest ----

void Zone::applyCellChange(Player& p, AoiCell from, AoiCell to)
{
    m_grid.forEachViewChange(
        from, to,
        [&](EntityId other) {
            if (Player* o = findById(other); o && o != &p)
                despawnPair(p, *o);
            else if (Monster* m = monsterById(other))
                monsterUnseen(p, *m);
            else if (Loot* l = lootById(other))
                lootUnseen(p, *l);
            else if (Npc* n = npcById(other))
                npcUnseen(p, *n);
        },
        [&](EntityId other) {
            if (Player* o = findById(other); o && o != &p)
                spawnPair(p, *o);
            else if (Monster* m = monsterById(other))
                monsterSeen(p, *m);
            else if (Loot* l = lootById(other))
                lootSeen(p, *l);
            else if (Npc* n = npcById(other))
                npcSeen(p, *n);
        });
}

void Zone::spawnPair(Player& a, Player& b)
{
    if (a.known.insert(b.id).second)
        sendSpawn(a, b);
    if (b.known.insert(a.id).second)
        sendSpawn(b, a);
}

void Zone::despawnPair(Player& a, Player& b)
{
    if (a.known.erase(b.id) > 0)
        sendDespawn(a, b.id);
    if (b.known.erase(a.id) > 0)
        sendDespawn(b, a.id);
}

void Zone::sendSpawn(Player& to, const Player& about)
{
    net::EntitySpawn s;
    s.entityId = about.id;
    s.entityType = net::kEntityTypePlayer;
    s.name = about.character.name;
    s.race = about.character.race;
    s.charClass = about.character.charClass;
    s.face = about.character.face;
    s.hair = about.character.hair;
    s.level = about.character.level;
    s.x = about.move.x;
    s.y = about.move.y;
    s.z = about.move.z;
    s.yaw = about.move.yaw;
    s.flags = about.move.flags;
    s.mainHandTemplate = about.character.mainHandTemplate;
    s.offHandTemplate = about.character.offHandTemplate;
    s.hp = about.combat.hp;
    s.maxHp = about.combat.stats.maxHp;
    s.templateId = 0;
    s.state = about.combat.sentState;
    to.out.appendMessage(s, m_scratch);
    ++m_counters.spawns;
    if (!about.combat.effects.empty())
        sendEffectList(to, about.id, about.combat.effects); // PROTOCOL_SKILLS.md §3.7
}

void Zone::sendDespawn(Player& to, EntityId about)
{
    to.out.appendMessage(net::EntityDespawn{about}, m_scratch);
    ++m_counters.despawns;
}

// ---- Per tick ----

void Zone::relay(Player& p, core::MonoTime logicalNow)
{
    if (p.everRelayed && logicalNow - p.lastRelay < m_settings.relayInterval)
        return; // stays dirty: the latest state goes out at the next allowed tick
    p.relayDirty = false;
    const auto& m = p.move;
    if (p.everRelayed && m.x == p.relayedX && m.y == p.relayedY && m.z == p.relayedZ && m.yaw == p.relayedYaw &&
        m.flags == p.relayedFlags)
        return; // nothing new (e.g. a repeated stop packet)
    p.everRelayed = true;
    p.lastRelay = logicalNow;
    p.relayedX = m.x;
    p.relayedY = m.y;
    p.relayedZ = m.z;
    p.relayedYaw = m.yaw;
    p.relayedFlags = m.flags;
    if (p.known.empty())
        return;
    net::EntityMove msg;
    msg.entityId = p.id;
    msg.flags = m.flags;
    msg.x = m.x;
    msg.y = m.y;
    msg.z = m.z;
    msg.yaw = m.yaw;
    msg.serverTimeMs = p.moveTimeMs;
    m_scratch.clear();
    msg.write(m_scratch);
    const auto payload = m_scratch.data();
    for (const EntityId other : p.known)
    {
        if (Player* o = findById(other))
        {
            o->out.append(static_cast<std::uint16_t>(net::Opcode::EntityMove), payload);
            ++m_counters.relays;
        }
    }
}

void Zone::update(core::MonoTime logicalNow, std::uint64_t tickIndex, const SavePositionFn& save)
{
    if (m_now == core::MonoTime{} || logicalNow > m_now)
    {
        // Without beginTick() (unit tests): keep server_time_ms running with the scheduled time.
        if (m_now != core::MonoTime{})
            m_nowMs += static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(logicalNow - m_now).count());
        m_now = logicalNow;
    }
    if (m_lastTick != core::MonoTime{})
        m_dt = std::clamp(static_cast<float>(core::toSeconds(m_now - m_lastTick)), 0.0f, 0.25f);
    m_lastTick = m_now;
    for (auto& [id, owned] : m_players)
    {
        Player& p = *owned;
        if (p.correctionPending)
        {
            p.correctionPending = false;
            net::PositionCorrect pc;
            pc.seq = p.lastSeq;
            pc.x = p.move.x;
            pc.y = p.move.y;
            pc.z = p.move.z;
            pc.yaw = p.move.yaw;
            p.out.appendMessage(pc, m_scratch);
            ++m_counters.corrections;
        }
    }
    // Combat: players (casts, swings, regen), then monsters (AI, movement, attacks, timers), then the
    // state/stat packets of this tick.
    for (auto& [id, owned] : m_players)
        updatePlayerCombat(*owned);
    for (auto& [id, owned] : m_monsters)
        updateMonster(*owned);
    flushCombatState();
    updateLoot();
    updateShops();
    for (auto& [id, owned] : m_players)
    {
        if (owned->relayDirty)
            relay(*owned, logicalNow);
    }
    for (auto& [id, owned] : m_monsters)
    {
        if (owned->relayDirty)
            relayMonster(*owned, logicalNow);
    }
    updateGates();
    if (save)
    {
        const bool periodic = m_settings.savePeriodTicks > 0;
        const auto phase = periodic ? static_cast<std::uint32_t>(tickIndex % m_settings.savePeriodTicks) : 0u;
        for (auto& [id, owned] : m_players)
        {
            Player& p = *owned;
            const bool now = p.combat.saveNow; // level-up (§3.5)
            if (!now && (!periodic || p.saveSlot != phase || !p.saveDirty))
                continue;
            p.saveDirty = false;
            p.combat.saveNow = false;
            if (!now)
                ++m_counters.periodicSaves;
            const auth::CharacterId character = p.character.id;
            save(p.account, character, {m_id, p.move.x, p.move.y, p.move.z, p.move.yaw}, progressOf(p),
                 [character](bool ok) {
                     if (!ok)
                         VO_LOG_WARN("world: periodic save of character {} failed", character);
                 });
        }
    }
    else
    {
        for (auto& [id, owned] : m_players)
            owned->combat.saveNow = false;
    }
}

void Zone::flush()
{
    for (auto& [id, owned] : m_players)
    {
        Player& p = *owned;
        if (p.out.empty())
            continue;
        if (p.link)
            p.link->deliver(std::move(p.out));
        p.out = WorldBatch{};
        p.out.generation = p.generation;
    }
}

} // namespace vo::server::world
