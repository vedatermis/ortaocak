// Zone, part 5: town NPCs, their shops and the use of consumables (docs/PROTOCOL_SHOP.md,
// docs/SERVER_WORLD.md §8g). NPCs are static entities (entity type 4) seen through the area of
// interest like loot containers. The world checks what depends on the live world (shop open, alive,
// range, stock; for consumables alive, stun, the shared cooldown, something to restore) and hands
// accepted requests to the player's session in its batch (ShopGrant, ItemUseGrant); the session
// decides against the committed inventory and balance, writes and answers. A consumed potion comes
// back as ItemUseCommand(Apply) and is restored here. All on the world thread, timed by m_now.

#include "server/world/Zone.h"

#include "shared/core/Log.h"
#include "shared/game/Movement.h"
#include "shared/game/Shop.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace vo::server::world {

namespace vn = vo::net;
namespace vg = vo::game;

namespace {

float dist(float ax, float az, float bx, float bz)
{
    return vg::horizontalDistance(ax, az, bx, bz);
}

bool isMarkerKeptClear(std::string_view name) noexcept
{
    return name.starts_with("spawn_") || name.starts_with("arrive_");
}

} // namespace

// ---- NPCs ----

Zone::Npc* Zone::npcById(EntityId id)
{
    const auto it = m_npcs.find(id);
    return it == m_npcs.end() ? nullptr : it->second.get();
}

const Zone::Npc* Zone::findNpc(EntityId id) const
{
    return const_cast<Zone*>(this)->npcById(id);
}

bool Zone::installNpcs(std::shared_ptr<const vg::NpcCatalog> catalog, const vg::ItemCatalog& items,
                       const std::function<EntityId()>& allocate, std::string& error)
{
    if (!catalog)
        return true;
    // PROTOCOL_SHOP.md §1.2: clear of the gates and of the markers players appear at.
    std::vector<vg::KeepOut> keep;
    for (std::size_t i = 0; i < m_layout.gates.size() && i < m_gatePoints.size(); ++i)
        keep.push_back({std::format("gate '{}'", m_layout.gates[i].def.marker), m_gatePoints[i].x, m_gatePoints[i].z,
                        m_gatePoints[i].radius + vg::kNpcGateMargin});
    for (const assets::SpawnPoint& s : m_terrain.spawns)
        if (isMarkerKeptClear(s.name))
            keep.push_back({std::format("marker '{}'", s.name), s.position.x, s.position.z, vg::kNpcMarkerMargin});

    std::vector<std::unique_ptr<Npc>> made;
    for (const vg::NpcDef& def : catalog->npcs())
    {
        if (def.zone != m_id)
            continue;
        std::string why;
        if (!vg::checkNpcSpot(m_terrain, def.x, def.z, keep, why))
        {
            error = std::format("NPC {} ({}) in zone {}: {}", def.id, def.name, m_id, why);
            return false;
        }
        auto n = std::make_unique<Npc>();
        n->def = &def;
        n->x = def.x;
        n->z = def.z;
        n->y = m_terrain.heightAt(def.x, def.z);
        n->yaw = vg::normalizeYaw(def.yaw);
        n->cell = m_grid.cellOf(def.x, def.z);
        for (const vg::ItemTemplateId tid : def.stock)
        {
            const vg::ItemTemplate* t = items.find(tid);
            if (!t || vg::buyPrice(*t) == 0)
            {
                error = std::format("NPC {} ({}): stock item {} is unknown or has no price", def.id, def.name, tid);
                return false;
            }
            n->stock.push_back({tid, vg::buyPrice(*t)});
        }
        made.push_back(std::move(n));
    }
    for (auto& owned : made)
    {
        owned->id = allocate ? allocate() : 0;
        if (owned->id == 0)
        {
            owned->id = 0x7E000000u; // zones driven without a world (tests)
            while (hasEntity(owned->id))
                ++owned->id;
        }
        Npc& n = *owned;
        m_npcs.emplace(n.id, std::move(owned));
        m_grid.forEachInView(n.cell, [&](EntityId other) {
            if (Player* p = findById(other))
                npcSeen(*p, n);
        });
        m_grid.add(n.id, n.cell);
        VO_LOG_DEBUG("zone {}: NPC {} '{}' ({}) as entity {} at ({:.1f}, {:.1f})", m_id, n.def->id, n.def->name,
                     n.def->model, n.id, n.x, n.z);
    }
    m_npcCatalog = std::move(catalog);
    return true;
}

