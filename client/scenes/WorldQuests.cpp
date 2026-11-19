// WorldScene, quest half (M17, docs/PROTOCOL_QUESTS.md §7): the quest rows (QUEST_LIST / QUEST_UPDATE), the
// NPC dialog (a click on a quest NPC, or on a merchant with a quest to give / take back: "Alışveriş" leads to
// its shop), accepting / handing in / abandoning (one request at a time, at most 4 a second), the quest log
// (L, the HUD button "Görevler"), the HUD tracker under the zone name, the completion notices and the quest
// automation. The pure rules and texts live in client/world/QuestView.h, the windows in QuestWindows.h.
#include "client/scenes/WorldScene.h"

#include "client/net/GameClient.h"
#include "client/render/TextRenderer.h"
#include "client/scenes/KunyeWindow.h"
#include "client/scenes/QuestWindows.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/ShopWindow.h"
#include "client/scenes/SkillWindow.h"
#include "client/ui/Ui.h"
#include "shared/core/Log.h"
#include "shared/game/Npcs.h"
#include "shared/game/ZoneData.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;
using game::QuestStatus;
using world::Vec3;

namespace {

constexpr float kQuestDialogFallbackClose = 9.5f; // as the shop: the server's range is 4.5 m for the requests
constexpr float kTrackerTop = 74.0f;              // logical: under the zone name and its second line
constexpr float kTrackerRight = 12.0f;

} // namespace

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void WorldScene::buildQuests()
{
    QuestDialog::Hooks dh;
    dh.select = [this](game::QuestId q) {
        m_questSelected = q;
        m_questChoice = -1;
        m_questMessage.clear();
        refreshQuestDialog();
    };
    dh.back = [this]() {
        m_questSelected = 0;
        m_questChoice = -1;
        refreshQuestDialog();
    };
    dh.accept = [this](game::QuestId q) { questAccept(q); };
    dh.handIn = [this](game::QuestId q, std::uint8_t choice) { questHandIn(q, choice); };
    dh.decline = [this]() { closeQuestDialog(); };
    dh.shop = [this]() {
        const std::uint32_t npc = m_questNpc;
        closeQuestDialog();
        if (npc != 0)
            (void)sendShopOpen(npc);
    };
    dh.choose = [this](int i) {
        m_questChoice = i;
        refreshQuestDialog();
    };
    m_questDialog = &ui().root().add<QuestDialog>(ctx().resources, m_inventory, std::move(dh));
    m_questDialog->setAnchors(anchor::kTopLeft).setMargins({8.0f, 120.0f, 0.0f, 0.0f});
    m_questDialog->setVisible(false);

    QuestLogWindow::Hooks lh;
    lh.select = [this](game::QuestId q) {
        m_questLogSelected = q;
        refreshQuestLog();
    };
    lh.abandon = [this](game::QuestId q) { questAbandon(q, true); };
    lh.close = [this]() { setQuestLogOpen(false); };
    m_questLog = &ui().root().add<QuestLogWindow>(ctx().resources, m_inventory, std::move(lh));
    m_questLog->setAnchors(anchor::kTopLeft).setMargins({400.0f, 120.0f, 0.0f, 0.0f});
    m_questLog->setVisible(false);

    m_questTracker = &ui().root().add<QuestTracker>();
    m_questTracker->setAnchors(anchor::kTopLeft).setSize(QuestTracker::kWidth, 1.0f);
    m_questTracker->setVisible(false);

    m_questBook.clear();
    m_questReq.forget();
    m_questNpc = 0;
    m_questSelected = 0;
    m_questLogSelected = 0;
    m_questChoice = -1;
    m_questLogOpen = false;
    m_questNotices.clear();
    m_questSeen = {};
}

game::QuestUser WorldScene::questUser() const noexcept
{
    return {m_race, m_charClass, m_self.level};
}

std::int64_t WorldScene::questNow() const noexcept
{
    return world::QuestBook::clock(m_time);
}

std::size_t WorldScene::questMaxActive() const noexcept
{
    return game::quests().maxActive();
}

game::QuestMarker WorldScene::questMarker(const world::NpcEntity& n) const
{
    if (!m_questBook.known())
        return game::QuestMarker::None;
    return game::questMarkerFor(game::quests(), n.npcId, m_questBook.log(), questUser(), m_inventory.view(),
                                m_inventory.catalog(), questNow(), questMaxActive());
}

// ---------------------------------------------------------------------------------------------
// The dialog
// ---------------------------------------------------------------------------------------------

