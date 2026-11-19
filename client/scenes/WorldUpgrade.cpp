// WorldScene, upgrade half (M16 "Tav", docs/PROTOCOL_UPGRADE.md §7): the "Yükselt" tab of a smith's shop
// window - an item put into the section (a right click on a bag item or a drop on the window while the tab is
// chosen), the attempt (a confirmation first when a failure destroys the item: +6 and above), UPGRADE_RES with
// its result line and effect - and the upgrade automation. The pure rules live in client/world/UpgradeView.h
// and shared/game/Upgrade.h; this file wires them to the network, the input and the shop window.
#include "client/scenes/WorldScene.h"

#include "client/net/GameClient.h"
#include "client/scenes/ShopWindow.h"
#include "client/scenes/UpgradePanel.h"
#include "client/ui/Ui.h"
#include "shared/core/Log.h"
#include "shared/net/Messages.h"

#include <format>

namespace vo::client::scenes {

namespace vn = vo::net;

namespace {

// ITEM_UPDATEs of an upgrade settle no item move (ClientInventory::beginTake): its own seq range.
constexpr std::uint32_t kUpgradeTakeTag = 0x20000000u;

} // namespace

bool WorldScene::upgradeTabOpen() const
{
    return m_shop.isOpen() && m_shopWindow != nullptr && m_shopWindow->isVisible() && m_shopWindow->upgradeTab();
}

void WorldScene::upgradePick(std::uint8_t bagSlot)
{
    if (!upgradeTabOpen())
        return;
    if (!m_upgrade.pick(m_inventory.view(), bagSlot))
        return;
    m_upgrade.clearOutcome();
    const game::ItemStack& s = m_inventory.view().at(game::ItemPlace::bag(bagSlot));
    VO_LOG_INFO("world: upgrade section: bag {} (item {}, template {}, +{})", bagSlot, s.id, s.templateId, s.upgrade);
}

void WorldScene::upgradeStart()
{
    if (!upgradeTabOpen() || m_upgrade.busy() || m_self.dead)
    {
        if (m_self.dead)
            showShopMessage(world::upgradeResultText(vn::UpgradeResult::YouAreDead, nullptr, 0), true);
        return;
    }
    const game::ItemCatalog* catalog = m_inventory.catalog();
    if (catalog == nullptr)
        return;
    const world::UpgradePanelView v = world::upgradePanelView(*catalog, m_inventory.view(), m_coins, m_upgrade.slot());
    if (!v.canAttempt)
    {
        if (!v.blockText.empty())
            showShopMessage(v.blockText, true);
        return;
    }
    const std::uint32_t npc = m_shop.npc();
    const std::uint64_t item = m_upgrade.itemId();
    if (!v.needsConfirm)
    {
        sendUpgrade(m_upgrade.begin(m_inventory.view(), npc));
        return;
    }
    // §7.3: a failure would destroy the item - ask first (Esc = "Vazgeç").
    m_upgradeConfirm = true;
    confirm(std::format("Dikkat: {}", v.name), world::upgradeConfirmText(v), "Yükselt", "Vazgeç",
            [this, npc, item](bool yes) {
                m_upgradeConfirm = false;
                if (!yes)
                {
                    ++m_upgradeSeen.cancelled;
                    return;
                }
                // Still the same item at the same open smith?
                if (!upgradeTabOpen() || m_shop.npc() != npc || m_upgrade.itemId() != item)
                    return;
                sendUpgrade(m_upgrade.begin(m_inventory.view(), npc));
            });
}

void WorldScene::sendUpgrade(std::optional<vn::UpgradeReq> req)
{
    if (!req)
        return;
    std::size_t recent = 0;
    for (const double t : m_upgradeSends)
        recent += (m_time - t) < 1.0 ? 1 : 0;
    const bool budget = recent < m_upgradeSends.size();
    if (m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked() || !budget)
    {
        m_upgrade.abandon();
        if (!budget)
            showShopMessage("Çok hızlı: biraz bekle", true);
        return;
    }
    if (!ctx().net.send(*req))
    {
        m_upgrade.abandon();
        return;
    }
    m_upgradeSends[m_upgradeSendNext] = m_time;
    m_upgradeSendNext = (m_upgradeSendNext + 1) % m_upgradeSends.size();
    ++m_upgradeSeen.sent;
    m_inventory.beginTake(kUpgradeTakeTag | req->seq); // its ITEM_UPDATEs settle no item move
    VO_LOG_INFO("world: UPGRADE_REQ seq {} npc {} bag {} item {} from +{} stone {}", req->seq, req->npcId, req->bagSlot,
                req->itemId, req->fromLevel,
                req->stoneSlot == vn::kUpgradeAnyStone ? std::string("any") : std::to_string(req->stoneSlot));
}

bool WorldScene::handleUpgradeRes(const net::NetEvent& e)
{
    vn::UpgradeRes m;
    if (!vn::decodeMessage(e.payload, m))
    {
        VO_LOG_ERROR("world: malformed UPGRADE_RES payload ({} bytes)", e.payload.size());
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return true;
    }
    m_inventory.endTake(kUpgradeTakeTag | m.seq);
    const auto r = m_upgrade.finish(m, ui().time());
    VO_LOG_INFO("world: UPGRADE_RES seq {} {} bag {} item {} level {} stone {} fee {}{}", m.seq,
                vn::upgradeResultName(m.result), m.bagSlot, m.templateId, m.level, m.stoneTemplateId, m.fee,
                r ? "" : " (not ours / forgotten)");
    if (!r)
        return true;
    const game::ItemTemplate* item = itemTemplate(m.templateId != 0 ? m.templateId : r->templateId);
    switch (m.result)
    {
    case vn::UpgradeResult::Success: ++m_upgradeSeen.success; break;
    case vn::UpgradeResult::Failed: ++m_upgradeSeen.failed; break;
    case vn::UpgradeResult::Destroyed: ++m_upgradeSeen.destroyed; break;
    default: ++m_upgradeSeen.refused; break;
    }
    const std::string text = world::upgradeResultText(m.result, item, m.level);
    if (upgradeTabOpen())
    {
        // The Yükselt section shows it (with its effect); the line above the bar too, not the window's footer.
        m_lastShopMessage = text;
        showCombatMessage(text);
    }
    else
        showShopMessage(text, world::upgradeResultIsBad(m.result));
    if (vn::isUpgradeAttempt(m.result))
        if (world::NpcEntity* n = m_npcs.find(r->npc); n != nullptr && n->animator.playInteract())
            ++m_shopSeen.interacts;
    return true;
}

void WorldScene::updateUpgrade()
{
    m_upgrade.sync(m_inventory.view());
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::upgradeStates(std::vector<std::string>& out) const
{
    const auto counter = [&out](const char* name, std::size_t n) {
        for (std::size_t i = 1; i <= std::min<std::size_t>(n, 12); ++i)
            out.emplace_back(name + std::to_string(i));
    };
    if (m_shopWindow != nullptr && m_shopWindow->hasTabs() && m_shop.isOpen())
        out.emplace_back("shoptabs");
    if (upgradeTabOpen())
        out.emplace_back("upgradetab");
    if (m_upgrade.slot())
        out.emplace_back("upgradeitem");
    out.emplace_back(m_upgrade.busy() ? "upgradebusy" : "upgradeidle");
    if (m_upgradeConfirm)
        out.emplace_back("upgradeconfirm");
    if (upgradeTabOpen() && m_shopWindow->upgradePanel() != nullptr && m_shopWindow->upgradePanel()->view().canAttempt)
        out.emplace_back("upgradeready");
    counter("upgradesent", m_upgradeSeen.sent);
    counter("upgradeok", m_upgradeSeen.success);
    counter("upgradefailed", m_upgradeSeen.failed);
    counter("upgradedestroyed", m_upgradeSeen.destroyed);
    counter("upgraderefused", m_upgradeSeen.refused);
    counter("upgradecancelled", m_upgradeSeen.cancelled);
}

bool WorldScene::upgradeAutomation(std::string_view command, bool& handled, std::string& error)
{
    handled = false;
    std::string parseError;
    const auto cmd = world::parseUpgradeCommand(command, parseError);
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
    switch (cmd->verb)
    {
    case world::UpgradeVerb::Tab:
        if (!m_shop.isOpen() || m_shopWindow == nullptr)
            return fail("shoptab: no shop open");
        if (cmd->text == "upgrade" && !m_shopWindow->hasTabs())
            return fail(std::format("shoptab: '{}' does not upgrade items (no tabs)", m_shopNpcName));
        m_shopWindow->setUpgradeTab(cmd->text == "upgrade");
        return true;
    case world::UpgradeVerb::Pick:
    {
        if (!upgradeTabOpen())
            return fail("upgradepick: the Yükselt tab is not open");
        const auto slot = static_cast<std::uint8_t>(cmd->a);
        if (!inventoryRightClick(game::ItemPlace::bag(slot)))
            return fail(std::format("upgradepick: bag slot {} was not taken", cmd->a));
        return true;
    }
    case world::UpgradeVerb::Clear:
        m_upgrade.clear();
        return true;
    case world::UpgradeVerb::Start:
        if (!upgradeTabOpen())
            return fail("upgradestart: the Yükselt tab is not open");
        upgradeStart();
        return true;
    case world::UpgradeVerb::Force:
    {
        if (!m_shop.isOpen())
            return fail("upgradeforce: no shop open");
        if (cmd->a >= 256)
            return fail("upgradeforce: bag slot out of range");
        const auto slot = static_cast<std::uint8_t>(cmd->a);
        game::ItemStack s;
        if (slot < game::kBagSlots)
            s = m_inventory.view().at(game::ItemPlace::bag(slot));
        const auto from = cmd->hasC ? static_cast<std::uint8_t>(cmd->c) : s.upgrade;
        sendUpgrade(m_upgrade.beginRaw(m_shop.npc(), slot, s.id, from, static_cast<std::uint8_t>(cmd->b), s.templateId));
        return true;
    }
    case world::UpgradeVerb::ExpectLevel:
    {
        const game::ItemStack& s = m_inventory.view().at(game::ItemPlace::bag(static_cast<std::uint8_t>(cmd->a)));
        if (s.empty())
            return fail(std::format("expectupgrade: bag slot {} is empty", cmd->a));
        if (s.upgrade != cmd->b)
            return fail(std::format("expectupgrade: bag slot {} holds +{}, expected +{}", cmd->a, s.upgrade, cmd->b));
        return true;
    }
    case world::UpgradeVerb::ExpectPanel:
    {
        if (!upgradeTabOpen() || m_shopWindow->upgradePanel() == nullptr)
            return fail("expectupgradepanel: the Yükselt tab is not open");
        const std::string& all = m_shopWindow->upgradePanel()->contents();
        if (all.find(cmd->text) == std::string::npos)
            return fail(std::format("expectupgradepanel: '{}' not in the section ('{}')", cmd->text, all));
        return true;
    }
    case world::UpgradeVerb::Log:
    {
        const auto slot = m_upgrade.slot();
        VO_LOG_INFO("world: upgrade: tab {}, item {} (bag {}), busy {}, Akçe {}", upgradeTabOpen(), m_upgrade.itemId(),
                    slot ? std::to_string(*slot) : std::string("-"), m_upgrade.busy(), m_coins);
        if (upgradeTabOpen() && m_shopWindow->upgradePanel() != nullptr)
            VO_LOG_INFO("world:   section: {}", m_shopWindow->upgradePanel()->contents());
        return true;
    }
    }
    return true;
}

} // namespace vo::client::scenes
