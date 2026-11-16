// WorldScene, character sheet half (M11 "Künye", docs/PROTOCOL_STATS.md §7): CHAR_STATS, the Künye
// window and its HUD button with the unspent-points marker, pending points and STAT_ALLOCATE_REQ,
// the inventory's attribute rules and the Künye automation. The pure rules live in
// client/world/StatsView.h; this file wires them to the network, the input and the screen.
#include "client/scenes/WorldScene.h"

#include "client/net/GameClient.h"
#include "client/platform/Input.h"
#include "client/render/TextRenderer.h"
#include "client/scenes/CharacterPreview.h"
#include "client/scenes/KunyeWindow.h"
#include "client/scenes/SceneResources.h"
#include "client/ui/Ui.h"
#include "client/world/ItemData.h"
#include "shared/core/Log.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;

namespace {

constexpr std::uint16_t kShiftStep = 5; // Shift+click on "+"

// "Künye (C)" with a glowing gold marker carrying the number of unspent points on its left edge.
class KunyeButton final : public ui::Button
{
public:
    KunyeButton(std::function<void()> onClick, std::function<std::uint16_t()> points)
        : ui::Button("Künye (C)", std::move(onClick)), m_points(std::move(points))
    {
        setName("world.kunye");
        setFocusable(false); // Space keeps jumping
    }
    void drawSelf(ui::DrawContext& ctx) override
    {
        ui::Button::drawSelf(ctx);
        const std::uint16_t n = m_points ? m_points() : 0;
        if (n == 0)
            return;
        const Rect r = rect();
        const float d = std::round(ctx.pxf(22.0f));
        const Vec2 c{std::round(r.x - ctx.pxf(4.0f)), std::round(r.y + r.h * 0.5f)};
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ctx.time) * 4.0f);
        auto& b = ctx.batch;
        // Soft glow rings, then the gold disc with the number.
        for (int i = 3; i >= 1; --i)
        {
            const float rr = d * 0.5f + ctx.pxf(2.0f + 3.0f * static_cast<float>(i)) * (0.8f + 0.4f * pulse);
            b.fillRoundedRect({c.x - rr, c.y - rr, 2.0f * rr, 2.0f * rr}, rr,
                              Color::hex(0xF5C84C, (0.10f + 0.06f * pulse) / static_cast<float>(i)));
        }
        const Rect disc{c.x - d * 0.5f, c.y - d * 0.5f, d, d};
        b.fillRoundedRectGradient(disc, d * 0.5f, Color::hex(0xFFE08A), Color::hex(0xC98A1E));
        b.strokeRoundedRect(disc, d * 0.5f, std::max(1.0f, ctx.pxf(1.0f)), Color::hex(0x5A3A08));
        render::TextStyle s;
        s.color = Color::hex(0x2A1A02);
        render::drawTextAligned(b, ctx.font(n >= 10 ? 11.0f : 13.0f, render::FontWeight::Bold), std::to_string(n), disc,
                                render::HAlign::Center, render::VAlign::Middle, s);
    }

private:
    std::function<std::uint16_t()> m_points;
};