void Zone::sendNpcSpawn(Player& to, const Npc& n)
{
    vn::EntitySpawn s;
    s.entityId = n.id;
    s.entityType = vn::kEntityTypeNpc;
    s.name = n.def->name;
    s.level = 0;
    s.x = n.x;
    s.y = n.y;
    s.z = n.z;
    s.yaw = n.yaw;
    s.templateId = n.def->id;
    s.state = 0;
    to.out.appendMessage(s, m_scratch);
    ++m_counters.spawns;
}

void Zone::npcSeen(Player& p, Npc& n)
{
    if (p.known.insert(n.id).second)
    {
        n.observers.insert(p.id);
        sendNpcSpawn(p, n);
    }
}

void Zone::npcUnseen(Player& p, Npc& n)
{
    if (p.known.erase(n.id) > 0)
    {
        n.observers.erase(p.id);
        if (p.shopNpc == n.id)
            closeShop(p, vn::ShopClosedReason::OutOfRange, true);
        sendDespawn(p, n.id);
    }
}

// ---- Shops (PROTOCOL_SHOP.md §2.2) ----

void Zone::closeShop(Player& p, vn::ShopClosedReason reason, bool notify)
{
    if (p.shopNpc == 0)
        return;
    const EntityId id = p.shopNpc;
    p.shopNpc = 0;
    if (notify)
        p.out.appendMessage(vn::ShopClosed{id, reason}, m_scratch);
}

vn::ShopResult Zone::checkShopAccess(const Player& p, const Npc* n) const
{
    using R = vn::ShopResult;
    if (!n || !knows(p, n->id) || n->def->kind != vg::NpcKind::Merchant)
        return R::NotFound;
    if (p.combat.dead)
        return R::YouAreDead;
    if (!vg::inRange(dist(p.move.x, p.move.z, n->x, n->z), vg::kNpcTalkRange))
        return R::OutOfRange;
    return R::Ok;
}

void Zone::shop(const ShopCommand& cmd)
{
    using K = ShopCommand::Kind;
    using R = vn::ShopResult;
    Player* pp = find(cmd.session);
    if (!pp || pp->loading)
        return; // PROTOCOL_ZONES.md §3.4: sent before the client knew it changed zone
    Player& p = *pp;
    switch (cmd.kind)
    {
    case K::Close:
        if (p.shopNpc == cmd.npc)
            closeShop(p, vn::ShopClosedReason::OutOfRange, false); // no answer (§2.2)
        return;
    case K::Open:
    {
        Npc* n = npcById(cmd.npc);
        vn::ShopOpenRes res;
        res.npcId = cmd.npc;
        res.result = checkShopAccess(p, n);
        if (res.result == R::Ok)
        {
            if (p.shopNpc != n->id)
                closeShop(p, vn::ShopClosedReason::OutOfRange, false); // another one closes silently
            p.shopNpc = n->id;
            res.entries = n->stock;
            ++m_counters.shopOpens;
        }
        p.out.appendMessage(res, m_scratch);
        return;
    }
    case K::Buy:
    case K::Sell:
    {
        Npc* n = npcById(cmd.npc);
        R r = p.shopNpc == 0 || p.shopNpc != cmd.npc || !n ? R::NotOpen : checkShopAccess(p, n);
        std::uint32_t unitPrice = 0;
        if (r == R::Ok && cmd.kind == K::Buy)
        {
            const auto it = std::find_if(n->stock.begin(), n->stock.end(),
                                         [&](const vn::ShopEntry& e) { return e.templateId == cmd.templateId; });
            if (it == n->stock.end())
                r = R::NotInStock;
            else
                unitPrice = it->price;
        }
        if (r != R::Ok)
        {
            if (cmd.kind == K::Buy)
                p.out.appendMessage(vn::ShopBuyRes{cmd.seq, r, cmd.templateId, cmd.quantity, 0}, m_scratch);
            else
                p.out.appendMessage(vn::ShopSellRes{cmd.seq, r, 0, cmd.quantity, 0}, m_scratch);
            return;
        }
        ShopGrant g;
        g.kind = cmd.kind == K::Buy ? ShopGrant::Kind::Buy : ShopGrant::Kind::Sell;
        g.seq = cmd.seq;
        g.npc = cmd.npc;
        g.templateId = cmd.templateId;
        g.quantity = cmd.quantity;
        g.bagSlot = cmd.bagSlot;
        g.unitPrice = unitPrice;
        p.out.shopGrants.push_back(g);
        ++m_counters.shopGrants;
        return;
    }
    }
}