bool WorldScene::questTalk(std::uint32_t npcEntity, bool forceDialog)
{
    const world::NpcEntity* n = m_npcs.find(npcEntity);
    if (n == nullptr || n->def == nullptr)
        return false;
    if (!n->def->isMerchant() || forceDialog)
    {
        openQuestDialog(npcEntity);
        return true;
    }
    // A merchant: the dialog first only when it has a quest to hand in here or to give now.
    for (const world::NpcQuestEntry& e :
         world::npcQuestEntries(game::quests(), n->npcId, m_questBook.log(), questUser(), m_inventory.view(),
                                m_inventory.catalog(), questNow(), questMaxActive()))
        if ((e.status == QuestStatus::Ready && e.turnIn) || (e.status == QuestStatus::Available && e.giver))
        {
            openQuestDialog(npcEntity);
            return true;
        }
    return false;
}

void WorldScene::openQuestDialog(std::uint32_t npcEntity, game::QuestId select)
{
    const world::NpcEntity* n = m_npcs.find(npcEntity);
    if (n == nullptr || m_questDialog == nullptr || m_phase != Phase::Ready)
        return;
    if (m_shop.isOpen())
        closeShop(true, {});
    // The dialog takes the left side, like the shop: the Künye and the Yetenekler window make room.
    setKunyeOpen(false);
    setSkillsOpen(false);
    m_questNpc = npcEntity;
    m_questChoice = -1;
    m_questMessage.clear();
    m_questSelected = select;
    if (m_questSelected == 0)
    {
        // One quest to offer / take back and no shop: straight to it.
        const auto entries = world::npcQuestEntries(game::quests(), n->npcId, m_questBook.log(), questUser(),
                                                    m_inventory.view(), m_inventory.catalog(), questNow(), questMaxActive());
        if (entries.size() == 1 && (n->def == nullptr || !n->def->isMerchant()))
            m_questSelected = entries.front().quest->id;
    }
    ++m_questSeen.dialogs;
    m_questDialog->setVisible(true);
    refreshQuestDialog();
    layoutHud();
    VO_LOG_INFO("world: quest dialog of {} '{}' (npc {}){}", npcEntity, n->name, n->npcId,
                m_questSelected != 0 ? std::format(", quest {}", m_questSelected) : std::string());
}

void WorldScene::closeQuestDialog()
{
    if (m_questDialog == nullptr || !m_questDialog->isVisible())
    {
        m_questNpc = 0;
        return;
    }
    m_questDialog->setVisible(false);
    m_questNpc = 0;
    m_questSelected = 0;
    m_questChoice = -1;
    layoutHud();
}

void WorldScene::refreshQuestDialog()
{
    if (m_questDialog == nullptr || m_questNpc == 0)
        return;
    const world::NpcEntity* n = m_npcs.find(m_questNpc);
    if (n == nullptr)
        return;
    const game::QuestCatalog& catalog = game::quests();
    QuestDialog::View v;
    v.npcName = n->name;
    v.npcTitle = std::string(n->title());
    v.greeting = n->def != nullptr ? n->def->greeting : std::string();
    v.merchant = n->def != nullptr && n->def->isMerchant();
    v.entries = world::npcQuestEntries(catalog, n->npcId, m_questBook.log(), questUser(), m_inventory.view(),
                                       m_inventory.catalog(), questNow(), questMaxActive());
    v.busy = m_questReq.busy();
    v.message = m_questMessage;
    v.messageError = m_questMessageError;
    if (const game::QuestDef* q = m_questSelected != 0 ? catalog.find(m_questSelected) : nullptr)
    {
        QuestBodyView& b = v.body;
        b.quest = q;
        b.status = game::questStatus(catalog, *q, m_questBook.log(), questUser(), m_inventory.view(), m_inventory.catalog(),
                                     questNow(), questMaxActive());
        b.record = m_questBook.log().find(q->id);
        b.charClass = m_charClass;
        b.selectedChoice = m_questChoice;
        switch (b.status)
        {
        case QuestStatus::Available: b.npcText = q->story; break;
        case QuestStatus::Ready: b.npcText = q->doneText; break;
        case QuestStatus::Active:
            b.npcText = q->progressText.empty() ? q->story : q->progressText;
            b.footnote = std::format("Teslim: {}", world::npcName(&game::npcs(), q->turnIn));
            break;
        case QuestStatus::Later:
            b.npcText = q->story;
            b.footnote = std::format("{}. seviyede alınabilir.", q->level);
            break;
        default: b.npcText = q->story; break;
        }
        if (b.status == QuestStatus::Available && q->turnIn != q->giver)
            b.footnote = std::format("Teslim: {}", world::npcName(&game::npcs(), q->turnIn));
    }
    m_questDialog->setView(std::move(v));
}

