// WorldScene, shop half (M14 "Pazar", docs/PROTOCOL_SHOP.md §8): the merchants in the world (model,
// idle / talking / interact clips, name plate with title, overhead marker, hover highlight and tooltip,
// click or E walks into talk range and opens the shop), the shop window next to the inventory, buying
// (quantity box, confirmation), selling (right click / drag from the bag, quantity box, confirmation),
// the result texts, potions (use from the bag or the bar, the shared cooldown, ITEM_USED numbers) and
// the shop automation. The pure rules live in client/world/ShopView.h; this file wires them to the
// network, the input and the screen.
#include "client/scenes/WorldScene.h"

#include "client/net/GameClient.h"
#include "client/platform/Input.h"
#include "client/render/AssetCache.h"
#include "client/render/TextRenderer.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/InventoryWindow.h"
#include "client/scenes/ItemTooltip.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/ShopWindow.h"
#include "client/scenes/SkillWindow.h"
#include "client/ui/Ui.h"
#include "client/world/ItemData.h"
#include "client/world/TerrainQuery.h"
#include "shared/core/Log.h"
#include "shared/game/Shop.h"
#include "shared/game/Skills.h"
#include "shared/game/TerrainWalk.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;
using world::Vec3;

namespace {

constexpr const char* kMarkerPotion = "ui/shop/marker_potion.png";
constexpr const char* kMarkerSmith = "ui/shop/marker_smith.png";
constexpr double kShopWindowSec = 1.0;   // the server allows 10 shop requests a second ...
constexpr std::size_t kShopPerWindow = 8; // ... we stay below it (ITEM_USE_REQ has its own 10/s)
constexpr float kNpcTurnRate = 3.0f;      // rad/s while turning to the customer and back
constexpr float kShopFallbackClose = 9.5f; // the server closes at 8 m; if it did not (a leap), we do
// ITEM_UPDATEs of shop / potion requests settle no item move (ClientInventory::beginTake): their own
// seq ranges, apart from the loot takes'.
constexpr std::uint32_t kShopTakeTag = 0x80000000u;
constexpr std::uint32_t kUseTakeTag = 0x40000000u;

std::array<float, 4> rgba(std::uint32_t rgb, float a)
{
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f, a};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void WorldScene::requestShopAssets()
{
    render::AssetCache* assets = ctx().assets;
    if (assets == nullptr)
        return;
    // This zone's merchants only (the others are loaded by the scene of their zone).
    for (const game::NpcDef& n : game::npcs().npcs())
        if (n.zone == m_zoneId)
            assets->requestNpc(n.model);
    for (const char* p : {kMarkerPotion, kMarkerSmith, "ui/shop/akce.png", "ui/shop/tab_sat.png", "ui/shop/tab_al.png"})
        (void)ctx().resources.uiImage(p);
}

void WorldScene::buildShop()
{
    ShopWindow::Hooks hooks;
    hooks.buy = [this](std::uint16_t t) { shopBuy(t); };
    hooks.close = [this]() { closeShop(true, {}); };
    hooks.sellDragActive = [this]() { return m_invWindow != nullptr && m_invWindow->dragging(); };
    m_shopWindow = &ui().root().add<ShopWindow>(ctx().resources, m_shop, m_inventory, std::move(hooks));
    m_shopWindow->setAnchors(anchor::kTopLeft).setMargins({8.0f, 120.0f, 0.0f, 0.0f});
    m_shopWindow->setVisible(false);
    m_npcs.clear();
    m_shop.close(true);
    m_potionCd.clear();
    m_npcWalk = 0;
    m_npcHover = 0;
    m_npcHoverForced = 0;
    m_shopOpenedInventory = false;
    m_quantityDialog = nullptr;
    m_lastShopMessage.clear();
    m_shopSends.fill(-1e9);
    m_useSends.fill(-1e9);
    m_shopSeen = {};
}

const game::ItemTemplate* WorldScene::itemTemplate(std::uint16_t templateId) const
{
    const game::ItemCatalog* c = m_inventory.catalog();
    return c != nullptr ? c->find(templateId) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------------------------

bool WorldScene::shopBudget() const
{
    std::size_t recent = 0;
    for (const double t : m_shopSends)
        recent += (m_time - t) < kShopWindowSec ? 1 : 0;
    return recent < kShopPerWindow;
}

void WorldScene::noteShopSend()
{
    m_shopSends[m_shopSendNext] = m_time;
    m_shopSendNext = (m_shopSendNext + 1) % m_shopSends.size();
}

bool WorldScene::useBudget() const
{
    std::size_t recent = 0;
    for (const double t : m_useSends)
        recent += (m_time - t) < 1.0 ? 1 : 0;
    return recent < m_useSends.size();
}

void WorldScene::showShopMessage(std::string_view text, bool error)
{
    if (text.empty())
        return;
    m_lastShopMessage = std::string(text);
    if (m_shopWindow != nullptr && m_shop.isOpen())
        m_shopWindow->setMessage(std::string(text), error);
    showCombatMessage(text); // the message line above the bar, also when the window is closed
}

bool WorldScene::sendShopOpen(std::uint32_t npcEntity)
{
    if (m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked() || m_zoneChanging)
        return false;
    if (!shopBudget())
    {
        showShopMessage("Çok hızlı: biraz bekle", true);
        return false;
    }
    if (!ctx().net.send(vn::ShopOpenReq{npcEntity}))
        return false;
    noteShopSend();
    m_shop.beginOpen(npcEntity);
    VO_LOG_INFO("world: SHOP_OPEN_REQ {}", npcEntity);
    return true;
}

void WorldScene::requestTalk(std::uint32_t npcEntity)
{
    const world::NpcEntity* n = m_npcs.find(npcEntity);
    if (n == nullptr || m_phase != Phase::Ready)
        return;
    if (m_self.dead)
    {
        showShopMessage(world::shopResultText(vn::ShopResult::YouAreDead), true);
        return;
    }
    if (m_shop.isOpen() && m_shop.npc() == npcEntity)
        return; // already talking to it
    const float d = world::horizontalDistance(n->position, m_model.local().mover().position());
    if (d <= world::kNpcWalkStop + 0.3f)
    {
        m_npcWalk = 0;
        (void)sendShopOpen(npcEntity);
        return;
    }
    if (m_localAnim.seated())
        m_localAnim.standUp();
    m_chasing = false;
    m_queuedSkill = 0;
    m_lootWalk = 0;
    m_npcWalk = npcEntity;
    m_npcWalkReplan = 0.0f;
    VO_LOG_INFO("world: walking to merchant {} '{}' ({:.1f} m away) to talk", npcEntity, n->name, d);
}

bool WorldScene::npcInteract()
{
    if (m_phase != Phase::Ready || m_menuOpen || m_self.dead)
        return false;
    const std::uint32_t id = m_npcs.nearest(m_model.local().mover().position(), world::kNpcInteractSearch);
    if (id == 0)
        return false;
    if (m_shop.isOpen() && m_shop.npc() == id)
        return true; // (E again: nothing; the window is open)
    requestTalk(id);
    return true;
}

void WorldScene::closeShop(bool sendClose, std::string_view message)
{
    const std::uint32_t npc = m_shop.npc();
    if (npc != 0 && sendClose && !m_leavingWorld && !m_model.leaving() && !leaving() && !ctx().net.movesBlocked())
    {
        // Always goes out (one request; not held back by the budget).
        (void)ctx().net.send(vn::ShopCloseReq{npc});
        noteShopSend();
        VO_LOG_INFO("world: SHOP_CLOSE_REQ {}", npc);
    }
    const bool wasOpen = m_shop.isOpen();
    m_shop.close(false);
    if (m_quantityDialog != nullptr && !m_quantityDialog->closed())
        m_quantityDialog->close(false);
    m_quantityDialog = nullptr;
    if (m_shopWindow != nullptr)
        m_shopWindow->setVisible(false);
    if (wasOpen && m_shopOpenedInventory)
        setInventoryOpen(false);
    m_shopOpenedInventory = false;
    if (!message.empty())
        showShopMessage(message, false);
    if (wasOpen)
        layoutHud();
}

void WorldScene::forgetShop()
{
    m_npcWalk = 0;
    m_npcHover = 0;
    m_npcHoverForced = 0;
    m_shop.close(true); // requests in flight are forgotten (PROTOCOL_SHOP.md §2.2)
    if (m_quantityDialog != nullptr && !m_quantityDialog->closed())
        m_quantityDialog->close(false);
    m_quantityDialog = nullptr;
    if (m_shopWindow != nullptr)
        m_shopWindow->setVisible(false);
    if (m_shopOpenedInventory)
        setInventoryOpen(false);
    m_shopOpenedInventory = false;
    m_npcs.clear();
}

void WorldScene::sendShopBuy(std::uint16_t templateId, std::uint16_t quantity)
{
    if (!m_shop.isOpen() || m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked())
        return;
    if (!shopBudget())
    {
        showShopMessage("Çok hızlı: biraz bekle", true);
        return;
    }
    const auto req = m_shop.beginBuy(templateId, quantity);
    if (!req)
        return;
    if (!ctx().net.send(*req))
    {
        m_shop.abandon();
        return;
    }
    noteShopSend();
    m_inventory.beginTake(kShopTakeTag | req->seq); // its ITEM_UPDATEs settle no item move
    VO_LOG_INFO("world: SHOP_BUY_REQ seq {} npc {} item {} x {}", req->seq, req->npcId, templateId, quantity);
}

void WorldScene::sendShopSell(std::uint8_t bagSlot, std::uint16_t quantity)
{
    if (!m_shop.isOpen() || m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked())
        return;
    if (!shopBudget())
    {
        showShopMessage("Çok hızlı: biraz bekle", true);
        return;
    }
    const game::ItemStack& s = m_inventory.view().at(game::ItemPlace::bag(bagSlot));
    const auto req = m_shop.beginSell(bagSlot, s.templateId, quantity);
    if (!req)
        return;
    if (!ctx().net.send(*req))
    {
        m_shop.abandon();
        return;
    }
    noteShopSend();
    m_inventory.beginTake(kShopTakeTag | req->seq);
    VO_LOG_INFO("world: SHOP_SELL_REQ seq {} npc {} bag {} x {}", req->seq, req->npcId, bagSlot, quantity);
}

void WorldScene::shopBuy(std::uint16_t templateId, std::uint16_t quantity, bool direct)
{
    if (!m_shop.isOpen() || m_shop.busy())
        return;
    if (m_self.dead)
    {
        showShopMessage(world::shopResultText(vn::ShopResult::YouAreDead), true);
        return;
    }
    const world::ShopOffer* offer = m_shop.offer(templateId);
    const game::ItemTemplate* item = itemTemplate(templateId);
    if (direct || offer == nullptr || item == nullptr)
    {
        sendShopBuy(templateId, quantity == 0 ? 1 : quantity); // (the server answers NotInStock / BadQuantity)
        return;
    }
    const std::uint32_t npc = m_shop.npc();
    const std::uint32_t price = offer->price;
    if (world::buyNeedsQuantity(*item))
    {
        QuantityDesc d;
        d.title = item->name;
        d.prompt = std::string(world::kBuyQuantityPrompt);
        d.icon = ctx().resources.itemIcon(templateId);
        d.max = item->maxStack;
        d.initial = quantity == 0 ? 1 : quantity;
        d.unitPrice = price;
        d.onConfirm = [this, npc, templateId](std::uint16_t q) {
            m_quantityDialog = nullptr;
            if (m_shop.isOpen() && m_shop.npc() == npc)
                sendShopBuy(templateId, q);
        };
        d.onCancel = [this]() { m_quantityDialog = nullptr; };
        m_quantityDialog = &QuantityDialog::show(ui(), std::move(d));
        return;
    }
    if (world::buyNeedsConfirm(*item, price))
    {
        confirm(item->name, world::buyConfirmText(item->name, price), "Evet", "Hayır", [this, npc, templateId](bool yes) {
            if (yes && m_shop.isOpen() && m_shop.npc() == npc)
                sendShopBuy(templateId, 1);
        });
        return;
    }
    sendShopBuy(templateId, 1);
}

void WorldScene::shopSell(std::uint8_t bagSlot, std::uint16_t quantity)
{
    if (!m_shop.isOpen() || m_shop.busy() || bagSlot >= game::kBagSlots)
        return;
    if (m_self.dead)
    {
        showShopMessage(world::shopResultText(vn::ShopResult::YouAreDead), true);
        return;
    }
    const game::ItemStack s = m_inventory.view().at(game::ItemPlace::bag(bagSlot));
    if (s.empty())
    {
        showShopMessage(world::shopResultText(vn::ShopResult::NoItem), true);
        return;
    }
    const game::ItemTemplate* item = itemTemplate(s.templateId);
    if (item == nullptr)
    {
        sendShopSell(bagSlot, quantity == 0 ? s.quantity : quantity); // a hidden item: the server says why
        return;
    }
    const std::uint32_t unit = game::sellPrice(*item);
    if (unit == 0)
    {
        showShopMessage(world::shopResultText(vn::ShopResult::CannotSell), true);
        return;
    }
    const std::uint32_t npc = m_shop.npc();
    const std::uint16_t tmpl = s.templateId;
    // Confirms (worth asking) and sends; the stack must still be there.
    const auto finish = [this, npc, bagSlot, tmpl, item, unit](std::uint16_t q) {
        if (!m_shop.isOpen() || m_shop.npc() != npc)
            return;
        const game::ItemStack& now = m_inventory.view().at(game::ItemPlace::bag(bagSlot));
        if (now.empty() || now.templateId != tmpl)
            return;
        if (world::sellNeedsConfirm(*item))
        {
            confirm(item->name, world::sellConfirmText(item->name, q, static_cast<std::uint64_t>(unit) * q), "Evet", "Hayır",
                    [this, npc, bagSlot, q](bool yes) {
                        if (yes && m_shop.isOpen() && m_shop.npc() == npc)
                            sendShopSell(bagSlot, q);
                    });
            return;
        }
        sendShopSell(bagSlot, q);
    };
    if (quantity == 0 && world::sellNeedsQuantity(s.quantity))
    {
        QuantityDesc d;
        d.title = item->name;
        d.prompt = std::string(world::kSellQuantityPrompt);
        d.icon = ctx().resources.itemIcon(tmpl);
        d.max = s.quantity;
        d.initial = s.quantity; // default: the whole stack
        d.unitPrice = unit;
        d.sell = true;
        d.onConfirm = [this, finish](std::uint16_t q) {
            m_quantityDialog = nullptr;
            finish(q);
        };
        d.onCancel = [this]() { m_quantityDialog = nullptr; };
        m_quantityDialog = &QuantityDialog::show(ui(), std::move(d));
        return;
    }
    finish(quantity == 0 ? s.quantity : quantity);
}

void WorldScene::usePotion(std::uint16_t templateId, std::uint8_t bagSlot)
{
    if (m_phase != Phase::Ready || m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked())
        return;
    const game::ItemTemplate* item = itemTemplate(templateId);
    const std::uint32_t have = bagSlot == vn::kItemUseAnyStack
                                   ? world::bagCount(m_inventory.view(), templateId)
                                   : (bagSlot < game::kBagSlots &&
                                              m_inventory.view().at(game::ItemPlace::bag(bagSlot)).templateId == templateId
                                          ? m_inventory.view().at(game::ItemPlace::bag(bagSlot)).quantity
                                          : 0u);
    if (have == 0)
    {
        // §8.4: with none left a line and no request.
        ++m_shopSeen.useRefused;
        showShopMessage(world::itemUseResultText(vn::ItemUseResult::NoItem, item), true);
        return;
    }
    if (!useBudget())
    {
        showShopMessage("Çok hızlı: biraz bekle", true);
        return;
    }
    const vn::ItemUseReq req{++m_itemUseSeq, templateId, bagSlot};
    if (!ctx().net.send(req))
        return;
    m_useSends[m_useSendNext] = m_time;
    m_useSendNext = (m_useSendNext + 1) % m_useSends.size();
    m_inventory.beginTake(kUseTakeTag | req.seq);
    VO_LOG_INFO("world: ITEM_USE_REQ seq {} item {} slot {}", req.seq, templateId,
                bagSlot == vn::kItemUseAnyStack ? std::string("any") : std::to_string(bagSlot));
}

// ---------------------------------------------------------------------------------------------
// Inventory hooks
// ---------------------------------------------------------------------------------------------

bool WorldScene::inventoryRightClick(game::ItemPlace place)
{
    if (!place.isBag())
        return false; // equipment: unequip as before (it can be sold from the bag afterwards)
    if (m_shop.isOpen())
    {
        shopSell(place.slot);
        return true;
    }
    return inventoryDoubleClick(place);
}

bool WorldScene::inventoryDoubleClick(game::ItemPlace place)
{
    if (!place.isBag())
        return false;
    const game::ItemStack& s = m_inventory.view().at(place);
    const game::ItemTemplate* item = s.empty() ? nullptr : itemTemplate(s.templateId);
    if (item == nullptr || !item->isConsumable())
        return false;
    usePotion(s.templateId, place.slot); // §8.4: a potion is drunk instead of equipped
    return true;
}

bool WorldScene::inventoryDropOutside(game::ItemPlace from, float x, float y)
{
    // Onto the shop window: sell (an equipped item: unequip first, no request).
    if (m_shop.isOpen() && m_shopWindow != nullptr && m_shopWindow->isVisible() && m_shopWindow->rect().contains(x, y))
    {
        if (!from.isBag())
        {
            showShopMessage(world::kUnequipFirstText, true);
            return true;
        }
        shopSell(from.slot);
        return true;
    }
    // Onto a bar slot: a potion goes on the bar (SKILL_BAR_SET_REQ with 0x8000 | id).
    int slot = m_skillBar != nullptr ? m_skillBar->slotAt(x, y) : -1;
    if (slot < 0 && m_skillsOpen && m_skillWindow != nullptr && m_skillWindow->barSlots() != nullptr)
        slot = m_skillWindow->barSlots()->slotAt(x, y);
    if (slot < 0)
        return false;
    const game::ItemStack& s = m_inventory.view().at(from);
    const game::ItemTemplate* item = s.empty() ? nullptr : itemTemplate(s.templateId);
    if (item == nullptr || !item->isConsumable() || item->id > game::kMaxConsumableTemplateId)
    {
        showShopMessage(world::kBarOnlyPotionsText, true);
        return true;
    }
    VO_LOG_INFO("world: potion {} dragged onto bar slot {}", item->id, slot + 1);
    sendSkillBar(world::barWithPlaced(m_skillBook.bar(), static_cast<std::size_t>(slot), game::barItemSlot(item->id)));
    return true;
}

void WorldScene::inventoryExtraTip(game::ItemPlace place, std::vector<world::TipLine>& lines)
{
    if (!m_shop.isOpen())
        return;
    const game::ItemStack& s = m_inventory.view().at(place);
    const game::ItemTemplate* item = s.empty() ? nullptr : itemTemplate(s.templateId);
    if (item == nullptr)
        return;
    if (!place.isBag())
    {
        lines.push_back({std::string(world::kUnequipFirstText), world::TipTone::Dim, 0, false});
        return;
    }
    lines.push_back({world::sellTipText(*item, s.quantity),
                     game::sellPrice(*item) > 0 ? world::TipTone::Value : world::TipTone::Bad, 0, false});
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

bool WorldScene::handleShopMessage(const net::NetEvent& e)
{
    const auto op = static_cast<vn::Opcode>(e.opcode);
    const auto malformed = [&]() {
        VO_LOG_ERROR("world: malformed {} payload ({} bytes)", vn::opcodeName(op), e.payload.size());
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return true;
    };
    switch (op)
    {
    case vn::Opcode::EntitySpawn:
    {
        // Entity type 4: a merchant (kept here, not in the WorldModel: never a target, a remote or a
        // monster). Other types go on to the model.
        vn::EntitySpawn m;
        if (!vn::decodeMessage(e.payload, m) || m.entityType != vn::kEntityTypeNpc)
            return false;
        (void)m_npcs.spawn(m, game::npcs(), m_phase == Phase::Ready ? m_terrain : nullptr);
        const world::NpcEntity* n = m_npcs.find(m.entityId);
        VO_LOG_INFO("world: merchant {} '{}' (npc {}{}) at ({:.1f}, {:.1f})", m.entityId, m.name, m.templateId,
                    n != nullptr && n->def == nullptr ? ", unknown to npcs.toml" : "", m.x, m.z);
        return true;
    }
    case vn::Opcode::EntityDespawn:
    {
        vn::EntityDespawn m;
        if (!vn::decodeMessage(e.payload, m))
            return false;
        if (!m_npcs.despawn(m.entityId))
            return false;
        if (m_npcWalk == m.entityId)
            m_npcWalk = 0;
        if (m_npcHover == m.entityId)
            m_npcHover = 0;
        VO_LOG_INFO("world: merchant {} left the view", m.entityId);
        return true;
    }
    case vn::Opcode::ShopOpenRes:
    {
        vn::ShopOpenRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        VO_LOG_INFO("world: SHOP_OPEN_RES {} result {} ({} entr(y/ies))", m.npcId, static_cast<int>(m.result), m.entries.size());
        if (!m_shop.finishOpen(m))
            return true; // stale
        if (m.result != vn::ShopResult::Ok)
        {
            ++m_shopSeen.openRefused;
            showShopMessage(world::shopResultText(m.result), true);
            return true;
        }
        const world::NpcEntity* n = m_npcs.find(m.npcId);
        if (n == nullptr || m_self.dead)
        {
            m_shop.close(false);
            return true;
        }
        ++m_shopSeen.opened;
        m_shopNpcName = n->name;
        if (m_shopWindow != nullptr)
        {
            m_shopWindow->open(n->name, n->title(), n->def != nullptr ? std::string_view(n->def->greeting) : "");
            m_shopWindow->setCoins(m_coins, m_coinsKnown);
            m_shopWindow->setVisible(true);
        }
        // §8.2: the inventory opens next to it (selling is a right click there).
        if (!m_invOpen)
        {
            setInventoryOpen(true);
            m_shopOpenedInventory = m_invOpen;
        }
        layoutHud();
        return true;
    }
    case vn::Opcode::ShopClosed:
    {
        vn::ShopClosed m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        VO_LOG_INFO("world: SHOP_CLOSED {} reason {}", m.npcId, static_cast<int>(m.reason));
        if (m.npcId == m_shop.npc())
        {
            ++m_shopSeen.closedByServer;
            const std::string text = world::shopClosedText(m.reason, m_shopNpcName);
            closeShop(false, {});
            if (!text.empty())
                showShopMessage(text, false);
        }
        return true;
    }
    case vn::Opcode::ShopBuyRes:
    {
        vn::ShopBuyRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_inventory.endTake(kShopTakeTag | m.seq);
        const auto r = m_shop.finishBuy(m);
        VO_LOG_INFO("world: SHOP_BUY_RES seq {} result {} item {} x {} cost {}{}", m.seq, static_cast<int>(m.result),
                    m.templateId, m.quantity, m.cost, r ? "" : " (not ours / forgotten)");
        if (!r)
            return true;
        const game::ItemTemplate* item = itemTemplate(m.templateId);
        if (m.result == vn::ShopResult::Ok)
        {
            ++m_shopSeen.bought;
            showShopMessage(world::buyOkText(item != nullptr ? item->name : std::format("Eşya #{}", m.templateId),
                                             m.quantity, m.cost),
                            false);
            if (world::NpcEntity* n = m_npcs.find(r->npc); n != nullptr && n->animator.playInteract())
                ++m_shopSeen.interacts;
        }
        else
        {
            ++m_shopSeen.buyRefused;
            showShopMessage(world::shopResultText(m.result), true);
        }
        return true;
    }
    case vn::Opcode::ShopSellRes:
    {
        vn::ShopSellRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_inventory.endTake(kShopTakeTag | m.seq);
        const auto r = m_shop.finishSell(m);
        VO_LOG_INFO("world: SHOP_SELL_RES seq {} result {} item {} x {} gained {}{}", m.seq, static_cast<int>(m.result),
                    m.templateId, m.quantity, m.gained, r ? "" : " (not ours / forgotten)");
        if (!r)
            return true;
        const game::ItemTemplate* item = itemTemplate(m.templateId != 0 ? m.templateId : r->templateId);
        if (m.result == vn::ShopResult::Ok)
        {
            ++m_shopSeen.sold;
            showShopMessage(world::sellOkText(item != nullptr ? item->name : std::string("Eşya"), m.quantity, m.gained), false);
            if (world::NpcEntity* n = m_npcs.find(r->npc); n != nullptr && n->animator.playInteract())
                ++m_shopSeen.interacts;
        }
        else
        {
            ++m_shopSeen.sellRefused;
            showShopMessage(world::shopResultText(m.result), true);
        }
        return true;
    }
    case vn::Opcode::ItemUseRes:
    {
        vn::ItemUseRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_inventory.endTake(kUseTakeTag | m.seq);
        VO_LOG_INFO("world: ITEM_USE_RES seq {} item {} {}", m.seq, m.templateId, vn::itemUseResultName(m.result));
        if (m.result != vn::ItemUseResult::Ok)
        {
            ++m_shopSeen.useRefused;
            showShopMessage(world::itemUseResultText(m.result, itemTemplate(m.templateId)), true);
        }
        return true;
    }
    case vn::Opcode::ItemCooldown:
    {
        vn::ItemCooldown m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_potionCd.set(m.remainingMs, m.totalMs, m_time);
        ++m_shopSeen.cooldowns;
        VO_LOG_INFO("world: ITEM_COOLDOWN {}/{} ms", m.remainingMs, m.totalMs);
        return true;
    }
    case vn::Opcode::ItemUsed:
    {
        vn::ItemUsed m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        const bool own = m.entityId == m_model.localEntityId();
        Vec3 at{};
        bool known = false;
        if (own)
        {
            at = localDrawPosition() + Vec3{0.0f, 2.0f, 0.0f};
            known = true;
            ++m_shopSeen.uses;
        }
        else if (const world::RemoteEntity* r = m_model.find(m.entityId))
        {
            at = headPoint(*r) + Vec3{0.0f, 0.35f, 0.0f};
            known = true;
            ++m_shopSeen.othersUsed;
        }
        VO_LOG_INFO("world: ITEM_USED by {} item {} +{} hp +{} mp{}", m.entityId, m.templateId, m.hpRestored, m.mpRestored,
                    own ? " (us)" : "");
        // §8.4: green "+hp", blue "+mp" over the drinker (both when a potion restores both).
        if (known && m.hpRestored > 0)
            m_numbers.add(world::FloatKind::Heal, m.hpRestored, at);
        if (known && m.mpRestored > 0)
            m_numbers.add(world::FloatKind::Mana, m.mpRestored, at + Vec3{0.0f, m.hpRestored > 0 ? 0.3f : 0.0f, 0.0f});
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------

void WorldScene::bindNpcAnimators()
{
    render::AssetCache& cache = *ctx().assets;
    for (world::NpcEntity& n : m_npcs.entities())
    {
        if (n.def == nullptr)
            continue;
        const render::GpuMesh* mesh = cache.mesh(render::AssetCache::npcMeshPath(n.def->model));
        const world::CharacterClips* clips = cache.npcClips(n.def->model);
        if (mesh == nullptr || !mesh->cpu || clips == nullptr || (n.animatorMesh == mesh && n.animatorClips == clips))
            continue;
        n.animator.bind(mesh->cpu.get(), clips);
        n.animatorMesh = mesh;
        n.animatorClips = clips;
        // Merchants side by side do not breathe in step.
        n.animator.update(0.37f * static_cast<float>(n.id % 11u), 0.0f, false);
    }
}

world::Vec3 WorldScene::npcHeadPoint(const world::NpcEntity& n) const
{
    float height = 1.85f;
    if (n.def != nullptr)
        if (const render::GpuMesh* mesh = ctx().assets->mesh(render::AssetCache::npcMeshPath(n.def->model));
            mesh != nullptr && mesh->cpu)
            height = mesh->cpu->boundsMax.y;
    return n.position + Vec3{0.0f, height, 0.0f};
}

std::uint32_t WorldScene::pickNpcAt(int x, int y, float* distance) const
{
    if (m_npcs.size() == 0)
        return 0;
    world::Mat4 inv;
    if (!assets::invert(currentViewProj(), inv))
        return 0;
    const world::Ray ray = world::screenRay(inv, static_cast<float>(x), static_cast<float>(y), ctx().viewportWidth,
                                            ctx().viewportHeight);
    std::vector<world::PickCandidate> c;
    for (const world::NpcEntity& n : m_npcs.entities())
        c.push_back({n.id, n.position, 0.45f, npcHeadPoint(n).y - n.position.y});
    const std::uint32_t id = world::pickEntity(ray, c, 120.0f);
    if (id == 0)
        return 0;
    const world::NpcEntity* n = m_npcs.find(id);
    Vec3 hit{};
    if (n != nullptr && m_terrain != nullptr && world::raycastTerrain(*m_terrain, ray, 120.0f, hit) &&
        assets::length(hit - ray.origin) + 1.0f < assets::length(n->position - ray.origin))
        return 0; // a hill in front of it
    if (distance != nullptr && n != nullptr)
        *distance = assets::length(n->position - ray.origin);
    return id;
}

void WorldScene::updateShop(float dt)
{
    world::LocalMover& mover = m_model.local().mover();
    const Vec3 me = mover.position();
    bindNpcAnimators();

    // Hover (not over a window, not while turning the camera).
    m_npcHover = 0;
    if (m_npcHoverForced != 0 && m_npcs.find(m_npcHoverForced) != nullptr)
        m_npcHover = m_npcHoverForced;
    else if (!m_dragging && !m_menuOpen && (ui().hover() == nullptr || ui().hover() == &ui().root() ||
                                            ui().hover() == m_combatHud))
        m_npcHover = pickNpcAt(ctx().input.mouseX(), ctx().input.mouseY());

    // Walking into talk range.
    if (m_npcWalk != 0)
    {
        const world::NpcEntity* n = m_npcs.find(m_npcWalk);
        if (n == nullptr || m_self.dead)
            m_npcWalk = 0;
        else
        {
            const float d = world::horizontalDistance(n->position, me);
            if (d <= world::kNpcWalkStop)
            {
                const std::uint32_t id = m_npcWalk;
                m_npcWalk = 0;
                m_route.clear();
                m_routeNext = 0;
                mover.clearDestination();
                (void)sendShopOpen(id);
            }
            else
            {
                m_npcWalkReplan -= dt;
                if (m_npcWalkReplan <= 0.0f && !m_localAnim.seated())
                {
                    m_npcWalkReplan = 1.0f;
                    // Aim at the point in front of the merchant (its counter side), not into it.
                    const Vec3 front = n->position + world::forwardFromYaw(n->yaw) * 1.6f;
                    const Vec3 goal = m_terrain != nullptr && m_terrain->isWalkable(front.x, front.z) ? front : n->position;
                    if (m_terrain == nullptr || game::isSegmentWalkable(*m_terrain, me.x, me.z, goal.x, goal.z))
                    {
                        m_route.clear();
                        m_routeNext = 0;
                        mover.setDestination(goal.x, goal.z);
                    }
                    else if (!routePending())
                    {
                        std::string ignored;
                        if (!setRouteNear(me, goal.x, goal.z, 2.5f, ignored))
                            mover.setDestination(goal.x, goal.z);
                    }
                }
            }
        }
    }

    // The open window follows its merchant (gone from the view, walked far away, dead).
    if (m_shop.isOpen())
    {
        const world::NpcEntity* n = m_npcs.find(m_shop.npc());
        if (n == nullptr)
            closeShop(false, {});
        else if (world::horizontalDistance(n->position, me) > kShopFallbackClose)
            closeShop(true, world::shopClosedText(vn::ShopClosedReason::OutOfRange, n->name));
        else if (m_shopWindow != nullptr)
            m_shopWindow->setCoins(m_coins, m_coinsKnown);
    }

    // Animation: idle_talking while their shop is open with us, turned to the customer, back after.
    for (world::NpcEntity& n : m_npcs.entities())
    {
        n.age += dt;
        const bool talking = m_shop.isOpen() && m_shop.npc() == n.id;
        n.animator.setTalking(talking);
        float rest = n.def != nullptr ? n.def->yaw : n.yaw;
        if (talking && world::horizontalDistance(n.position, me) > 0.5f)
            rest = std::atan2(me.x - n.position.x, me.z - n.position.z);
        const float delta = world::angleDelta(n.yaw, rest);
        const float step = kNpcTurnRate * dt;
        n.yaw = world::wrapAngle(n.yaw + std::clamp(delta, -step, step));
        if (n.animator.bound())
            n.animator.update(dt, 0.0f, false);
    }
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

void WorldScene::addNpcDraws()
{
    render::AssetCache& cache = *ctx().assets;
    render::WorldRenderer& r = *ctx().worldRenderer;
    for (world::NpcEntity& n : m_npcs.entities())
    {
        if (n.def == nullptr)
            continue;
        const bool hovered = n.id == m_npcHover;
        const bool open = m_shop.isOpen() && m_shop.npc() == n.id;
        // Hover: a soft brightening (the level-up glow path, weaker).
        addCharacterDraw(cache.mesh(render::AssetCache::npcMeshPath(n.def->model)), n.animator, n.position, n.yaw,
                         n.age / 0.4f, 1.0f, 0, 0, 0, 0, 1.0f, hovered ? 0.28f : 0.0f);
        if ((hovered || open) && m_terrain != nullptr)
        {
            const Vec3 ground{n.position.x, m_terrain->heightAt(n.position.x, n.position.z), n.position.z};
            render::DecalDraw ring;
            ring.center = ground;
            ring.normal = m_terrain->normalAt(n.position.x, n.position.z);
            const float breathe = 0.5f + 0.5f * std::sin(static_cast<float>(m_time) * 3.0f);
            ring.radius = 0.78f + 0.05f * breathe;
            ring.color = rgba(world::kNpcNameRgb, open ? 0.8f : 0.55f + 0.25f * breathe);
            ring.ring = true;
            r.addDecal(ring);
        }
    }
}

void WorldScene::drawNpcOverlay(const world::Mat4& vp)
{
    if (m_npcs.size() == 0)
        return;
    auto& batch = ctx().batch;
    const float s = ui().scale();
    const float w = ctx().viewportWidth;
    const float h = ctx().viewportHeight;
    const Vec3 eye = m_camera.eye();
    const render::Font& nameFont = ctx().fonts.get(render::FontWeight::Bold, 15.0f * s);
    const render::Font& titleFont = ctx().fonts.get(render::FontWeight::Regular, 12.5f * s);
    const auto hidden = [&](Vec3 p) {
        const Vec3 d = p - eye;
        const float len = assets::length(d);
        if (len < 0.3f)
            return false;
        if (m_terrain != nullptr && !world::segmentClearOfTerrain(*m_terrain, eye, p, 0.1f, 2.0f))
            return true;
        const Vec3 end = eye + d * ((len - 0.25f) / len);
        for (const world::LabelOccluder& o : m_propOccluders)
            if (world::segmentHitsOccluder(o, eye, end))
                return true;
        return false;
    };
    for (const world::NpcEntity& n : m_npcs.entities())
    {
        const Vec3 head = npcHeadPoint(n);
        const float dist = assets::length(head - eye);
        if (dist > 70.0f)
            continue;
        world::Vec2 sp{};
        if (!world::projectToScreen(vp, head + Vec3{0.0f, 0.16f, 0.0f}, w, h, sp))
            continue;
        if (sp.x < -200.0f || sp.x > w + 200.0f || sp.y < -80.0f || sp.y > h + 80.0f)
            continue;
        if (hidden(head + Vec3{0.0f, 0.3f, 0.0f}))
            continue;
        // Name plate: the title line just over the head, the name above it (merchant colours).
        const float labelAlpha = 1.0f - std::clamp((dist - 28.0f) / (70.0f - 28.0f), 0.0f, 1.0f);
        render::TextStyle ts;
        ts.outline = true;
        ts.outlineColor = Color::hex(0x000000, 0.85f * labelAlpha);
        const std::string title = world::npcTitlePlate(n.title());
        float top = std::round(sp.y);
        if (!title.empty())
        {
            ts.color = Color::hex(world::kNpcTitleRgb, labelAlpha);
            const float tw = render::measureText(titleFont, title);
            top -= titleFont.lineHeight();
            render::drawText(batch, titleFont, title, {std::round(sp.x - tw * 0.5f), top}, ts);
        }
        ts.color = Color::hex(world::kNpcNameRgb, labelAlpha);
        const float nw = render::measureText(nameFont, n.name);
        top -= nameFont.lineHeight() - std::round(1.0f * s);
        render::drawText(batch, nameFont, n.name, {std::round(sp.x - nw * 0.5f), top}, ts);

        // The overhead marker (docs/SHOP_ART.md §1): ~0.55 m, its pin tip 0.45 m over the head - but
        // always above the name plate on screen.
        if (dist > world::kNpcMarkerHidden)
            continue;
        const render::Texture2D* marker = ctx().resources.uiImage(n.sellsPotions() ? kMarkerPotion : kMarkerSmith);
        if (marker == nullptr)
            continue;
        world::Vec2 mc{};
        world::Vec2 mb{};
        if (!world::projectToScreen(vp, head + Vec3{0.0f, world::kNpcMarkerLift, 0.0f}, w, h, mc) ||
            !world::projectToScreen(vp, head + Vec3{0.0f, world::kNpcMarkerLift - world::kNpcMarkerWorldSize * 0.5f, 0.0f}, w,
                                    h, mb))
            continue;
        float size = std::clamp(2.0f * std::fabs(mb.y - mc.y), world::kNpcMarkerMinPx * s, world::kNpcMarkerMaxPx * s);
        size = std::round(size);
        const float bob = std::round(2.0f * s * std::sin(static_cast<float>(m_time) * 2.2f + static_cast<float>(n.id)));
        float bottom = std::min(mc.y + size * 0.5f, top - std::round(3.0f * s)) + bob;
        const float alpha = 1.0f - std::clamp((dist - world::kNpcMarkerFullAlpha) /
                                                  (world::kNpcMarkerHidden - world::kNpcMarkerFullAlpha),
                                              0.0f, 1.0f);
        batch.drawImage(*marker, Rect{std::round(mc.x - size * 0.5f), std::round(bottom - size), size, size},
                        Color{1.0f, 1.0f, 1.0f, alpha});
    }

    // The hover tooltip next to the cursor ("<Ad> — <Unvan>", "Konuşmak için tıkla").
    if (const world::NpcEntity* n = m_npcHover != 0 ? m_npcs.find(m_npcHover) : nullptr;
        n != nullptr && !ui().hasModal() && !m_menuOpen)
    {
        const std::vector<std::string> tip = world::npcTipLines(n->name, n->title());
        std::vector<ShownTipLine> lines;
        lines.push_back({tip[0], Color::hex(world::kNpcNameRgb), true, false});
        lines.push_back({tip[1], theme().textDim, false, false});
        ui::DrawContext dc{batch, ctx().fonts, theme(), s, m_time};
        float mx = static_cast<float>(ctx().input.mouseX());
        float my = static_cast<float>(ctx().input.mouseY());
        if (m_npcHoverForced != 0)
        {
            world::Vec2 p{};
            if (world::projectToScreen(vp, npcHeadPoint(*n), w, h, p))
            {
                mx = p.x;
                my = p.y;
            }
        }
        drawTooltipBox(dc, lines, Rect{mx + 14.0f * s, my + 10.0f * s, 1.0f, 1.0f}, w, h);
    }
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::shopStates(std::vector<std::string>& out) const
{
    const auto counter = [&out](const char* name, std::size_t n) {
        for (std::size_t i = 1; i <= std::min<std::size_t>(n, 12); ++i)
            out.emplace_back(name + std::to_string(i));
    };
    counter("npcs", m_npcs.size());
    out.emplace_back(m_shop.isOpen() ? "shopopen" : "shopclosed");
    if (m_shop.opening() != 0)
        out.emplace_back("shopopening");
    out.emplace_back(m_shop.busy() ? "shopbusy" : "shopidle");
    if (m_npcWalk != 0)
        out.emplace_back("npcwalking");
    if (m_npcHover != 0)
        out.emplace_back("npchover");
    if (m_quantityDialog != nullptr)
        out.emplace_back("quantitybox");
    if (m_potionCd.active(m_time))
        out.emplace_back("potioncd");
    else
        out.emplace_back("potionready");
    for (const world::NpcEntity& n : m_npcs.entities())
    {
        if (n.animator.state() == world::AnimState::IdleTalking)
            out.emplace_back("npctalking");
        if (n.animator.action() == world::AnimAction::Interact)
            out.emplace_back("npcinteract");
        if (n.animator.bound())
            out.emplace_back("npcanimated");
    }
    counter("shopopened", m_shopSeen.opened);
    counter("shoprefused", m_shopSeen.openRefused);
    counter("bought", m_shopSeen.bought);
    counter("buyrefused", m_shopSeen.buyRefused);
    counter("sold", m_shopSeen.sold);
    counter("sellrefused", m_shopSeen.sellRefused);
    counter("shopclosedby", m_shopSeen.closedByServer);
    counter("potionuses", m_shopSeen.uses);
    counter("potionrefused", m_shopSeen.useRefused);
    counter("otherpotions", m_shopSeen.othersUsed);
    counter("potioncds", m_shopSeen.cooldowns);
    counter("npcinteracts", m_shopSeen.interacts);
}

bool WorldScene::shopAutomation(std::string_view command, bool& handled, std::string& error)
{
    handled = false;
    std::string parseError;
    const auto cmd = world::parseShopCommand(command, parseError);
    if (!cmd)
    {
        if (parseError.empty())
            return true;
        handled = true;
        error = parseError;
        return false;
    }
    handled = true;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    const Vec3 me = m_model.local().mover().position();
    const auto npcByName = [&](const std::string& name) -> std::uint32_t {
        if (name == "nearest")
            return m_npcs.nearest(me, 1e9f);
        return m_npcs.byName(name);
    };
    switch (cmd->verb)
    {
    case world::ShopVerb::Talk:
    {
        const std::uint32_t id = npcByName(cmd->text);
        if (id == 0)
            return fail(std::format("no merchant '{}' in view ({} merchant(s))", cmd->text, m_npcs.size()));
        requestTalk(id);
        return true;
    }
    case world::ShopVerb::OpenForce:
    {
        const std::uint32_t id = npcByName(cmd->text);
        if (id == 0)
            return fail(std::format("no merchant '{}' in view", cmd->text));
        (void)sendShopOpen(id);
        return true;
    }
    case world::ShopVerb::WalkTo:
    {
        const std::uint32_t id = npcByName(cmd->text);
        const world::NpcEntity* n = id != 0 ? m_npcs.find(id) : nullptr;
        if (n == nullptr)
            return fail(std::format("no merchant '{}' in view", cmd->text));
        const float d = world::horizontalDistance(n->position, me);
        const float stop = static_cast<float>(cmd->lo);
        const float k = d > stop ? (d - stop) / d : 0.0f;
        return setRouteNear(me, me.x + (n->position.x - me.x) * k, me.z + (n->position.z - me.z) * k, 2.0f, error);
    }
    case world::ShopVerb::Buy:
        if (!m_shop.isOpen())
            return fail("shopbuy: no shop open");
        if (m_shop.busy())
            return fail("shopbuy: a request is in flight");
        shopBuy(static_cast<std::uint16_t>(cmd->a), cmd->hasB ? static_cast<std::uint16_t>(cmd->b) : std::uint16_t{0});
        return true;
    case world::ShopVerb::BuyForce:
        if (!m_shop.isOpen())
            return fail("shopbuyforce: no shop open");
        shopBuy(static_cast<std::uint16_t>(cmd->a), static_cast<std::uint16_t>(cmd->b), true);
        return true;
    case world::ShopVerb::Sell:
        if (!m_shop.isOpen())
            return fail("shopsell: no shop open");
        shopSell(static_cast<std::uint8_t>(cmd->a), cmd->hasB ? static_cast<std::uint16_t>(cmd->b) : std::uint16_t{0});
        return true;
    case world::ShopVerb::Close:
        if (!m_shop.isOpen())
            return fail("shopclose: no shop open");
        closeShop(true, {});
        return true;
    case world::ShopVerb::ExpectOpen:
        if (cmd->text == "none")
        {
            if (m_shop.isOpen())
                return fail(std::format("a shop is open ({})", m_shopNpcName));
            return true;
        }
        if (!m_shop.isOpen() || m_shopNpcName != cmd->text)
            return fail(std::format("the open shop is '{}', expected '{}'", m_shop.isOpen() ? m_shopNpcName : "none", cmd->text));
        return true;
    case world::ShopVerb::ExpectMessage:
        if (m_lastShopMessage.find(cmd->text) == std::string::npos)
            return fail(std::format("last shop message is '{}', expected '{}'", m_lastShopMessage, cmd->text));
        return true;
    case world::ShopVerb::ExpectCount:
    {
        const SlotView v = slotView(cmd->a - 1);
        if (v.item == nullptr)
            return fail(std::format("bar slot {} holds no potion", cmd->a));
        if (v.count != cmd->b)
            return fail(std::format("bar slot {} shows {} {}, expected {}", cmd->a, v.count, v.item->name, cmd->b));
        return true;
    }
    case world::ShopVerb::ExpectCooldown:
    {
        const double left = m_potionCd.remaining(m_time);
        if (left < cmd->lo - 1e-3 || left > cmd->hi + 1e-3)
            return fail(std::format("potion cooldown {:.2f} s, expected {:.2f}..{:.2f}", left, cmd->lo, cmd->hi));
        return true;
    }
    case world::ShopVerb::Use:
        usePotion(static_cast<std::uint16_t>(cmd->a), cmd->hasB ? static_cast<std::uint8_t>(cmd->b) : vn::kItemUseAnyStack);
        return true;
    case world::ShopVerb::BarItem:
        sendSkillBar(world::barWithPlaced(m_skillBook.bar(), cmd->a - 1,
                                          cmd->b == 0 ? std::uint16_t{0} : game::barItemSlot(static_cast<std::uint16_t>(cmd->b))));
        return true;
    case world::ShopVerb::Hover:
    {
        if (cmd->text == "none")
        {
            m_npcHoverForced = 0;
            return true;
        }
        const std::uint32_t id = npcByName(cmd->text);
        if (id == 0)
            return fail(std::format("no merchant '{}' in view", cmd->text));
        m_npcHoverForced = id;
        return true;
    }
    case world::ShopVerb::Log:
    {
        VO_LOG_INFO("world: shop: {} merchant(s), open {} ('{}'), busy {}, Akçe {}, potion cooldown {:.1f} s", m_npcs.size(),
                    m_shop.npc(), m_shopNpcName, m_shop.busy(), m_coins, m_potionCd.remaining(m_time));
        for (const world::NpcEntity& n : m_npcs.entities())
            VO_LOG_INFO("world:   merchant {} '{}' npc {} at ({:.1f}, {:.1f}) {:.1f} m, anim {} ({})", n.id, n.name, n.npcId,
                        n.position.x, n.position.z, world::horizontalDistance(n.position, me),
                        world::animStateName(n.animator.state()), n.animator.bound() ? "bound" : "unbound");
        for (const world::ShopOffer& o : m_shop.offers())
            VO_LOG_INFO("world:   offer {} for {} Akçe", o.templateId, o.price);
        return true;
    }
    }
    return true;
}

} // namespace vo::client::scenes
