// Zone, part 4: skill execution and timed effects (docs/PROTOCOL_SKILLS.md §1, docs/SERVER_WORLD.md
// §8d): ranks, area targeting, the leap, effects on players (buffs, heal over time, shields) and on
// monsters (damage over time, slow, stun, root, weaken, taunt), their ticks, expiry and the
// EFFECT_* / SKILL_* packets. World thread only, with the tick's scheduled time m_now as the clock.

#include "server/world/Zone.h"

#include "shared/core/Log.h"
#include "shared/game/Movement.h"
#include "shared/game/TerrainWalk.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vo::server::world {

namespace vn = vo::net;
namespace vg = vo::game;
using Ms = std::chrono::milliseconds;

namespace {

core::MonoClock::duration msDuration(std::uint32_t ms)
{
    return std::chrono::duration_cast<core::MonoClock::duration>(Ms(ms));
}

float dist(float ax, float az, float bx, float bz)
{
    return vg::horizontalDistance(ax, az, bx, bz);
}

bool changesStats(vg::EffectKind k) noexcept
{
    switch (k)
    {
    case vg::EffectKind::AttackUp:
    case vg::EffectKind::DefenseUp:
    case vg::EffectKind::Haste:
    case vg::EffectKind::DodgeUp:
    case vg::EffectKind::MaxHpUp: return true;
    default: return false;
    }
}

// Inserts or refreshes (same skill and kind). False if the list is full.
bool upsert(std::vector<Zone::ActiveEffect>& list, const Zone::ActiveEffect& e)
{
    for (auto& o : list)
    {
        if (o.skill == e.skill && o.kind == e.kind)
        {
            o = e;
            return true;
        }
    }
    if (list.size() >= vg::kMaxEffectsPerEntity)
        return false;
    list.push_back(e);
    return true;
}

} // namespace

// ---- Queries ----

std::uint8_t Zone::rankOf(const Player& p, const vg::SkillDef& skill) const noexcept
{
    return skill.charClass == p.character.charClass ? p.combat.skillRanks[skill.id] : std::uint8_t{0};
}

bool Zone::hasEffect(const std::vector<ActiveEffect>& list, vg::EffectKind kind) noexcept
{
    return std::any_of(list.begin(), list.end(), [&](const ActiveEffect& e) { return e.kind == kind; });
}

float Zone::strongest(const std::vector<ActiveEffect>& list, vg::EffectKind kind) noexcept
{
    float v = 0.0f;
    for (const auto& e : list)
        if (e.kind == kind)
            v = std::max(v, e.value);
    return std::min(v, 90.0f);
}

float Zone::summed(const std::vector<ActiveEffect>& list, vg::EffectKind kind) noexcept
{
    float v = 0.0f;
    for (const auto& e : list)
        if (e.kind == kind)
            v += e.value;
    return v;
}

vg::EffectModifiers Zone::modifiersOf(const Player& p) noexcept
{
    vg::EffectModifiers m;
    for (const auto& e : p.combat.effects)
    {
        switch (e.kind)
        {
        case vg::EffectKind::AttackUp: m.attackPct += e.value; break;
        case vg::EffectKind::DefenseUp: m.defensePct += e.value; break;
        case vg::EffectKind::Haste: m.hastePct += e.value; break;
        case vg::EffectKind::DodgeUp: m.dodgePct += e.value; break;
        case vg::EffectKind::MaxHpUp: m.maxHpPct += e.value; break;
        default: break;
        }
    }
    return m;
}

EntityId Zone::tauntSource(const Monster& m) const noexcept
{
    // §1.6: the latest taunt wins.
    EntityId source = 0;
    core::MonoTime latest{};
    for (const auto& e : m.effects)
    {
        if (e.kind != vg::EffectKind::Taunt)
            continue;
        const core::MonoTime started = e.endsAt - msDuration(e.durationMs);
        if (source == 0 || started >= latest)
        {
            source = e.source;
            latest = started;
        }
    }
    return source;
}

const std::vector<Zone::ActiveEffect>* Zone::effectsOf(EntityId id) const
{
    if (const Player* p = findEntity(id))
        return &p->combat.effects;
    if (const Monster* m = findMonster(id))
        return &m->effects;
    return nullptr;
}

