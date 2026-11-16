// Zone, part 3: loot containers (docs/PROTOCOL_LOOT.md, docs/SERVER_WORLD.md §8c). A dying monster
// with a non-empty roll leaves a container entity (entity_type 3) at its corpse; the world checks
// every request (range, dead, owner window, availability), reserves the requested units and hands
// them to the player's session in its batch (LootGrant). The session writes them through and
// settles the reservation (LootSettleCommand) with whatever was not taken. All on the world thread,
// timed by the tick's m_now like combat.

#include "server/world/Zone.h"

#include "shared/core/Log.h"

#include <algorithm>
#include <cmath>

namespace vo::server::world {

namespace vn = vo::net;
namespace vg = vo::game;

namespace {

float dist(float ax, float az, float bx, float bz)
{
    return vg::horizontalDistance(ax, az, bx, bz);
}

} // namespace

bool Zone::Loot::empty() const noexcept
{
    if (coins > 0)
        return false;
    for (std::size_t i = 0; i < entryCount; ++i)
        if (entries[i].quantity > 0)
            return false;
    return true;
}

Zone::Loot* Zone::lootById(EntityId id)
{
    const auto it = m_loot.find(id);
    return it == m_loot.end() ? nullptr : it->second.get();
}

const Zone::Loot* Zone::findLoot(EntityId id) const
{
    return const_cast<Zone*>(this)->lootById(id);
}

void Zone::installLoot(std::shared_ptr<const vg::LootTables> tables, std::function<EntityId()> allocate)
{
    m_lootTables = std::move(tables);
    m_allocate = std::move(allocate);
}

// ---- Creation and visibility ----

void Zone::dropLoot(const Monster& m, auth::CharacterId owner)
{
    if (!m_lootTables)
        return;
    const vg::LootTable* table = m_lootTables->forMonster(m.tmpl->id);
    if (!table)
        return;
    const vg::LootRoll roll = vg::rollLoot(*table, m_rng);
    if (roll.empty())
        return;
    (void)createLoot(m.x, m.z, owner, m.tmpl->id, m.tmpl->name, m.tmpl->level, roll);
}

EntityId Zone::createLoot(float x, float z, auth::CharacterId owner, std::uint16_t monsterTemplate, std::string name,
                          std::uint16_t level, const vg::LootRoll& roll)
{
    EntityId id = 0;
    if (m_allocate)
        id = m_allocate();
    else
    {
        // Zones driven without a world (tests): ids from the top of the range.
        id = 0x7F000000u;
        while (hasEntity(id))
            ++id;
    }
    auto owned = std::make_unique<Loot>();
    Loot& l = *owned;
    l.id = id;
    l.monsterTemplate = monsterTemplate;
    l.name = std::move(name);
    l.level = level;
    l.x = x;
    l.z = z;
    l.y = m_terrain.heightAt(x, z);
    l.cell = m_grid.cellOf(x, z);
    l.owner = owner;
    l.createdAt = m_now;
    l.ownerUntil = m_now + std::chrono::duration_cast<core::MonoClock::duration>(vg::kLootOwnerWindow);
    l.expiresAt = m_now + std::chrono::duration_cast<core::MonoClock::duration>(vg::kLootLifetime);
    l.ownerWindowOver = owner == 0;
    l.coins = roll.coins;
    for (const vg::LootItem& item : roll.items)
    {
        if (l.entryCount >= l.entries.size())
            break;
        l.entries[l.entryCount++] = {item.templateId, item.quantity};
    }
    m_loot.emplace(id, std::move(owned));
    m_grid.forEachInView(l.cell, [&](EntityId other) {
        if (Player* p = findById(other))
            lootSeen(*p, l);
    });
    m_grid.add(id, l.cell);
    ++m_counters.lootDrops;
    VO_LOG_TRACE("zone {}: loot {} of monster template {} at ({:.1f}, {:.1f}) for character {}: {} coins, {} entries",
                 m_id, id, monsterTemplate, x, z, owner, l.coins, l.entryCount);
    return id;
}

EntityId Zone::dropLootForTesting(float x, float z, auth::CharacterId owner, const vg::LootRoll& roll,
                                  std::uint16_t monsterTemplate)
{
    std::string name = "Ganimet";
    std::uint16_t level = 1;
    if (m_monsterData)
    {
        if (const vg::MonsterTemplate* t = m_monsterData->find(monsterTemplate))
        {
            name = t->name;
            level = t->level;
        }
    }
    return createLoot(x, z, owner, monsterTemplate, std::move(name), level, roll);
}

std::uint8_t Zone::lootStateFor(const Loot& l, const Player& p) const noexcept
{
    if (l.ownerWindowOver)
        return 0;
    return p.character.id == l.owner ? vn::kEntityStateLootMine : vn::kEntityStateLootLocked;
}

void Zone::sendLootSpawn(Player& to, const Loot& l)
{
    vn::EntitySpawn s;
    s.entityId = l.id;
    s.entityType = vn::kEntityTypeLoot;
    s.name = l.name;
    s.level = l.level;
    s.x = l.x;
    s.y = l.y;
    s.z = l.z;
    s.templateId = l.monsterTemplate;
    s.state = lootStateFor(l, to);
    to.out.appendMessage(s, m_scratch);
    ++m_counters.spawns;
}

void Zone::lootSeen(Player& p, Loot& l)
{
    if (p.known.insert(l.id).second)
    {
        l.observers.insert(p.id);
        sendLootSpawn(p, l);
    }
}

void Zone::lootUnseen(Player& p, Loot& l)
{
    if (p.known.erase(l.id) > 0)
    {
        l.observers.erase(p.id);
        if (p.viewingLoot == l.id)
            closeLootView(p, vn::LootClosedReason::OutOfRange, true);
        sendDespawn(p, l.id);
    }
}

void Zone::sendLootContents(Player& to, const Loot& l)
{
    vn::LootContents msg;
    msg.lootId = l.id;
    msg.coins = l.coins;
    for (std::size_t i = 0; i < l.entryCount; ++i)
        if (l.entries[i].quantity > 0)
            msg.entries.push_back({static_cast<std::uint8_t>(i), l.entries[i].templateId, l.entries[i].quantity});
    to.out.appendMessage(msg, m_scratch);
}

void Zone::closeLootView(Player& p, vn::LootClosedReason reason, bool notify)
{
    if (p.viewingLoot == 0)
        return;
    const EntityId id = p.viewingLoot;
    p.viewingLoot = 0;
    if (Loot* l = lootById(id))
        l->viewers.erase(p.id);
    if (notify)
        p.out.appendMessage(vn::LootClosed{id, reason}, m_scratch);
}

void Zone::removeLoot(Loot& l)
{
    for (const EntityId v : l.viewers)
    {
        if (Player* p = findById(v); p && p->viewingLoot == l.id)
        {
            p->viewingLoot = 0;
            p->out.appendMessage(vn::LootClosed{l.id, vn::LootClosedReason::Gone}, m_scratch);
        }
    }
    l.viewers.clear();
    for (const EntityId o : l.observers)
        if (Player* p = findById(o); p && p->known.erase(l.id) > 0)
            sendDespawn(*p, l.id);
    l.observers.clear();
    m_grid.remove(l.id, l.cell);
    ++m_counters.lootDespawns;
    m_loot.erase(l.id); // l is gone after this
}

// ---- Requests (§3) ----

vn::LootResult Zone::checkLootAccess(const Player& p, const Loot* l) const
{
    using R = vn::LootResult;
    if (!l || !knows(p, l->id))
        return R::NotFound;
    if (p.combat.dead)
        return R::YouAreDead;
    if (!vg::inRange(dist(p.move.x, p.move.z, l->x, l->z), vg::kLootRange))
        return R::OutOfRange;
    if (!l->ownerWindowOver && p.character.id != l->owner)
        return R::NotYours;
    return R::Ok;
}

void Zone::loot(const LootCommand& cmd)
{
    using K = LootCommand::Kind;
    using R = vn::LootResult;
    Player* pp = find(cmd.session);
    if (!pp)
        return;
    Player& p = *pp;
    switch (cmd.kind)
    {
    case K::Close:
        if (p.viewingLoot == cmd.loot)
            closeLootView(p, vn::LootClosedReason::Gone, false); // no answer (§3.2)
        return;
    case K::Open:
    {
        Loot* l = lootById(cmd.loot);
        const R r = checkLootAccess(p, l);
        p.out.appendMessage(vn::LootOpenRes{cmd.loot, r}, m_scratch);
        if (r != R::Ok)
            return;
        if (p.viewingLoot != l->id)
        {
            closeLootView(p, vn::LootClosedReason::Gone, false); // a new view replaces the old silently
            p.viewingLoot = l->id;
            l->viewers.insert(p.id);
        }
        sendLootContents(p, *l);
        return;
    }
    case K::Take:
    {
        Loot* l = lootById(cmd.loot);
        R r = checkLootAccess(p, l);
        LootReservation res;
        if (r == R::Ok)
        {
            const auto open = static_cast<std::size_t>(std::count_if(
                m_reservations.begin(), m_reservations.end(),
                [&](const auto& kv) { return kv.second.session == cmd.session; }));
            if (open >= vn::kMaxLootTakesInFlight)
                r = R::ServerError;
        }
        if (r == R::Ok)
        {
            const auto takeEntry = [&](std::size_t i) {
                Loot::Entry& e = l->entries[i];
                if (e.quantity == 0)
                    return;
                res.items.push_back({static_cast<std::uint8_t>(i), e.templateId, e.quantity});
                e.quantity = 0;
            };
            if (cmd.what < vn::kMaxLootEntries)
            {
                if (cmd.what < l->entryCount && l->entries[cmd.what].quantity > 0)
                    takeEntry(cmd.what);
                else
                    r = R::AlreadyTaken;
            }
            else if (cmd.what == vn::kLootTakeCoins)
            {
                if (l->coins > 0)
                {
                    res.coins = l->coins;
                    l->coins = 0;
                }
                else
                    r = R::AlreadyTaken;
            }
            else if (cmd.what == vn::kLootTakeAll)
            {
                if (l->empty())
                    r = R::AlreadyTaken;
                else
                {
                    res.coins = l->coins;
                    l->coins = 0;
                    for (std::size_t i = 0; i < l->entryCount; ++i)
                        takeEntry(i);
                }
            }
            else
            {
                r = R::AlreadyTaken; // 8..0xFD: no such entry
            }
        }
        if (r != R::Ok)
        {
            p.out.appendMessage(vn::LootTakeRes{cmd.seq, cmd.loot, r}, m_scratch);
            return;
        }
        res.id = (static_cast<std::uint64_t>(m_id) << 48) | m_nextReservation++;
        res.session = cmd.session;
        res.loot = l->id;
        ++l->reservations;
        l->contentsDirty = true;
        LootGrant grant;
        grant.reservation = res.id;
        grant.loot = l->id;
        grant.seq = cmd.seq;
        grant.coins = res.coins;
        grant.items = res.items;
        p.out.lootGrants.push_back(std::move(grant));
        m_reservations.emplace(res.id, std::move(res));
        ++m_counters.lootTakes;
        return;
    }
    }
}

void Zone::returnReservation(const LootReservation& r, std::uint32_t coins, const std::vector<LootGrant::Item>& items)
{
    Loot* l = lootById(r.loot);
    if (!l)
    {
        // Cannot happen: a container is never removed while a reservation is open.
        VO_LOG_ERROR("zone {}: loot {} of reservation {} is gone; {} coin(s) and {} item line(s) lost", m_id, r.loot,
                     r.id, coins, items.size());
        return;
    }
    bool returned = false;
    // Never more than was reserved, never onto another entry.
    const std::uint32_t backCoins = std::min(coins, r.coins);
    if (backCoins > 0)
    {
        l->coins += backCoins;
        returned = true;
    }
    for (const LootGrant::Item& back : items)
    {
        const auto reserved = std::find_if(r.items.begin(), r.items.end(),
                                           [&](const LootGrant::Item& i) { return i.index == back.index; });
        if (reserved == r.items.end() || back.quantity == 0 || back.index >= l->entryCount)
            continue;
        Loot::Entry& e = l->entries[back.index];
        e.quantity = static_cast<std::uint16_t>(e.quantity + std::min(back.quantity, reserved->quantity));
        returned = true;
    }
    if (returned)
    {
        l->contentsDirty = true;
        ++m_counters.lootReturns;
    }
    if (l->reservations > 0)
        --l->reservations;
}

bool Zone::settleLoot(const LootSettleCommand& cmd)
{
    if ((cmd.reservation >> 48) != m_id)
        return false;
    const auto it = m_reservations.find(cmd.reservation);
    if (it == m_reservations.end())
    {
        VO_LOG_ERROR("zone {}: settle of unknown loot reservation {} ignored", m_id, cmd.reservation);
        return true;
    }
    const LootReservation res = std::move(it->second);
    m_reservations.erase(it);
    returnReservation(res, cmd.coinsReturned, cmd.itemsReturned);
    return true;
}

void Zone::cancelLootTakes(SessionKey session, const std::vector<std::uint64_t>& keep)
{
    for (auto it = m_reservations.begin(); it != m_reservations.end();)
    {
        const LootReservation& r = it->second;
        if (r.session != session || std::find(keep.begin(), keep.end(), r.id) != keep.end())
        {
            ++it;
            continue;
        }
        const LootReservation res = std::move(it->second);
        it = m_reservations.erase(it);
        returnReservation(res, res.coins, res.items);
    }
}

// ---- Per tick ----

void Zone::updateLoot()
{
    if (m_loot.empty())
        return;
    std::vector<EntityId> gone;
    for (auto& [id, owned] : m_loot)
    {
        Loot& l = *owned;
        if (!l.ownerWindowOver && m_now >= l.ownerUntil)
        {
            // §1.4: the owner window ended; everybody who saw a lock bit sees it cleared.
            l.ownerWindowOver = true;
            const vn::EntityState msg{l.id, 0};
            for (const EntityId o : l.observers)
                if (Player* p = findById(o))
                    p->out.appendMessage(msg, m_scratch);
        }
        if (!l.viewers.empty())
        {
            const std::vector<EntityId> viewers(l.viewers.begin(), l.viewers.end());
            for (const EntityId v : viewers)
            {
                Player* p = findById(v);
                if (!p || p->viewingLoot != l.id)
                {
                    l.viewers.erase(v);
                    continue;
                }
                if (p->combat.dead)
                    closeLootView(*p, vn::LootClosedReason::YouDied, true);
                else if (dist(p->move.x, p->move.z, l.x, l.z) > vg::kLootViewDistance)
                    closeLootView(*p, vn::LootClosedReason::OutOfRange, true);
            }
        }
        if (l.contentsDirty)
        {
            l.contentsDirty = false;
            for (const EntityId v : l.viewers)
                if (Player* p = findById(v))
                    sendLootContents(*p, l);
        }
        if (l.reservations == 0 && (l.empty() || m_now >= l.expiresAt))
            gone.push_back(id);
    }
    for (const EntityId id : gone)
        if (Loot* l = lootById(id))
            removeLoot(*l);
}

} // namespace vo::server::world