void WorldScene::showQuestMessage(std::string_view text, bool error)
{
    if (text.empty())
        return;
    m_lastQuestMessage = std::string(text);
    m_questMessage = m_lastQuestMessage;
    m_questMessageError = error;
    showCombatMessage(text); // the message line above the bar, also when the window is closed
    refreshQuestDialog();
}

void WorldScene::forgetQuests()
{
    m_questReq.forget();
    if (m_questDialog != nullptr && m_questDialog->isVisible())
        closeQuestDialog();
    m_questNpc = 0;
}

// ---------------------------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------------------------

bool WorldScene::sendQuestRequest(vn::QuestAction action, game::QuestId quest, std::uint32_t npcEntity,
                                  std::uint8_t choice, bool direct)
{
    if (m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked() || m_zoneChanging)
        return false;
    if (!direct && m_self.dead)
    {
        showQuestMessage(world::questResultText(vn::QuestResult::YouAreDead, nullptr, nullptr), true);
        return false;
    }
    if (!m_questReq.canSend(m_time))
    {
        if (!m_questReq.busy())
            showQuestMessage("Çok hızlı: biraz bekle", true);
        return false;
    }
    const std::uint32_t seq = m_questReq.begin(action, quest, npcEntity, m_time);
    bool sent = false;
    switch (action)
    {
    case vn::QuestAction::Accept: sent = ctx().net.send(vn::QuestAcceptReq{seq, npcEntity, quest}); break;
    case vn::QuestAction::Abandon: sent = ctx().net.send(vn::QuestAbandonReq{seq, quest}); break;
    case vn::QuestAction::HandIn: sent = ctx().net.send(vn::QuestHandInReq{seq, npcEntity, quest, choice}); break;
    }
    if (!sent)
    {
        m_questReq.forget();
        return false;
    }
    VO_LOG_INFO("world: QUEST_{}_REQ seq {} quest {} npc {}{}",
                action == vn::QuestAction::Accept ? "ACCEPT" : action == vn::QuestAction::Abandon ? "ABANDON" : "HANDIN", seq,
                quest, npcEntity, choice != vn::kQuestNoChoice ? std::format(" choice {}", choice) : std::string());
    refreshQuestDialog();
    refreshQuestLog();
    return true;
}

void WorldScene::questAccept(game::QuestId quest)
{
    if (m_questNpc == 0)
        return;
    const game::QuestDef* def = game::quests().find(quest);
    if (def != nullptr)
    {
        // Local checks first (the server decides anyway): a refusal we can already name needs no request.
        const vn::QuestResult r = game::checkQuestAccept(game::quests(), *def, m_questBook.log(), questUser(), questNow(),
                                                         questMaxActive());
        if (r != vn::QuestResult::Ok)
        {
            ++m_questSeen.refused;
            showQuestMessage(world::questResultText(r, def, &game::npcs()), true);
            return;
        }
    }
    (void)sendQuestRequest(vn::QuestAction::Accept, quest, m_questNpc);
}

void WorldScene::questHandIn(game::QuestId quest, std::uint8_t choice)
{
    if (m_questNpc == 0)
        return;
    const game::QuestDef* def = game::quests().find(quest);
    if (def != nullptr && !def->reward.choice.empty() && choice == vn::kQuestNoChoice)
    {
        showQuestMessage(world::questResultText(vn::QuestResult::BadChoice, def, nullptr), true);
        return;
    }
    (void)sendQuestRequest(vn::QuestAction::HandIn, quest, m_questNpc, choice);
}

void WorldScene::questAbandon(game::QuestId quest, bool ask)
{
    const game::QuestDef* def = game::quests().find(quest);
    if (def == nullptr)
        return;
    if (!ask)
    {
        (void)sendQuestRequest(vn::QuestAction::Abandon, quest, 0);
        return;
    }
    m_questAbandonAsk = true;
    confirm(def->name, world::abandonQuestion(*def), "Bırak", "Vazgeç", [this, quest](bool yes) {
        m_questAbandonAsk = false;
        if (yes)
            (void)sendQuestRequest(vn::QuestAction::Abandon, quest, 0);
    }, true); // Enter = Vazgeç: losing the progress takes a click
}