net::EffectEntry Zone::effectEntry(const ActiveEffect& e) const
{
    net::EffectEntry out;
    out.skillId = e.skill;
    out.kind = static_cast<std::uint8_t>(e.kind);
    out.stacks = 1;
    out.sourceId = e.source;
    out.durationMs = e.durationMs;
    const auto left = e.endsAt > m_now ? std::chrono::duration_cast<Ms>(e.endsAt - m_now).count() : 0;
    out.remainingMs = static_cast<std::uint32_t>(std::clamp<long long>(left, 0, e.durationMs));
    out.amount = e.amount;
    return out;
}

void Zone::sendEffectList(Player& to, EntityId about, const std::vector<ActiveEffect>& effects)
{
    net::EffectList msg;
    msg.entityId = about;
    for (const auto& e : effects)
    {
        if (msg.effects.size() >= net::kMaxEffectEntries)
            break;
        msg.effects.push_back(effectEntry(e));
    }
    to.out.appendMessage(msg, m_scratch);
}

void Zone::sendCooldown(Player& p, std::uint16_t skill, std::uint32_t totalMs)
{
    if (totalMs == 0)
        return;
    p.out.appendMessage(net::SkillCooldowns{{{skill, totalMs, totalMs}}}, m_scratch);
}

// ---- Targeting (§1.4) ----

bool Zone::findLeapPoint(const Player& p, const Monster& m, float leapStop, float& x, float& z) const
{
    const float d = dist(p.move.x, p.move.z, m.x, m.z);
    if (d <= leapStop + vg::kRangeTolerance)
    {
        x = p.move.x; // already next to it: no movement
        z = p.move.z;
        return true;
    }
    const float ux = (p.move.x - m.x) / d, uz = (p.move.z - m.z) / d; // from the target towards the caster
    constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;
    for (const float deg : {0.0f, 30.0f, -30.0f, 60.0f, -60.0f, 90.0f, -90.0f})
    {
        const float a = deg * kDeg, ca = std::cos(a), sa = std::sin(a);
        const float cx = m.x + leapStop * (ux * ca - uz * sa);
        const float cz = m.z + leapStop * (ux * sa + uz * ca);
        if (!placeable(cx, cz) || inSafeZone(cx, cz) || !vg::isSegmentWalkable(m_terrain, p.move.x, p.move.z, cx, cz))
            continue;
        x = cx;
        z = cz;
        return true;
    }
    return false;
}

void Zone::collectEnemies(const Player& p, float cx, float cz, float radius, std::size_t max, EntityId primary,
                          std::vector<Monster*>& out)
{
    out.clear();
    std::vector<std::pair<float, Monster*>> found;
    for (const EntityId id : p.known)
    {
        Monster* m = monsterById(id);
        if (!m || !m->inGrid || (m->state != Monster::State::Idle && m->state != Monster::State::Chase) ||
            inSafeZone(m->x, m->z))
            continue;
        const float d = dist(cx, cz, m->x, m->z);
        if (m->id == primary)
            found.emplace_back(-1.0f, m); // always first
        else if (vg::inRange(d, radius))
            found.emplace_back(d, m);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->id < b.second->id;
    });
    for (const auto& [d, m] : found)
    {
        if (out.size() >= max)
            break;
        out.push_back(m);
    }
}

void Zone::collectAllies(Player& p, float cx, float cz, float radius, std::size_t max, std::vector<Player*>& out)
{
    out.clear();
    std::vector<std::pair<float, Player*>> found;
    if (!p.combat.dead)
        found.emplace_back(-1.0f, &p); // the caster always
    for (const EntityId id : p.known)
    {
        Player* o = findById(id);
        if (!o || o->combat.dead)
            continue;
        const float d = dist(cx, cz, o->move.x, o->move.z);
        if (vg::inRange(d, radius))
            found.emplace_back(d, o);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->id < b.second->id;
    });
    for (const auto& [d, o] : found)
    {
        if (out.size() >= max)
            break;
        out.push_back(o);
    }
}