void Zone::updateShops()
{
    for (auto& [id, owned] : m_players)
    {
        Player& p = *owned;
        if (p.shopNpc == 0)
            continue;
        const Npc* n = npcById(p.shopNpc);
        if (!n)
        {
            p.shopNpc = 0;
            continue;
        }
        if (p.combat.dead)
            closeShop(p, vn::ShopClosedReason::YouDied, true);
        else if (dist(p.move.x, p.move.z, n->x, n->z) > vg::kShopViewDistance)
            closeShop(p, vn::ShopClosedReason::OutOfRange, true);
    }
}

// ---- Consumables (PROTOCOL_SHOP.md §2.3) ----

void Zone::sendItemCooldown(Player& p, core::MonoTime now)
{
    const auto& c = p.combat;
    if (c.potionReadyAt <= now || c.potionCooldownMs == 0)
        return;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(c.potionReadyAt - now).count();
    const auto remaining = static_cast<std::uint32_t>(std::clamp<long long>(left, 1, c.potionCooldownMs));
    p.out.appendMessage(vn::ItemCooldown{remaining, c.potionCooldownMs}, m_scratch);
}

void Zone::useItem(const ItemUseCommand& cmd)
{
    Player* pp = find(cmd.session);
    if (!pp)
        return;
    Player& p = *pp;
    auto& c = p.combat;
    if (cmd.kind == ItemUseCommand::Kind::Apply)
    {
        // The unit is consumed (committed). A player who died meanwhile gets nothing back (§2.3).
        if (c.dead)
            return;
        const std::uint32_t hpGain = std::min(cmd.restoreHp, c.stats.maxHp - std::min(c.hp, c.stats.maxHp));
        const std::uint32_t mpGain = std::min(cmd.restoreMp, c.stats.maxMp - std::min(c.mp, c.stats.maxMp));
        c.hp += hpGain;
        c.mp += mpGain;
        if (hpGain > 0 || mpGain > 0)
        {
            c.selfStatsDirty = true;
            p.saveDirty = true;
        }
        if (hpGain > 0)
            c.entityStatsDirty = true;
        sendAround(vn::ItemUsed{p.id, cmd.templateId, hpGain, mpGain}, p.id);
        ++m_counters.itemRestores;
        return;
    }
    if (p.loading)
        return; // PROTOCOL_ZONES.md §3.4
    vg::ItemUseState s;
    s.dead = c.dead;
    s.stunned = hasEffect(c.effects, vg::EffectKind::Stun);
    s.onCooldown = c.potionReadyAt > m_now;
    s.hp = c.hp;
    s.maxHp = c.stats.maxHp;
    s.mp = c.mp;
    s.maxMp = c.stats.maxMp;
    const vn::ItemUseResult r = vg::checkItemUse(cmd.restoreHp, cmd.restoreMp, s);
    if (r != vn::ItemUseResult::Ok)
    {
        p.out.appendMessage(vn::ItemUseRes{cmd.seq, r, cmd.templateId}, m_scratch);
        return;
    }
    c.potionReadyAt = m_now + std::chrono::milliseconds(vg::kConsumableCooldownMs);
    c.potionCooldownMs = vg::kConsumableCooldownMs;
    sendItemCooldown(p, m_now);
    p.out.useGrants.push_back({cmd.seq, cmd.templateId, cmd.bagSlot, cmd.restoreHp, cmd.restoreMp});
    ++m_counters.itemUses;
}

void Zone::addPlayerEffectForTesting(SessionKey session, vg::EffectKind kind, std::uint32_t ms)
{
    Player* p = find(session);
    if (!p)
        return;
    ActiveEffect e;
    e.skill = 7; // Sarsıntı: a skill that stuns (only monsters in v8)
    e.kind = kind;
    e.durationMs = ms;
    e.endsAt = m_now + std::chrono::milliseconds(ms);
    p->combat.effects.push_back(e);
}

} // namespace vo::server::world
