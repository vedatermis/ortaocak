#include "client/world/ClientInventory.h"

#include "shared/core/Log.h"

#include <format>

namespace vo::client::world {

namespace vn = vo::net;

namespace {

game::ItemPlace placeOf(const vn::ItemEntry& e)
{
    return {e.container, e.slot};
}

} // namespace

void ClientInventory::setRules(const game::ItemCatalog* catalog, game::ItemUser user) noexcept
{
    m_catalog = catalog;
    m_user = user;
}

void ClientInventory::reset()
{
    m_confirmed = {};
    m_view = {};
    m_pending.clear();
    m_sentTimes.clear();
    m_loaded = false;
}

bool ClientInventory::applyInventory(const vn::Inventory& msg, std::string& error)
{
    std::vector<game::StoredItem> items;
    items.reserve(msg.items.size());
    for (const vn::ItemEntry& e : msg.items)
    {
        if (e.itemId == 0)
            continue; // INVENTORY lists occupied slots only; tolerate an empty entry
        game::StoredItem s;
        s.id = e.itemId;
        s.place = placeOf(e);
        s.templateId = e.templateId;
        s.quantity = e.quantity;
        items.push_back(s);
    }
    game::Inventory inv;
    if (!game::Inventory::fromItems(items, inv, error))
        return false;
    m_confirmed = inv;
    m_pending.clear();
    m_loaded = true;
    rebuildView();
    return true;
}

bool ClientInventory::applyUpdate(const vn::ItemUpdate& msg)
{
    bool ok = true;
    for (const vn::ItemEntry& e : msg.items)
    {
        const game::ItemPlace p = placeOf(e);
        if (!game::isValidPlace(p))
        {
            ok = false;
            continue;
        }
        game::ItemStack& s = m_confirmed.at(p);
        if (e.itemId == 0)
            s = {};
        else
            s = {e.itemId, e.templateId, e.quantity};
    }
    // The oldest move still waiting for its effect is the one this update carries (§3: in order,
    // ITEM_UPDATE before ITEM_MOVE_RES(Ok); a refused move sends no update).
    for (Pending& p : m_pending)
    {
        if (!p.settled)
        {
            p.settled = true;
            break;
        }
    }
    rebuildView();
    return ok;
}

std::optional<ClientInventory::Answer> ClientInventory::applyResult(const vn::ItemMoveRes& msg)
{
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
        if (it->seq != msg.seq)
            continue;
        Answer a{it->seq, msg.result, it->from, it->to};
        m_pending.erase(it);
        rebuildView(); // a refused move's optimistic effect disappears here
        return a;
    }
    return std::nullopt;
}

ClientInventory::Begin ClientInventory::beginMove(game::ItemPlace from, game::ItemPlace to, double now, bool force)
{
    Begin b;
    if (!m_loaded || m_catalog == nullptr)
        return b;
    const game::ItemMovePlan plan = game::planItemMove(*m_catalog, m_view, m_user, from, to);
    b.result = plan.result;
    if (plan.result != game::ItemMoveResult::Ok && !force)
    {
        b.status = BeginStatus::Refused;
        return b;
    }
    while (!m_sentTimes.empty() && now - m_sentTimes.front() >= 1.0)
        m_sentTimes.pop_front();
    if (static_cast<int>(m_sentTimes.size()) >= kMaxMovesPerSecond)
    {
        b.status = BeginStatus::Throttled;
        return b;
    }
    m_sentTimes.push_back(now);
    Pending p;
    p.seq = m_nextSeq++;
    p.from = from;
    p.to = to;
    p.optimistic = plan.result == game::ItemMoveResult::Ok;
    m_pending.push_back(p);
    vn::ItemMoveReq req;
    req.seq = p.seq;
    req.fromContainer = from.container;
    req.fromSlot = from.slot;
    req.toContainer = to.container;
    req.toSlot = to.slot;
    b.request = req;
    b.status = BeginStatus::Sent;
    rebuildView();
    return b;
}

void ClientInventory::rebuildView()
{
    m_view = m_confirmed;
    if (m_catalog == nullptr)
        return;
    for (const Pending& p : m_pending)
    {
        if (p.settled || !p.optimistic)
            continue;
        const game::ItemMovePlan plan = game::planItemMove(*m_catalog, m_view, m_user, p.from, p.to);
        if (plan.result == game::ItemMoveResult::Ok)
            game::applyItemMove(m_view, plan);
    }
}

game::VisibleEquipment ClientInventory::visible() const noexcept
{
    if (m_catalog == nullptr)
        return {m_view.equipped(game::EquipSlot::MainHand).templateId,
                m_view.equipped(game::EquipSlot::OffHand).templateId};
    return game::visibleEquipment(*m_catalog, m_view);
}

std::optional<game::ItemPlace> ClientInventory::quickTarget(game::ItemPlace from) const
{
    if (m_catalog == nullptr || !game::isValidPlace(from))
        return std::nullopt;
    return game::quickMoveTarget(*m_catalog, m_view, from);
}

game::ItemMoveResult ClientInventory::check(game::ItemPlace from, game::ItemPlace to) const
{
    if (m_catalog == nullptr)
        return game::ItemMoveResult::ServerError;
    return game::planItemMove(*m_catalog, m_view, m_user, from, to).result;
}

} // namespace vo::client::world