// ---- Execution ----

void Zone::executeSkill(Player& p, const vg::SkillDef& skill, EntityId target)
{
    auto& c = p.combat;
    const std::uint8_t rank = std::max<std::uint8_t>(rankOf(p, skill), 1);
    const vg::SkillRankData& r = skill.at(rank);
    c.mp -= std::min(c.mp, r.mpCost);
    c.skillReadyAt[skill.id] = m_now + msDuration(r.cooldownMs);
    c.skillCooldownMs[skill.id] = r.cooldownMs;
    c.gcdUntil = std::max(c.gcdUntil, m_now + vg::kGlobalCooldown);
    c.selfStatsDirty = true;
    sendCooldown(p, skill.id, r.cooldownMs); // PROTOCOL_SKILLS.md §3.6

    Monster* primary = skill.target == vg::SkillTarget::Enemy ? monsterById(target) : nullptr;
    if (skill.leap && primary)
    {
        float x = 0.0f, z = 0.0f;
        if (findLeapPoint(p, *primary, skill.leapStop, x, z) && dist(p.move.x, p.move.z, x, z) > 0.05f)
        {
            // §1.4: the server moves the caster; its own moves in flight get corrected.
            p.move.flags = 0;
            p.move.credit = 0.0f; // no banked movement on top of the leap
            p.move.yaw = vg::yawFromDirection(primary->x - x, primary->z - z);
            p.moveTimeMs = m_nowMs;
            movePlayerTo(p, x, z);
            p.correctionPending = true;
            sendAround(vn::SkillLeap{p.id, p.move.x, p.move.y, p.move.z, p.move.yaw, primary->id}, p.id, primary->id);
        }
    }
    const EntityId shownTarget = skill.target == vg::SkillTarget::Self ? p.id : target;
    sendAround(vn::SkillUsed{p.id, skill.id, shownTarget, rank}, p.id, shownTarget == p.id ? 0 : shownTarget);

    if (skill.hitsEnemies())
    {
        std::vector<Monster*> hit;
        if (skill.area == vg::SkillArea::AroundSelf)
            collectEnemies(p, p.move.x, p.move.z, skill.radius, skill.maxTargets, 0, hit);
        else if (primary && skill.area == vg::SkillArea::AroundTarget)
            collectEnemies(p, primary->x, primary->z, skill.radius, skill.maxTargets, primary->id, hit);
        else if (primary)
            hit.push_back(primary);
        const float crit = std::min(vg::kMaxSkillCritChance, c.stats.critChance + r.critBonus);
        for (Monster* m : hit)
        {
            const auto alive = [&] {
                return m->hp > 0 && (m->state == Monster::State::Idle || m->state == Monster::State::Chase);
            };
            for (std::uint8_t i = 0; r.multiplier > 0.0f && i < skill.hits && alive(); ++i)
                damageMonster(p, *m, skill.id,
                              vg::rollDamage(c.stats.attackMin, c.stats.attackMax, r.multiplier, m->tmpl->defense,
                                             crit, m_rng)); // M18: the weapon's damage range
            for (const vg::EffectDef& e : skill.effects)
                if (!e.onSelf && alive())
                    applyEffect(p, *m, skill, rank, e);
        }
    }
    else
    {
        std::vector<Player*> hit;
        if (skill.area == vg::SkillArea::AroundSelf && skill.affectsAllies)
            collectAllies(p, p.move.x, p.move.z, skill.radius, skill.maxTargets, hit);
        else if (Player* t = target == p.id || target == 0 ? &p : findById(target))
            hit.push_back(t);
        for (Player* t : hit)
        {
            if (t->combat.dead)
                continue;
            if (r.healFraction > 0.0f)
                healPlayer(p, *t, skill, rank);
            for (const vg::EffectDef& e : skill.effects)
                if (!e.onSelf)
                    applyEffect(p, *t, skill, rank, e);
        }
    }
    for (const vg::EffectDef& e : skill.effects)
        if (e.onSelf && !c.dead)
            applyEffect(p, p, skill, rank, e);
}

// ---- Effects (§1.5) ----