// ---------------------------------------------------------------------------------------------
// The log and the tracker
// ---------------------------------------------------------------------------------------------

void WorldScene::setQuestLogOpen(bool open)
{
    if (m_questLog == nullptr || (open && m_phase != Phase::Ready))
        return;
    if (open && (m_leavingWorld || m_model.leaving()))
        return;
    if (open == m_questLogOpen)
        return;
    m_questLogOpen = open;
    m_questLog->setVisible(open);
    if (open)
        refreshQuestLog();
    layoutHud();
    VO_LOG_INFO("world: Görevler {}", open ? "opened" : "closed");
}

void WorldScene::refreshQuestLog()
{
    if (m_questLog == nullptr || !m_questLogOpen)
        return;
    const game::QuestCatalog& catalog = game::quests();
    QuestLogWindow::View v;
    v.limit = questMaxActive();
    v.busy = m_questReq.busy();
    const std::vector<game::QuestId> active = m_questBook.activeInOrder();
    for (const game::QuestId id : active)
        if (const game::QuestDef* q = catalog.find(id))
        {
            const game::QuestRecord* rec = m_questBook.log().find(id);
            v.rows.push_back({id, q->name, rec != nullptr && game::questReady(*q, *rec, m_inventory.view(), m_inventory.catalog())});
        }
    if (std::find(active.begin(), active.end(), m_questLogSelected) == active.end())
        m_questLogSelected = active.empty() ? 0 : active.front();
    if (const game::QuestDef* q = m_questLogSelected != 0 ? catalog.find(m_questLogSelected) : nullptr)
    {
        QuestBodyView& b = v.body;
        b.quest = q;
        b.record = m_questBook.log().find(q->id);
        b.status = game::questStatus(catalog, *q, m_questBook.log(), questUser(), m_inventory.view(), m_inventory.catalog(),
                                     questNow(), questMaxActive());
        b.charClass = m_charClass;
        b.npcText = b.status == QuestStatus::Ready ? std::string() : (q->progressText.empty() ? q->story : q->progressText);
        const game::NpcDef* turnIn = game::npcs().find(q->turnIn);
        const game::ZoneDef* zone = turnIn != nullptr ? game::zones().find(turnIn->zone) : nullptr;
        b.footnote = turnIn != nullptr ? std::format("Teslim: {} ({})", turnIn->name, zone != nullptr ? zone->name : "?")
                                       : std::string();
    }
    m_questLog->setView(std::move(v));
}

void WorldScene::refreshQuestTracker()
{
    if (m_questTracker == nullptr)
        return;
    const game::QuestCatalog& catalog = game::quests();
    std::vector<QuestTracker::Entry> entries;
    for (const game::QuestId id : m_questBook.activeInOrder())
    {
        if (entries.size() >= world::kTrackerMaxQuests)
            break;
        const game::QuestDef* q = catalog.find(id);
        const game::QuestRecord* rec = m_questBook.log().find(id);
        if (q == nullptr || rec == nullptr)
            continue;
        QuestTracker::Entry e;
        e.name = q->name;
        e.lines = world::objectiveLines(*q, *rec, m_inventory.view(), m_inventory.catalog());
        e.ready = game::questReady(*q, *rec, m_inventory.view(), m_inventory.catalog());
        e.turnIn = std::format("{} ile konuş", world::npcName(&game::npcs(), q->turnIn));
        // A talk-only quest reads as its one line ("Avcı Selvi ile konuş") rather than as "ready".
        if (e.ready && q->objectives.size() == 1 && q->objectives.front().kind == game::ObjectiveKind::Talk)
            e.ready = false;
        entries.push_back(std::move(e));
    }
    m_questTracker->setEntries(std::move(entries));
}