const char* attributeKeyText(std::size_t i)
{
    return game::attributeKey(static_cast<game::Attribute>(i)).data();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void WorldScene::buildKunye()
{
    ui::Widget& root = ui().root();
    KunyeWindow::Hooks hooks;
    hooks.add = [this](std::size_t i, bool shift) { kunyeAdd(i, shift); };
    hooks.remove = [this](std::size_t i) { kunyeRemove(i); };
    hooks.confirm = [this]() { kunyeConfirm(); };
    hooks.cancel = [this]() { kunyeCancel(); };
    hooks.close = [this]() { setKunyeOpen(false); };
    m_kunyeWindow = &root.add<KunyeWindow>(m_sheet, std::move(hooks));
    // Left half of the screen, vertically centred (the inventory sits on the right): both fit next to
    // each other from 1280 logical pixels on.
    m_kunyeWindow->setAnchors(anchor::kLeft | anchor::kCenterY).setMargins({14.0f, 10.0f, 0.0f, 0.0f});
    m_kunyeWindow->setVisible(false);
    m_kunyeOpen = false;

    // Under the inventory button (top right).
    auto& button = root.add<KunyeButton>([this]() { setKunyeOpen(!m_kunyeOpen); },
                                         [this]() -> std::uint16_t { return m_sheet.known() ? m_sheet.unspent() : 0; });
    button.setAnchors(anchor::kTopRight).setMargins({0.0f, 88.0f, 18.0f, 0.0f}).setSize(132.0f, 32.0f);
    button.setVisible(false);
    m_kunyeButton = &button;
    m_lastStatMessage.clear();
    m_levelUpPoints = 0;
    m_statsSeen = {};
}

void WorldScene::setKunyeOpen(bool open)
{
    if (m_kunyeWindow == nullptr || (open && m_phase != Phase::Ready))
        return;
    if (open && (m_leavingWorld || m_model.leaving()))
        return;
    if (open == m_kunyeOpen)
        return;
    m_kunyeOpen = open;
    if (open)
        refreshKunye();
    m_kunyeWindow->setVisible(open);
    VO_LOG_INFO("world: Künye {}", open ? "opened" : "closed");
}

game::EquipBonus WorldScene::localEquipBonus() const
{
    const world::ItemData* items = ctx().resources.itemData();
    if (items == nullptr || !items->catalogLoaded || !m_inventory.loaded())
        return {};
    return game::equipmentBonus(items->catalog, m_inventory.confirmed());
}

void WorldScene::refreshItemRules()
{
    const world::ItemData* items = ctx().resources.itemData();
    game::ItemUser user;
    user.charClass = m_charClass;
    user.level = std::max<std::uint16_t>(m_self.level, 1);
    user.attributes = m_sheet.known() ? m_sheet.baseValues() : game::baseAttributes(m_charClass, {});
    m_inventory.setRules(items != nullptr && items->catalogLoaded ? &items->catalog : nullptr, user);
}

void WorldScene::refreshKunye()
{
    if (m_kunyeWindow == nullptr)
        return;
    KunyeWindow::Info info;
    info.name = m_name;
    info.charClass = m_charClass;
    info.classLine = describeCharacter(m_race, m_charClass, m_self.level);
    info.xp = m_self.xp;
    info.xpToNext = m_self.xpToNext;
    info.equip = localEquipBonus();
    m_kunyeWindow->refresh(info);
}

void WorldScene::showStatMessage(std::string_view text, bool error)
{
    m_lastStatMessage = std::string(text);
    if (m_kunyeWindow != nullptr)
        m_kunyeWindow->setMessage(std::string(text), error);
    if (!text.empty())
        VO_LOG_INFO("world: Künye message: {}", text);
}

// ---------------------------------------------------------------------------------------------
// Pending points and the request
// ---------------------------------------------------------------------------------------------

void WorldScene::kunyeAdd(std::size_t attribute, bool shift)
{
    const std::uint16_t added = m_sheet.add(attribute, shift ? kShiftStep : 1);
    if (added > 0)
    {
        VO_LOG_INFO("world: Künye: +{} {} pending ({} in total)", added, attributeKeyText(attribute),
                    m_sheet.pendingTotal());
        showStatMessage("", false);
    }
    refreshKunye();
}

void WorldScene::kunyeRemove(std::size_t attribute)
{
    if (m_sheet.remove(attribute, 1) > 0)
        VO_LOG_INFO("world: Künye: -1 {} pending ({} in total)", attributeKeyText(attribute), m_sheet.pendingTotal());
    refreshKunye();
}

void WorldScene::kunyeCancel()
{
    if (m_sheet.clearPending())
        VO_LOG_INFO("world: Künye: pending points dropped");
    showStatMessage("", false);
    refreshKunye();
}

void WorldScene::kunyeConfirm()
{
    if (!m_sheet.hasPending() || m_sheet.busy())
        return;
    auto& account = ctx().net.account();
    if (account.statSpendConfirmed)
    {
        sendStatAllocate();
        return;
    }
    // Spending is permanent: asked once per session (PROTOCOL_STATS.md §7).
    confirm("Künye", "Dağıtılan puanlar geri alınamaz. Onaylıyor musun?", "Evet", "Hayır", [this](bool yes) {
        if (!yes)
            return;
        ctx().net.account().statSpendConfirmed = true;
        sendStatAllocate();
    });
}

void WorldScene::sendStatAllocate()
{
    if (m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked())
        return; // never a STAT_ALLOCATE_REQ after LEAVE_WORLD_REQ
    const auto req = m_sheet.beginConfirm(m_time);
    if (!req)
        return;
    if (!ctx().net.send(*req))
    {
        VO_LOG_WARN("world: STAT_ALLOCATE_REQ could not be sent");
        (void)m_sheet.finish({req->seq, vn::StatAllocateResult::ServerError, m_sheet.unspent()}, m_time);
        refreshKunye();
        return;
    }
    VO_LOG_INFO("world: STAT_ALLOCATE_REQ seq {} points {}/{}/{}/{}/{}", req->seq, req->points[0], req->points[1],
                req->points[2], req->points[3], req->points[4]);
    showStatMessage("Kaydediliyor…", false);
    refreshKunye();
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

bool WorldScene::handleStatsMessage(const net::NetEvent& e)
{
    const auto op = static_cast<vn::Opcode>(e.opcode);
    if (op != vn::Opcode::CharStats && op != vn::Opcode::StatAllocateRes)
        return false;
    const auto malformed = [&]() {
        VO_LOG_ERROR("world: malformed {} payload ({} bytes)", vn::opcodeName(op), e.payload.size());
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return true;
    };
    if (op == vn::Opcode::CharStats)
    {
        vn::CharStats m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        const bool wasAwaiting = m_sheet.awaitingStats();
        m_sheet.applyCharStats(m);
        ++m_statsSeen.charStats;
        VO_LOG_INFO("world: CHAR_STATS level {} base {}/{}/{}/{}/{} bonus {}/{}/{}/{}/{} unspent {}; hp {} mp {} attack {} "
                    "defense {} crit {} dodge {} regen {}/{} interval {} heal {}",
                    m.level, m.attributes[0].base, m.attributes[1].base, m.attributes[2].base, m.attributes[3].base,
                    m.attributes[4].base, m.attributes[0].bonus, m.attributes[1].bonus, m.attributes[2].bonus,
                    m.attributes[3].bonus, m.attributes[4].bonus, m.unspent, m.maxHp, m.maxMp, m.attack, m.defense,
                    m.critPermille, m.dodgePermille, m.hpRegenTenths, m.mpRegenTenths, m.attackIntervalMs,
                    m.healPermille);
        if (wasAwaiting)
            showStatMessage("Puanlar dağıtıldı.", false);
        refreshItemRules();
        refreshKunye();
        return true;
    }
    vn::StatAllocateRes m;
    if (!vn::decodeMessage(e.payload, m))
        return malformed();
    const world::StatSheet::Answer a = m_sheet.finish(m, m_time);
    VO_LOG_INFO("world: STAT_ALLOCATE_RES seq {} {} unspent {}{}", m.seq, vn::statAllocateResultName(m.result), m.unspent,
                a.matched ? "" : " (not the request in flight)");
    if (a.matched)
    {
        if (m.result == vn::StatAllocateResult::Ok)
            ++m_statsSeen.accepted;
        else
        {
            ++m_statsSeen.refused;
            showStatMessage(world::statResultText(m.result), true);
        }
    }
    refreshKunye();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------

void WorldScene::updateStats(float)
{
    if (m_sheet.awaitingStats())
    {
        m_sheet.expire(m_time);
        if (!m_sheet.awaitingStats())
        {
            VO_LOG_WARN("world: no CHAR_STATS after an accepted STAT_ALLOCATE_RES; pending points dropped");
            refreshKunye();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::statsStates(std::vector<std::string>& out) const
{
    out.emplace_back(m_kunyeOpen ? "kunye" : "kunyeclosed");
    if (m_sheet.known())
    {
        out.emplace_back("statsknown");
        out.emplace_back("unspent" + std::to_string(m_sheet.unspent()));
        out.emplace_back(m_sheet.unspent() > 0 ? "points" : "nopoints");
        if (m_sheet.unspent() > 0 && m_kunyeButton != nullptr && m_kunyeButton->isVisible())
            out.emplace_back("unspentmarker");
    }
    out.emplace_back(m_sheet.hasPending() ? "pending" : "nopending");
    out.emplace_back(m_sheet.busy() ? "statbusy" : "statidle");
    bool canAdd = false;
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        canAdd = canAdd || m_sheet.canAdd(i);
    out.emplace_back(canAdd ? "canadd" : "noadd");
    if (m_levelUpAge < 3.2f && m_levelUpPoints > 0)
        out.emplace_back("levelpoints"); // the level-up toast shows the points line
    const auto counter = [&out](const char* name, std::uint32_t n) {
        for (std::uint32_t i = 1; i <= std::min<std::uint32_t>(n, 12); ++i)
            out.emplace_back(name + std::to_string(i));
    };
    counter("charstats", m_statsSeen.charStats);
    counter("allocated", m_statsSeen.accepted);
    counter("allocrefused", m_statsSeen.refused);
    counter("dodged", m_statsSeen.dodges);
    counter("seenmiss", m_statsSeen.missesSeen);
}

bool WorldScene::statsAutomation(std::string_view command, bool& handled, std::string& error)
{
    handled = false;
    std::string parseError;
    const auto cmd = world::parseStatsCommand(command, parseError);
    if (!cmd)
    {
        if (parseError.empty())
            return true; // not a stats command
        handled = true;
        error = parseError;
        return false;
    }
    handled = true;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    const std::size_t i = game::attributeIndex(cmd->attribute);
    const std::string_view name = game::attributeKey(cmd->attribute);
    switch (cmd->verb)
    {
    case world::StatsVerb::Kunye:
        setKunyeOpen(cmd->mode == "open");
        if (m_kunyeOpen != (cmd->mode == "open"))
            return fail("the Künye cannot be " + std::string(cmd->mode == "open" ? "opened" : "closed") + " now");
        return true;
    case world::StatsVerb::Add:
    {
        const std::uint16_t added = m_sheet.add(i, static_cast<std::uint16_t>(cmd->a));
        refreshKunye();
        if (added != cmd->a)
            return fail(std::format("statadd {} {}: only {} could be added (unspent {}, pending {})", name, cmd->a, added,
                                    m_sheet.unspent(), m_sheet.pendingTotal()));
        return true;
    }
    case world::StatsVerb::Remove:
    {
        const std::uint16_t removed = m_sheet.remove(i, static_cast<std::uint16_t>(cmd->a));
        refreshKunye();
        if (removed != cmd->a)
            return fail(std::format("statremove {} {}: only {} pending", name, cmd->a, removed));
        return true;
    }
    case world::StatsVerb::ExpectAttr:
        if (!m_sheet.known())
            return fail("expectattr: no CHAR_STATS yet");
        if (m_sheet.base(i) != cmd->a || (cmd->b >= 0 && m_sheet.bonus(i) != static_cast<std::uint32_t>(cmd->b)))
            return fail(std::format("{} is {} (+{}), expected {}{}", name, m_sheet.base(i), m_sheet.bonus(i), cmd->a,
                                    cmd->b >= 0 ? std::format(" (+{})", cmd->b) : std::string()));
        return true;
    case world::StatsVerb::ExpectUnspent:
        if (!m_sheet.known())
            return fail("expectunspent: no CHAR_STATS yet");
        if (m_sheet.unspent() != cmd->a)
            return fail(std::format("{} unspent point(s), expected {}", m_sheet.unspent(), cmd->a));
        return true;
    case world::StatsVerb::ExpectPending:
        if (m_sheet.pending(i) != cmd->a)
            return fail(std::format("{} pending on {}, expected {}", m_sheet.pending(i), name, cmd->a));
        return true;
    case world::StatsVerb::ExpectDerived:
    case world::StatsVerb::ExpectPreview:
    {
        if (!m_sheet.known())
            return fail("no CHAR_STATS yet");
        const vn::CharStats s = cmd->verb == world::StatsVerb::ExpectDerived
                                    ? m_sheet.stats()
                                    : world::toCharStats(m_sheet.preview(m_charClass, localEquipBonus()), m_sheet.level());
        const double v = world::derivedValue(s, cmd->field);
        if (std::fabs(v - cmd->value) > 0.051)
            return fail(std::format("{} {} is {}, expected {}", cmd->verb == world::StatsVerb::ExpectDerived ? "derived" : "preview",
                                    world::derivedFieldKey(cmd->field), v, cmd->value));
        return true;
    }
    case world::StatsVerb::ExpectMessage:
        if (m_lastStatMessage.find(cmd->text) == std::string::npos)
            return fail(std::format("last Künye message is '{}', expected '{}'", m_lastStatMessage, cmd->text));
        return true;
    case world::StatsVerb::Log:
    {
        const vn::CharStats& s = m_sheet.stats();
        VO_LOG_INFO("world: Künye: known {} level {} unspent {} pending {} busy {} open {}; {}", m_sheet.known(), s.level,
                    s.unspent, m_sheet.pendingTotal(), m_sheet.busy(), m_kunyeOpen,
                    world::derivedRowsText(world::derivedRows(m_charClass, s, nullptr)));
        for (std::size_t k = 0; k < game::kAttributeCount; ++k)
            VO_LOG_INFO("world:   {}", world::attributeRowText(static_cast<game::Attribute>(k), m_sheet.base(k),
                                                               m_sheet.bonus(k), m_sheet.pending(k)));
        // The client's own derivation must match the server's numbers (same shared function).
        const vn::CharStats mine = world::toCharStats(m_sheet.current(m_charClass, localEquipBonus()), s.level);
        if (m_sheet.known() && (mine.maxHp != s.maxHp || mine.maxMp != s.maxMp || mine.attack != s.attack ||
                                mine.defense != s.defense || mine.critPermille != s.critPermille ||
                                mine.dodgePermille != s.dodgePermille))
            VO_LOG_WARN("world: Künye: client derivation differs: hp {} mp {} attack {} defense {} crit {} dodge {}",
                        mine.maxHp, mine.maxMp, mine.attack, mine.defense, mine.critPermille, mine.dodgePermille);
        return true;
    }
    }
    return true;
}

} // namespace vo::client::scenes