void Zone::applyEffect(Player& caster, Player& target, const vg::SkillDef& skill, std::uint8_t rank,
                       const vg::EffectDef& def)
{
    const std::size_t ri = static_cast<std::size_t>(std::clamp<std::uint8_t>(rank, 1, vg::kMaxSkillRank) - 1);
    ActiveEffect e;
    e.skill = skill.id;
    e.kind = def.kind;
    e.source = caster.id;
    e.value = def.value[ri];
    e.durationMs = def.durationMs[ri];
    e.endsAt = m_now + msDuration(e.durationMs);
    switch (def.kind)
    {
    case vg::EffectKind::Hot:
        e.tickMs = def.tickMs;
        e.nextTickAt = m_now + msDuration(def.tickMs);
        e.amount = vg::periodicHeal(target.combat.stats.maxHp, e.value, caster.combat.stats.healMultiplier);
        break;
    case vg::EffectKind::Shield: e.amount = vg::shieldAmount(caster.combat.stats.attack, e.value); break;
    default: e.amount = static_cast<std::uint32_t>(std::lround(e.value)); break;
    }
    const std::uint32_t maxHpBefore = target.combat.stats.maxHp;
    if (!upsert(target.combat.effects, e))
        return;
    sendAround(vn::EffectApply{target.id, effectEntry(e)}, target.id);
    if (changesStats(def.kind))
        effectStatsChanged(target, maxHpBefore);
}

void Zone::applyEffect(Player& caster, Monster& target, const vg::SkillDef& skill, std::uint8_t rank,
                       const vg::EffectDef& def)
{
    const std::size_t ri = static_cast<std::size_t>(std::clamp<std::uint8_t>(rank, 1, vg::kMaxSkillRank) - 1);
    ActiveEffect e;
    e.skill = skill.id;
    e.kind = def.kind;
    e.source = caster.id;
    e.value = def.value[ri];
    e.durationMs = def.durationMs[ri];
    e.endsAt = m_now + msDuration(e.durationMs);
    switch (def.kind)
    {
    case vg::EffectKind::Dot:
        e.tickMs = def.tickMs;
        e.nextTickAt = m_now + msDuration(def.tickMs);
        e.amount = vg::periodicDamage(caster.combat.stats.attack, e.value);
        break;
    case vg::EffectKind::Stun:
    case vg::EffectKind::Root:
    case vg::EffectKind::Taunt: e.amount = 0; break;
    default: e.amount = static_cast<std::uint32_t>(std::lround(e.value)); break;
    }
    if (!upsert(target.effects, e))
        return;
    sendAround(vn::EffectApply{target.id, effectEntry(e)}, target.id);
    // A harmful effect is an attack: it pulls the monster and counts a little threat; a taunt puts
    // the caster on top (§1.6).
    Monster::Threat* t = threatOf(target, caster.id);
    if (!t)
    {
        target.threats.push_back({caster.id, 0.0f, 0});
        t = &target.threats.back();
    }
    if (def.kind == vg::EffectKind::Taunt)
    {
        float top = 0.0f;
        for (const auto& o : target.threats)
            top = std::max(top, o.threat);
        t->threat = std::max(t->threat, top * 1.1f + 1.0f);
    }
    else
    {
        t->threat += 1.0f;
    }
    markInCombat(caster);
    if (target.state == Monster::State::Idle)
        startChase(target, caster);
    if (def.kind == vg::EffectKind::Stun || def.kind == vg::EffectKind::Root)
        stopMonster(target);
}

void Zone::effectStatsChanged(Player& p, std::uint32_t maxHpBefore)
{
    auto& c = p.combat;
    const vg::Stats before = c.stats;
    recomputeStats(p);
    if (c.stats.maxHp > maxHpBefore && !c.dead)
        c.hp = std::min(c.stats.maxHp, c.hp + (c.stats.maxHp - maxHpBefore)); // §1.5: max_hp_up raises hp too
    c.selfStatsDirty = true;
    c.charStatsDirty = true;
    if (before.maxHp != c.stats.maxHp)
        c.entityStatsDirty = true; // observers' hp bars
}