void WorldScene::layoutQuestTracker(float w, float s)
{
    if (m_questTracker == nullptr)
        return;
    const float h = m_questTracker->contentHeight();
    const float x = w - QuestTracker::kWidth - kTrackerRight;
    float top = kTrackerTop;
    if (m_shownSlot != nullptr)
        top += 6.0f; // (the hunting slot line under the zone name is a little lower)
    m_questTracker->setMargins({x, top, 0.0f, 0.0f}).setSize(QuestTracker::kWidth, std::max(1.0f, h));
    const Rect r{x * s, top * s, QuestTracker::kWidth * s, h * s};
    const bool ready = m_phase == Phase::Ready && !m_zoneChanging && !m_menuOpen;
    const bool shown = ready && h > 0.0f && !world::coveredByAny(r, m_windowCovers);
    if (m_questTracker->isVisible() != shown)
        m_questTracker->setVisible(shown);
    m_questTrackerShown = shown;
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

bool WorldScene::handleQuestMessage(const net::NetEvent& e)
{
    const auto op = static_cast<vn::Opcode>(e.opcode);
    const auto malformed = [&]() {
        VO_LOG_ERROR("world: malformed {} payload ({} bytes)", vn::opcodeName(op), e.payload.size());
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return true;
    };
    switch (op)
    {
    case vn::Opcode::QuestList:
    {
        vn::QuestList m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_questBook.apply(m, m_time);
        (void)m_questBook.takeNotices();
        ++m_questSeen.lists;
        VO_LOG_INFO("world: QUEST_LIST ({} row(s), {} active)", m.quests.size(), m_questBook.activeInOrder().size());
        refreshQuestTracker();
        refreshQuestDialog();
        refreshQuestLog();
        return true;
    }
    case vn::Opcode::QuestUpdate:
    {
        vn::QuestUpdate m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_questBook.apply(m, m_time, m_inventory.view(), m_inventory.catalog());
        ++m_questSeen.updates;
        VO_LOG_INFO("world: QUEST_UPDATE quest {} state {} progress {}/{}/{}", m.quest.questId,
                    static_cast<int>(m.quest.state), m.quest.progress[0], m.quest.progress[1], m.quest.progress[2]);
        refreshQuestTracker();
        refreshQuestDialog();
        refreshQuestLog();
        return true;
    }
    case vn::Opcode::QuestRes:
    {
        vn::QuestRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        const auto req = m_questReq.finish(m);
        VO_LOG_INFO("world: QUEST_RES seq {} quest {} action {} {}{}", m.seq, m.questId, static_cast<int>(m.action),
                    vn::questResultName(m.result), req ? "" : " (not ours / forgotten)");
        if (!req)
            return true;
        const game::QuestDef* def = game::quests().find(m.questId);
        if (m.result != vn::QuestResult::Ok)
        {
            ++m_questSeen.refused;
            showQuestMessage(world::questResultText(m.result, def, &game::npcs()), true);
            refreshQuestLog();
            return true;
        }
        const std::string name = def != nullptr ? def->name : std::format("Görev #{}", m.questId);
        switch (m.action)
        {
        case vn::QuestAction::Accept:
            ++m_questSeen.accepted;
            m_questSelected = 0; // back to the list (or the dialog closes when nothing is left)
            showQuestMessage(std::format("Görev alındı: {}", name), false);
            break;
        case vn::QuestAction::HandIn:
            ++m_questSeen.handedIn;
            m_questSelected = 0;
            m_questChoice = -1;
            if (def != nullptr)
            {
                showQuestMessage(world::handInText(*def, m.xp, m.coins), false);
                m_questNotices.push_back({world::handInText(*def, m.xp, m.coins), world::QuestNotice::Kind::HandedIn, m_time});
                ++m_questSeen.notices;
            }
            if (world::NpcEntity* n = m_npcs.find(req->npc); n != nullptr)
                (void)n->animator.playInteract();
            break;
        case vn::QuestAction::Abandon:
            ++m_questSeen.abandoned;
            showQuestMessage(std::format("Görev bırakıldı: {}", name), false);
            break;
        }
        // The dialog: back to the list while the NPC still offers a quest or takes one back; else it closes.
        if (m_questNpc != 0 && m.action != vn::QuestAction::Abandon)
        {
            const world::NpcEntity* n = m_npcs.find(m_questNpc);
            bool more = false;
            if (n != nullptr)
                for (const world::NpcQuestEntry& x :
                     world::npcQuestEntries(game::quests(), n->npcId, m_questBook.log(), questUser(), m_inventory.view(),
                                            m_inventory.catalog(), questNow(), questMaxActive()))
                    more = more || (x.status == QuestStatus::Available && x.giver) || (x.status == QuestStatus::Ready && x.turnIn);
            if (!more)
                closeQuestDialog();
        }
        refreshQuestDialog();
        refreshQuestLog();
        refreshQuestTracker();
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------

void WorldScene::updateQuests(float dt)
{
    (void)dt;
    if (m_phase != Phase::Ready)
        return;
    // Collect objectives follow the bag; the notices of the book are shown for a few seconds.
    m_questBook.noteInventory(m_inventory.view(), m_inventory.catalog());
    for (world::QuestNotice& n : m_questBook.takeNotices())
    {
        if (n.kind == world::QuestNotice::Kind::Abandoned)
            continue; // (the result line says it)
        VO_LOG_INFO("world: quest notice: {}", n.text);
        m_questNotices.push_back({std::move(n.text), n.kind, m_time});
        ++m_questSeen.notices;
    }
    std::erase_if(m_questNotices, [&](const ShownNotice& n) { return m_time - n.at > world::kQuestNoticeSeconds; });
    if (m_questNotices.size() > 3)
        m_questNotices.erase(m_questNotices.begin(), m_questNotices.end() - 3);
    refreshQuestTracker();
    // The dialog follows its NPC (gone from the view, walked away, dead).
    if (m_questNpc != 0)
    {
        const world::NpcEntity* n = m_npcs.find(m_questNpc);
        const Vec3 me = m_model.local().mover().position();
        if (n == nullptr || m_self.dead)
            closeQuestDialog();
        else if (world::horizontalDistance(n->position, me) > kQuestDialogFallbackClose)
        {
            const std::string name = n->name;
            closeQuestDialog();
            showQuestMessage(std::format("{} çok uzakta kaldı.", name), false);
        }
        else
            refreshQuestDialog();
    }
    if (m_questLogOpen)
        refreshQuestLog();
}

void WorldScene::drawQuestOverlay()
{
    if (m_questNotices.empty() || m_phase != Phase::Ready || m_zoneChanging)
        return;
    auto& batch = ctx().batch;
    const float s = ui().scale();
    const float w = ctx().viewportWidth;
    const render::Font& f = ctx().fonts.get(render::FontWeight::Bold, 17.0f * s);
    // Centred, a quarter down the screen (under the target frame and the zone banner): newest last. Centred on
    // the band the quest dialog / log leave free, so a notice is not half hidden under the dialog.
    float left = 0.0f;
    float right = w;
    if (m_questDialog != nullptr && m_questDialog->isVisible())
        left = std::max(left, m_questDialog->rect().right());
    if (m_questLog != nullptr && m_questLog->isVisible())
    {
        const Rect& r = m_questLog->rect();
        if (r.centerX() > w * 0.5f)
            right = std::min(right, r.x);
        else
            left = std::max(left, r.right());
    }
    if (right - left < w * 0.35f)
    {
        left = 0.0f;
        right = w;
    }
    float y = std::round(ctx().viewportHeight * 0.24f);
    for (const ShownNotice& n : m_questNotices)
    {
        const double age = m_time - n.at;
        const float alpha = static_cast<float>(std::clamp(std::min(age / 0.25, (world::kQuestNoticeSeconds - age) / 0.8), 0.0, 1.0));
        const std::uint32_t rgb = n.kind == world::QuestNotice::Kind::Ready || n.kind == world::QuestNotice::Kind::HandedIn
                                      ? world::kQuestReadyRgb
                                      : (n.kind == world::QuestNotice::Kind::Accepted ? world::kQuestAvailableRgb : 0xEDE6D3);
        const float tw = render::measureText(f, n.text);
        const float gem = std::round(14.0f * s);
        const Rect plate = Rect{std::round(left + (right - left - tw) * 0.5f) - gem - 14.0f * s, y - 4.0f * s, tw + gem + 28.0f * s,
                                f.lineHeight() + 8.0f * s}
                               .snapped();
        batch.fillRoundedRect(plate, 6.0f * s, Color::hex(0x07090C, 0.78f * alpha));
        drawQuestMarker(batch, {plate.x + 10.0f * s + gem * 0.5f, plate.centerY() - 1.0f * s}, gem,
                        n.kind == world::QuestNotice::Kind::Accepted ? game::QuestMarker::Available : game::QuestMarker::Ready,
                        m_time, alpha);
        render::TextStyle ts;
        ts.color = Color::hex(rgb, alpha);
        ts.outline = true;
        ts.outlineColor = Color::hex(0x000000, 0.85f * alpha);
        render::drawText(batch, f, n.text, {plate.x + 16.0f * s + gem, y}, ts);
        y += f.lineHeight() + 12.0f * s;
    }
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::questStates(std::vector<std::string>& out) const
{
    const auto counter = [&out](const char* name, std::size_t n) {
        for (std::size_t i = 1; i <= std::min<std::size_t>(n, 12); ++i)
            out.emplace_back(name + std::to_string(i));
    };
    if (m_questBook.known())
        out.emplace_back("questsknown");
    out.emplace_back(m_questDialog != nullptr && m_questDialog->isVisible() ? "questdialog" : "noquestdialog");
    if (m_questDialog != nullptr && m_questDialog->isVisible())
        out.emplace_back(m_questSelected != 0 ? "questdetail" : "questlist");
    out.emplace_back(m_questLogOpen ? "questlog" : "noquestlog");
    out.emplace_back(m_questReq.busy() ? "questbusy" : "questidle");
    out.emplace_back(m_questTrackerShown ? "questtracker" : "noquesttracker");
    if (!m_questNotices.empty())
        out.emplace_back("questnotice");
    if (m_questAbandonAsk)
        out.emplace_back("questabandonask");
    counter("questactive", m_questBook.activeInOrder().size());
    counter("questaccepted", m_questSeen.accepted);
    counter("questhandedin", m_questSeen.handedIn);
    counter("questabandoned", m_questSeen.abandoned);
    counter("questrefused", m_questSeen.refused);
    counter("questupdates", m_questSeen.updates);
    for (const world::NpcEntity& n : m_npcs.entities())
        out.push_back(std::format("marker{}_{}", n.npcId, game::questMarkerName(questMarker(n))));
}

bool WorldScene::questsAutomation(std::string_view command, bool& handled, std::string& error)
{
    handled = false;
    std::string parseError;
    const auto cmd = world::parseQuestCommand(command, parseError);
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
    const game::QuestCatalog& catalog = game::quests();
    const bool dialogOpen = m_questDialog != nullptr && m_questDialog->isVisible();
    switch (cmd->verb)
    {
    case world::QuestVerb::Select:
        if (!dialogOpen)
            return fail("questselect: no quest dialog open");
        m_questSelected = static_cast<game::QuestId>(cmd->a);
        m_questChoice = -1;
        refreshQuestDialog();
        return true;
    case world::QuestVerb::Accept:
        if (!dialogOpen || m_questSelected == 0)
            return fail("questaccept: no quest shown in a dialog");
        questAccept(m_questSelected);
        return true;
    case world::QuestVerb::HandIn:
        if (!dialogOpen || m_questSelected == 0)
            return fail("questhandin: no quest shown in a dialog");
        if (cmd->hasB)
            m_questChoice = static_cast<int>(cmd->b);
        questHandIn(m_questSelected, m_questChoice < 0 ? vn::kQuestNoChoice : static_cast<std::uint8_t>(m_questChoice));
        return true;
    case world::QuestVerb::Decline: // (nothing to do when the dialog already closed itself)
        closeQuestDialog();
        return true;
    case world::QuestVerb::AcceptForce:
    case world::QuestVerb::HandInForce:
    {
        const std::uint32_t id = m_npcs.byName(cmd->text);
        if (id == 0)
            return fail(std::format("no NPC '{}' in view", cmd->text));
        if (cmd->verb == world::QuestVerb::AcceptForce)
            (void)sendQuestRequest(vn::QuestAction::Accept, static_cast<game::QuestId>(cmd->a), id, vn::kQuestNoChoice, true);
        else
            (void)sendQuestRequest(vn::QuestAction::HandIn, static_cast<game::QuestId>(cmd->a), id,
                                   cmd->hasB ? static_cast<std::uint8_t>(cmd->b) : vn::kQuestNoChoice, true);
        return true;
    }
    case world::QuestVerb::Abandon: questAbandon(static_cast<game::QuestId>(cmd->a), true); return true;
    case world::QuestVerb::AbandonForce: questAbandon(static_cast<game::QuestId>(cmd->a), false); return true;
    case world::QuestVerb::Log: setQuestLogOpen(cmd->text == "open"); return true;
    case world::QuestVerb::LogSelect:
        m_questLogSelected = static_cast<game::QuestId>(cmd->a);
        refreshQuestLog();
        return true;
    case world::QuestVerb::ExpectState:
    {
        const game::QuestDef* def = catalog.find(static_cast<game::QuestId>(cmd->a));
        const game::QuestRecord* rec = m_questBook.log().find(static_cast<game::QuestId>(cmd->a));
        std::string state = "none";
        if (rec != nullptr && rec->state == vn::QuestState::Active)
            state = def != nullptr && game::questReady(*def, *rec, m_inventory.view(), m_inventory.catalog()) ? "ready" : "active";
        else if (rec != nullptr && rec->state == vn::QuestState::Done)
            state = def != nullptr && game::questCooldownLeft(*def, rec, questNow()) > 0 ? "cooldown" : "done";
        const bool stateOk = state == cmd->text || (cmd->text == "active" && state == "ready");
        if (!stateOk)
            return fail(std::format("quest {} is {}, expected {}", cmd->a, state, cmd->text));
        for (std::size_t i = 0; i < cmd->progress.size() && i < 3; ++i)
        {
            const std::uint16_t p = rec != nullptr && def != nullptr
                                        ? game::objectiveProgress(*def, *rec, m_inventory.view(), m_inventory.catalog(), i)
                                        : 0;
            if (p != cmd->progress[i])
                return fail(std::format("quest {} objective {} at {}, expected {}", cmd->a, i + 1, p, cmd->progress[i]));
        }
        return true;
    }
    case world::QuestVerb::ExpectMarker:
    {
        const auto nl = cmd->text.find('\n');
        const std::string name = cmd->text.substr(0, nl);
        const std::string want = cmd->text.substr(nl + 1);
        const std::uint32_t id = m_npcs.byName(name);
        const world::NpcEntity* n = id != 0 ? m_npcs.find(id) : nullptr;
        if (n == nullptr)
            return fail(std::format("no NPC '{}' in view", name));
        const std::string_view got = game::questMarkerName(questMarker(*n));
        if (got != want)
            return fail(std::format("marker over {} is {}, expected {}", name, got, want));
        return true;
    }
    case world::QuestVerb::ExpectTracker:
    {
        const std::string text = m_questTracker != nullptr ? m_questTracker->plainText() : std::string();
        if (cmd->text == "none")
        {
            if (m_questTrackerShown)
                return fail("the tracker is shown: " + text);
            return true;
        }
        if (!m_questTrackerShown || text.find(cmd->text) == std::string::npos)
            return fail(std::format("tracker ({}) does not show '{}': {}", m_questTrackerShown ? "shown" : "hidden", cmd->text, text));
        return true;
    }
    case world::QuestVerb::ExpectMessage:
        if (m_lastQuestMessage.find(cmd->text) == std::string::npos)
            return fail(std::format("last quest message is '{}', expected '{}'", m_lastQuestMessage, cmd->text));
        return true;
    case world::QuestVerb::ExpectNotice:
        for (const ShownNotice& n : m_questNotices)
            if (n.text.find(cmd->text) != std::string::npos)
                return true;
        return fail(std::format("no quest notice contains '{}' ({} shown)", cmd->text, m_questNotices.size()));
    case world::QuestVerb::ExpectDialog:
    {
        const world::NpcEntity* n = m_questNpc != 0 ? m_npcs.find(m_questNpc) : nullptr;
        if (cmd->text == "none")
            return dialogOpen ? fail(std::format("a quest dialog is open ({})", n != nullptr ? n->name : "?")) : true;
        if (!dialogOpen || n == nullptr || n->name != cmd->text)
            return fail(std::format("the quest dialog is {}, expected {}", dialogOpen && n != nullptr ? n->name : "closed", cmd->text));
        return true;
    }
    case world::QuestVerb::Dump:
    {
        VO_LOG_INFO("world: quests: {} row(s), {} active, request {}, dialog npc {} quest {}, log {}",
                    m_questBook.log().records().size(), m_questBook.activeInOrder().size(),
                    m_questReq.busy() ? "in flight" : "none", m_questNpc, m_questSelected, m_questLogOpen);
        for (const game::QuestRecord& r : m_questBook.log().records())
            VO_LOG_INFO("world:   quest {} state {} progress {}/{}/{} completions {} completedAt {}", r.quest,
                        static_cast<int>(r.state), r.progress[0], r.progress[1], r.progress[2], r.completions, r.completedAt);
        for (const world::NpcEntity& n : m_npcs.entities())
            VO_LOG_INFO("world:   npc {} '{}' marker {}", n.npcId, n.name, game::questMarkerName(questMarker(n)));
        if (m_questDialog != nullptr && m_questDialog->isVisible())
            VO_LOG_INFO("world:   dialog: {}", m_questDialog->plainText());
        if (m_questTracker != nullptr)
            VO_LOG_INFO("world:   tracker: {}", m_questTracker->plainText());
        return true;
    }
    }
    return true;
}

} // namespace vo::client::scenes