void Zone::removePlayerEffect(Player& p, std::size_t index, net::EffectRemoveReason reason)
{
    auto& list = p.combat.effects;
    const ActiveEffect e = list[index];
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
    sendAround(vn::EffectRemove{p.id, e.skill, static_cast<std::uint8_t>(e.kind), reason}, p.id);
    if (changesStats(e.kind))
        effectStatsChanged(p, p.combat.stats.maxHp);
}

void Zone::removeMonsterEffect(Monster& m, std::size_t index, net::EffectRemoveReason reason)
{
    const ActiveEffect e = m.effects[index];
    m.effects.erase(m.effects.begin() + static_cast<std::ptrdiff_t>(index));
    sendAround(vn::EffectRemove{m.id, e.skill, static_cast<std::uint8_t>(e.kind), reason}, m.id);
}

void Zone::clearPlayerEffects(Player& p)
{
    if (p.combat.effects.empty())
        return;
    const bool stats = std::any_of(p.combat.effects.begin(), p.combat.effects.end(),
                                   [](const ActiveEffect& e) { return changesStats(e.kind); });
    p.combat.effects.clear();
    if (stats)
        effectStatsChanged(p, p.combat.stats.maxHp);
}

void Zone::clearMonsterEffects(Monster& m, bool notify)
{
    if (notify)
        while (!m.effects.empty())
            removeMonsterEffect(m, m.effects.size() - 1, net::EffectRemoveReason::Cleared);
    m.effects.clear();
}

std::uint32_t Zone::absorbDamage(Player& p, std::uint32_t amount, bool& absorbed)
{
    auto& list = p.combat.effects;
    for (std::size_t i = 0; i < list.size() && amount > 0;)
    {
        ActiveEffect& e = list[i];
        if (e.kind != vg::EffectKind::Shield)
        {
            ++i;
            continue;
        }
        const std::uint32_t take = std::min(amount, e.amount);
        if (take > 0)
            absorbed = true;
        e.amount -= take;
        amount -= take;
        if (e.amount == 0)
        {
            removePlayerEffect(p, i, net::EffectRemoveReason::Depleted);
            continue;
        }
        sendAround(vn::EffectApply{p.id, effectEntry(e)}, p.id); // the absorb left
        ++i;
    }
    return amount;
}

void Zone::updatePlayerEffects(Player& p)
{
    auto& list = p.combat.effects;
    for (std::size_t i = 0; i < list.size();)
    {
        ActiveEffect& e = list[i];
        if (e.kind == vg::EffectKind::Hot)
        {
            while (e.tickMs > 0 && e.nextTickAt <= m_now && e.nextTickAt <= e.endsAt)
            {
                e.nextTickAt += msDuration(e.tickMs);
                const std::uint16_t skill = e.skill;
                const EntityId source = e.source;
                const std::uint32_t amount = e.amount;
                healPlayerBy(findById(source), source, p, skill, amount, vn::kCombatFlagPeriodic);
            }
        }
        if (m_now >= list[i].endsAt)
        {
            removePlayerEffect(p, i, net::EffectRemoveReason::Expired);
            continue;
        }
        ++i;
    }
}

void Zone::updateMonsterEffects(Monster& m)
{
    for (std::size_t i = 0; i < m.effects.size();)
    {
        ActiveEffect& e = m.effects[i];
        if (e.kind == vg::EffectKind::Dot)
        {
            while (e.tickMs > 0 && e.nextTickAt <= m_now && e.nextTickAt <= e.endsAt)
            {
                e.nextTickAt += msDuration(e.tickMs);
                const std::uint16_t skill = e.skill;
                const EntityId source = e.source;
                const std::uint32_t amount = e.amount;
                damageMonster(findById(source), source, m, skill, vg::Hit{amount, false}, vn::kCombatFlagPeriodic);
                if (m.state == Monster::State::Dead || m.effects.empty())
                    return; // died (its effects are cleared)
            }
        }
        if (m_now >= m.effects[i].endsAt)
        {
            removeMonsterEffect(m, i, net::EffectRemoveReason::Expired);
            continue;
        }
        ++i;
    }
}

} // namespace vo::server::world
