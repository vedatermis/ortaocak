#include "client/scenes/QuestWindows.h"

#include "client/render/TextRenderer.h"
#include "client/scenes/ItemTooltip.h"
#include "client/scenes/SceneResources.h"
#include "client/ui/Button.h"
#include "client/ui/DrawContext.h"
#include "client/ui/Label.h"
#include "client/ui/UiContext.h"
#include "client/world/ClientInventory.h"
#include "client/world/ShopView.h"
#include "shared/game/Npcs.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
using game::QuestStatus;

namespace {

constexpr float kPadX = 12.0f;
constexpr float kPadTop = 4.0f;
constexpr float kPadBottom = 10.0f;
constexpr float kTitleBar = 38.0f;
constexpr float kButtonsH = 34.0f;
constexpr float kMessageH = 16.0f;
const Color kGold = Color::hex(0xF2CA64);
const Color kRed = Color::hex(0xE5736A);
const Color kGreen = Color::hex(0x9BE07A);
const Color kWords = Color::hex(0xD8CCAA); // an NPC's words

class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

struct BodyHit
{
    Rect rect;
    std::uint16_t item = 0;
    int choice = -1;
};

Color statusColor(QuestStatus s)
{
    switch (s)
    {
    case QuestStatus::Ready: return Color::hex(world::kQuestReadyRgb);
    case QuestStatus::Available: return Color::hex(world::kQuestAvailableRgb);
    case QuestStatus::Active: return Color::hex(world::kQuestProgressRgb);
    default: return Color::hex(0x8A8578);
    }
}

game::QuestMarker statusMarker(QuestStatus s)
{
    switch (s)
    {
    case QuestStatus::Ready: return game::QuestMarker::Ready;
    case QuestStatus::Available: return game::QuestMarker::Available;
    case QuestStatus::Active: return game::QuestMarker::InProgress;
    default: return game::QuestMarker::None;
    }
}

// Wrapped paragraph at `y`; returns the height used (physical).
float paragraph(ui::DrawContext& ctx, const render::Font& f, std::string_view text, float x, float y, float w,
                const render::TextStyle& style)
{
    if (text.empty())
        return 0.0f;
    std::vector<ui::TextLine> lines;
    ui::wrapText(f, text, w, lines);
    const float h = f.lineHeight() * static_cast<float>(lines.size());
    render::drawTextLines(ctx.batch, f, text, lines, {x, y, w, h}, render::HAlign::Left, render::VAlign::Top, style);
    return h;
}

void drawItemIcon(ui::DrawContext& ctx, const SceneResources& res, std::uint16_t templateId, const Rect& r, bool lit)
{
    auto& b = ctx.batch;
    b.fillRoundedRect(r, ctx.pxf(3.0f), Color::hex(0x000000, 0.45f));
    b.strokeRoundedRect(r, ctx.pxf(3.0f), std::max(1.0f, ctx.pxf(lit ? 2.0f : 1.0f)),
                        lit ? ctx.theme.accentBright : ctx.theme.insetBorder);
    if (const render::Texture2D* icon = res.itemIcon(templateId))
        b.drawImage(*icon, r.inset(ctx.px(2.0f)), res.itemTint(templateId));
}

// The quest body: name, status, the NPC's words, objectives, rewards, the choice row, the footnote. Drawn from
// `area`'s top minus `scroll` (clipped by the caller); returns the content height. Item icons and choice
// items are added to `hits`.
float drawQuestBody(ui::DrawContext& ctx, const SceneResources& res, const world::ClientInventory& inv,
                    const QuestBodyView& v, const Rect& area, float scroll, std::vector<BodyHit>& hits)
{
    if (v.quest == nullptr)
        return 0.0f;
    const game::QuestDef& q = *v.quest;
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const float x = area.x;
    const float w = area.w;
    float y = area.y - scroll;
    const game::ItemCatalog* items = inv.catalog();
    render::TextStyle shadow;
    shadow.shadow = true;
    shadow.shadowColor = t.textShadow;

    // Name and status.
    const render::Font& nameFont = ctx.font(16.0f, render::FontWeight::Bold);
    render::TextStyle ns = shadow;
    ns.color = kGold;
    y += paragraph(ctx, nameFont, q.name, x, y, w, ns);
    const std::string status = world::questStatusText(q, v.status);
    if (!status.empty())
    {
        const render::Font& sf = ctx.font(12.0f, render::FontWeight::Bold);
        render::TextStyle ss = shadow;
        ss.color = statusColor(v.status);
        render::drawTextAligned(b, sf, status, {x, y, w, sf.lineHeight()}, render::HAlign::Left, render::VAlign::Top, ss);
        y += sf.lineHeight();
    }
    y += ctx.px(6.0f);
    // The NPC's words (quoted, warm).
    if (!v.npcText.empty())
    {
        const render::Font& wf = ctx.font(13.0f);
        render::TextStyle ws = shadow;
        ws.color = kWords;
        ws.slant = render::kSyntheticItalicSlant * 0.6f;
        y += paragraph(ctx, wf, std::format("“{}”", v.npcText), x, y, w, ws);
        y += ctx.px(8.0f);
    }
    const render::Font& head = ctx.font(13.0f, render::FontWeight::Bold);
    const render::Font& line = ctx.font(13.0f);
    const auto header = [&](std::string_view text) {
        render::TextStyle hs = shadow;
        hs.color = t.ornament;
        render::drawTextAligned(b, head, text, {x, y, w, head.lineHeight()}, render::HAlign::Left, render::VAlign::Top, hs);
        y += head.lineHeight() + ctx.px(2.0f);
    };
    // Objectives (with progress while active).
    header("Hedefler");
    game::QuestRecord fresh;
    fresh.quest = q.id;
    fresh.state = vo::net::QuestState::None;
    const game::QuestRecord& rec = v.record != nullptr ? *v.record : fresh;
    const bool active = v.record != nullptr && v.record->state == vo::net::QuestState::Active;
    for (std::size_t i = 0; i < q.objectives.size(); ++i)
    {
        const std::uint16_t p = active ? game::objectiveProgress(q, rec, inv.view(), items, i) : 0;
        const bool done = active && p >= q.objectives[i].count;
        std::string text = world::objectiveText(q, i, p);
        if (!active && q.objectives[i].kind != game::ObjectiveKind::Talk)
            text = q.objectives[i].kind == game::ObjectiveKind::Boss
                       ? std::format("Yen: {} ({})", q.objectives[i].label, q.objectives[i].count)
                       : std::format("{} × {}", q.objectives[i].count, q.objectives[i].label);
        const float bullet = ctx.px(4.0f);
        b.fillDiamond({x + ctx.px(6.0f), y + line.lineHeight() * 0.5f}, bullet, done ? kGreen : t.ornament.withAlpha(0.85f));
        render::TextStyle ls = shadow;
        ls.color = done ? kGreen : t.text;
        y += paragraph(ctx, line, text, x + ctx.px(16.0f), y, w - ctx.px(16.0f), ls);
    }
    y += ctx.px(8.0f);
    // Rewards: experience, Akçe, items (icon + name; hover: tooltip).
    const std::vector<world::RewardLine> rewards = world::rewardLines(q, v.charClass, items);
    const std::vector<world::RewardLine> choices = world::rewardChoices(q, items);
    if (!rewards.empty() || !choices.empty())
        header("Ödüller");
    const float icon = ctx.px(24.0f);
    for (const world::RewardLine& r : rewards)
    {
        render::TextStyle rs = shadow;
        rs.color = r.templateId != 0 ? t.text : kGold;
        if (r.templateId != 0)
        {
            const Rect ir = Rect{x, y, icon, icon}.snapped();
            drawItemIcon(ctx, res, r.templateId, ir, false);
            hits.push_back({ir, r.templateId, -1});
            if (const game::ItemTemplate* it = items != nullptr ? items->find(r.templateId) : nullptr)
                rs.color = Color::hex(world::rarityRgb(it->rarity));
            render::drawTextAligned(b, line, r.text, {ir.right() + ctx.px(8.0f), y, w - icon - ctx.px(8.0f), icon},
                                    render::HAlign::Left, render::VAlign::Middle, rs);
            y += icon + ctx.px(3.0f);
        }
        else
        {
            b.fillDiamond({x + ctx.px(6.0f), y + line.lineHeight() * 0.5f}, ctx.px(4.0f), kGold.withAlpha(0.85f));
            render::drawTextAligned(b, line, r.text, {x + ctx.px(16.0f), y, w, line.lineHeight()}, render::HAlign::Left,
                                    render::VAlign::Top, rs);
            y += line.lineHeight() + ctx.px(2.0f);
        }
    }
    if (!choices.empty())
    {
        y += ctx.px(2.0f);
        render::TextStyle cs = shadow;
        cs.color = t.textDim;
        const std::string pick = v.selectedChoice >= 0 && static_cast<std::size_t>(v.selectedChoice) < choices.size()
                                     ? std::format("Seçilen: {}", choices[static_cast<std::size_t>(v.selectedChoice)].text)
                                     : std::string("Birini seç (tıkla):");
        render::drawTextAligned(b, line, pick, {x, y, w, line.lineHeight()}, render::HAlign::Left, render::VAlign::Top, cs);
        y += line.lineHeight() + ctx.px(3.0f);
        const float big = ctx.px(38.0f);
        float cx = x;
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            const Rect cr = Rect{cx, y, big, big}.snapped();
            drawItemIcon(ctx, res, choices[i].templateId, cr, static_cast<int>(i) == v.selectedChoice);
            hits.push_back({cr, choices[i].templateId, static_cast<int>(i)});
            cx += big + ctx.px(6.0f);
        }
        y += big + ctx.px(4.0f);
    }
    if (!v.footnote.empty())
    {
        y += ctx.px(4.0f);
        render::TextStyle fs = shadow;
        fs.color = t.textDim;
        y += paragraph(ctx, ctx.font(12.0f), v.footnote, x, y, w, fs);
    }
    return y + scroll - area.y;
}

std::string bodyPlainText(const QuestBodyView& v, const world::ClientInventory& inv)
{
    if (v.quest == nullptr)
        return {};
    const game::QuestDef& q = *v.quest;
    std::string out = q.name + "\n" + world::questStatusText(q, v.status) + "\n" + v.npcText;
    if (v.record != nullptr && v.record->state == vo::net::QuestState::Active)
        for (const world::ObjectiveLine& l : world::objectiveLines(q, *v.record, inv.view(), inv.catalog()))
            out += "\n" + l.text + (l.done ? " (tamam)" : "");
    else
        for (std::size_t i = 0; i < q.objectives.size(); ++i)
            out += "\nHedef: " + world::objectiveText(q, i, 0);
    for (const world::RewardLine& r : world::rewardLines(q, v.charClass, inv.catalog()))
        out += "\nÖdül: " + r.text;
    for (const world::RewardLine& r : world::rewardChoices(q, inv.catalog()))
        out += "\nSeçmeli: " + r.text;
    if (!v.footnote.empty())
        out += "\n" + v.footnote;
    return out;
}

void drawRewardTooltip(ui::DrawContext& ctx, const world::ClientInventory& inv, std::uint16_t templateId, const Rect& anchor,
                       const ui::UiContext& ui)
{
    const game::ItemCatalog* catalog = inv.catalog();
    const game::ItemTemplate* item = catalog != nullptr ? catalog->find(templateId) : nullptr;
    if (item == nullptr)
        return;
    const std::vector<world::TipLine> tip = world::itemTipLines(*item, inv.user(), 0);
    drawTooltipBox(ctx, shownTipLines(tip, ctx.theme), anchor, ui.viewportWidth(), ui.viewportHeight());
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The marker
// ---------------------------------------------------------------------------------------------

void drawQuestMarker(render::SpriteBatch& b, Vec2 c, float size, game::QuestMarker marker, double time, float alpha)
{
    if (marker == game::QuestMarker::None || size < 4.0f || alpha <= 0.0f)
        return;
    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(time * 3.2));
    const float half = size * 0.5f;          // the gem's half height
    const float stroke = std::max(1.5f, size * 0.07f);
    const Color dark = Color::hex(0x120C05, 0.92f * alpha);
    // The stem under the gem (a short ember-lit pin), then the gem.
    b.drawLine({c.x, c.y + half * 0.9f}, {c.x, c.y + half * 1.55f}, std::max(2.0f, size * 0.10f), dark, true);
    b.drawLine({c.x, c.y + half * 0.9f}, {c.x, c.y + half * 1.5f}, std::max(1.0f, size * 0.05f),
               Color::hex(0xD49A3A, 0.85f * alpha), true);
    switch (marker)
    {
    case game::QuestMarker::Available:
    {
        // A solid amber köz gem with a bright core and a faint glow.
        b.fillDiamond(c, half * 1.35f, Color::hex(world::kQuestAvailableRgb, (0.10f + 0.12f * pulse) * alpha));
        b.fillDiamond(c, half + stroke, dark);
        b.fillDiamond(c, half, Color::hex(0xE08A1E, alpha));
        b.fillDiamond({c.x, c.y - half * 0.12f}, half * 0.72f, Color::hex(world::kQuestAvailableRgb, alpha));
        b.fillDiamond({c.x, c.y - half * 0.22f}, half * 0.30f, Color::hex(0xFFF2C4, alpha));
        break;
    }
    case game::QuestMarker::InProgress:
    {
        // A hollow ash-grey gem: an outline with a dim heart (something is still to be done).
        b.fillDiamond(c, half + stroke, dark);
        b.fillDiamond(c, half, Color::hex(world::kQuestProgressRgb, 0.95f * alpha));
        b.fillDiamond(c, half - stroke * 1.6f, Color::hex(0x2A2C30, 0.95f * alpha));
        b.fillDiamond(c, half * 0.22f, Color::hex(world::kQuestProgressRgb, 0.8f * alpha));
        break;
    }
    case game::QuestMarker::Ready:
    {
        // A green-gold gem with a pulsing halo and a tick: come back and hand it in.
        const float halo = half * (1.45f + 0.18f * pulse);
        b.fillRoundedRect({c.x - halo, c.y - halo, 2.0f * halo, 2.0f * halo}, halo,
                          Color::hex(world::kQuestReadyRgb, (0.16f + 0.14f * pulse) * alpha));
        b.fillDiamond(c, half + stroke, dark);
        b.fillDiamond(c, half, Color::hex(0x3E9A3A, alpha));
        b.fillDiamond({c.x, c.y - half * 0.1f}, half * 0.78f, Color::hex(world::kQuestReadyRgb, alpha));
        const float tw = std::max(2.0f, size * 0.11f);
        const Vec2 a{c.x - half * 0.38f, c.y + half * 0.02f};
        const Vec2 m{c.x - half * 0.08f, c.y + half * 0.32f};
        const Vec2 e{c.x + half * 0.42f, c.y - half * 0.34f};
        b.drawLine(a, m, tw + 2.0f, dark, true);
        b.drawLine(m, e, tw + 2.0f, dark, true);
        b.drawLine(a, m, tw, Color::hex(0xFFFBE8, alpha), true);
        b.drawLine(m, e, tw, Color::hex(0xFFFBE8, alpha), true);
        break;
    }
    case game::QuestMarker::None: break;
    }
}

// ---------------------------------------------------------------------------------------------
// QuestDialog
// ---------------------------------------------------------------------------------------------

QuestDialog::QuestDialog(const SceneResources& resources, const world::ClientInventory& inventory, Hooks hooks)
    : ui::Panel(ui::Panel::Style::Window, "Görev"), m_resources(resources), m_inventory(inventory), m_hooks(std::move(hooks))
{
    setName("quest.dialog");
    setPadding({kPadX, kPadTop, kPadX, kPadBottom});
    setSize(kWidth, kHeight);
    const float bw = std::floor((kWidth - 2.0f * kPadX - 8.0f) * 0.5f);
    const auto button = [&](const char* text, const char* name, std::function<void()> click) {
        auto& btn = add<ui::Button>(text, std::move(click));
        btn.setFocusable(false);
        btn.setAnchors(anchor::kBottomLeft).setSize(bw, kButtonsH);
        btn.setName(name);
        return &btn;
    };
    m_accept = button("Kabul et", "quest.accept", [this]() {
        if (m_view.body.quest != nullptr && m_hooks.accept)
            m_hooks.accept(m_view.body.quest->id);
    });
    m_handIn = button("Teslim et", "quest.handin", [this]() {
        if (m_view.body.quest != nullptr && m_hooks.handIn)
            m_hooks.handIn(m_view.body.quest->id, m_view.body.selectedChoice < 0
                                                      ? vo::net::kQuestNoChoice
                                                      : static_cast<std::uint8_t>(m_view.body.selectedChoice));
    });
    m_decline = button("Vazgeç", "quest.decline", [this]() {
        if (m_hooks.decline)
            m_hooks.decline();
    });
    m_back = button("Geri", "quest.back", [this]() {
        if (m_hooks.back)
            m_hooks.back();
    });
    m_shop = button(std::string(world::kQuestShopButton).c_str(), "quest.shop", [this]() {
        if (m_hooks.shop)
            m_hooks.shop();
    });
    auto& close = add<ui::Button>("X", [this]() {
        if (m_hooks.decline)
            m_hooks.decline();
    });
    close.setFocusable(false);
    close.setAnchors(anchor::kTopRight).setMargins({0.0f, -34.0f, -6.0f, 0.0f}).setSize(28.0f, 28.0f);
    close.setName("quest.close");
    auto probe = std::make_unique<TextProbe>();
    probe->setName("quest.text");
    probe->setSize(0.0f, 0.0f);
    m_probe = probe.get();
    addChild(std::move(probe));
    relayout();
}

void QuestDialog::setView(View v)
{
    const bool other = m_view.body.quest != v.body.quest || m_view.npcName != v.npcName;
    m_view = std::move(v);
    if (other)
        m_scroll = 0.0f;
    setTitle(m_view.npcName);
    relayout();
}

void QuestDialog::setMaxHeight(float logical)
{
    if (std::fabs(logical - m_maxHeight) < 0.5f)
        return;
    m_maxHeight = logical;
    relayout();
}

float QuestDialog::preferredHeight() const noexcept
{
    return m_maxHeight > 0.0f ? std::clamp(m_maxHeight, 260.0f, kHeight) : kHeight;
}

void QuestDialog::relayout()
{
    setSize(kWidth, preferredHeight());
    const float bw = std::floor((kWidth - 2.0f * kPadX - 8.0f) * 0.5f);
    const bool detail = m_view.body.quest != nullptr;
    const QuestStatus s = m_view.body.status;
    const bool canAccept = detail && s == QuestStatus::Available;
    const bool canHandIn = detail && s == QuestStatus::Ready && m_view.body.record != nullptr;
    // Bottom row: [Kabul et | Teslim et] [Vazgeç]; in the list: [Alışveriş] [Vazgeç]. "Geri" above the
    // right button when there is a list to go back to.
    const auto at = [&](ui::Button* b, int col, int row, bool shown) {
        b->setVisible(shown);
        b->setMargins({col == 0 ? 0.0f : bw + 8.0f, 0.0f, 0.0f, static_cast<float>(row) * (kButtonsH + 6.0f)});
    };
    at(m_accept, 0, 0, canAccept);
    at(m_handIn, 0, 0, canHandIn);
    at(m_shop, 0, detail ? 1 : 0, m_view.merchant && (!detail || (!canAccept && !canHandIn)));
    if (detail && !canAccept && !canHandIn && m_view.merchant)
        at(m_shop, 0, 0, true);
    at(m_decline, 1, 0, true);
    m_decline->setText(detail && (canAccept || canHandIn) ? "Vazgeç" : "Kapat");
    at(m_back, 1, 1, detail && m_view.entries.size() > 1);
    m_accept->setEnabled(!m_view.busy);
    m_handIn->setEnabled(!m_view.busy && (m_view.body.quest == nullptr || m_view.body.quest->reward.choice.empty() ||
                                          m_view.body.selectedChoice >= 0));
    m_probe->setText(plainText());
}

std::string QuestDialog::plainText() const
{
    std::string out = m_view.npcName;
    if (m_view.body.quest != nullptr)
        out += "\n" + bodyPlainText(m_view.body, m_inventory);
    else
        for (const world::NpcQuestEntry& e : m_view.entries)
            out += std::format("\n{} — {}", e.quest->name, world::questStatusText(*e.quest, e.status));
    if (!m_view.message.empty())
        out += "\n" + m_view.message;
    return out;
}

void QuestDialog::drawSelf(ui::DrawContext& ctx)
{
    ui::Panel::drawSelf(ctx);
    auto& b = ctx.batch;
    const ui::Theme& t = ctx.theme;
    const Rect c = contentRect();
    m_hits.clear();
    render::TextStyle shadow;
    shadow.shadow = true;
    shadow.shadowColor = t.textShadow;
    // The NPC's title, then the body area down to the buttons (and the message line above them).
    const render::Font& tf = ctx.font(12.0f, render::FontWeight::Bold);
    render::TextStyle ts = shadow;
    ts.color = Color::hex(world::kNpcTitleRgb);
    render::drawTextAligned(b, tf, world::npcTitlePlate(m_view.npcTitle), {c.x, c.y, c.w, tf.lineHeight()},
                            render::HAlign::Center, render::VAlign::Top, ts);
    const bool detail = m_view.body.quest != nullptr;
    const int buttonRows = detail && (m_view.entries.size() > 1 || (m_view.merchant && m_accept->isVisible())) ? 2 : 1;
    const float bottom = c.bottom() - ctx.px(static_cast<float>(buttonRows) * (kButtonsH + 6.0f) + kMessageH + 4.0f);
    float top = c.y + tf.lineHeight() + ctx.px(6.0f);
    b.drawLine({c.x, std::round(top) + 0.5f}, {c.right(), std::round(top) + 0.5f}, std::max(1.0f, ctx.pxf(1.0f)),
               t.ornament.withAlpha(0.3f));
    top += ctx.px(6.0f);
    const Rect body{c.x, top, c.w, std::max(1.0f, bottom - top)};
    b.pushClip(body);
    if (detail)
    {
        std::vector<BodyHit> hits;
        m_bodyH = drawQuestBody(ctx, m_resources, m_inventory, m_view.body, body, m_scroll, hits);
        for (const BodyHit& h : hits)
            m_hits.push_back({h.rect, 0, h.item, h.choice});
    }
    else
    {
        // The greeting, then one row per quest.
        float y = body.y - m_scroll;
        if (!m_view.greeting.empty())
        {
            render::TextStyle gs = shadow;
            gs.color = kWords;
            gs.slant = render::kSyntheticItalicSlant * 0.6f;
            y += paragraph(ctx, ctx.font(13.0f), std::format("“{}”", m_view.greeting), body.x, y, body.w, gs);
            y += ctx.px(10.0f);
        }
        const float rowH = ctx.px(40.0f);
        const render::Font& nf = ctx.font(14.0f, render::FontWeight::Bold);
        const render::Font& sf = ctx.font(12.0f);
        for (const world::NpcQuestEntry& e : m_view.entries)
        {
            const Rect r = Rect{body.x, y, body.w, rowH}.snapped();
            const bool later = e.status == QuestStatus::Later;
            const bool hot = !later && isHovered() && r.contains(m_mouse.x, m_mouse.y);
            b.fillRoundedRect(r, ctx.pxf(4.0f), hot ? t.rowHover.withAlpha(0.95f) : Color::hex(0x0C0F13, 0.72f));
            b.strokeRoundedRect(r, ctx.pxf(4.0f), std::max(1.0f, ctx.pxf(1.0f)), hot ? t.buttonBorderHover : t.insetBorder);
            drawQuestMarker(b, {r.x + ctx.px(16.0f), r.centerY() - ctx.px(2.0f)}, ctx.px(18.0f), statusMarker(e.status), ctx.time,
                            later ? 0.5f : 1.0f);
            render::TextStyle ns = shadow;
            ns.color = later ? t.textDim : t.text;
            const float tx = r.x + ctx.px(34.0f);
            render::drawTextAligned(b, nf, e.quest->name, {tx, r.y + ctx.px(3.0f), r.w - ctx.px(40.0f), nf.lineHeight()},
                                    render::HAlign::Left, render::VAlign::Top, ns);
            render::TextStyle ss = shadow;
            ss.color = statusColor(e.status);
            render::drawTextAligned(b, sf, world::questStatusText(*e.quest, e.status),
                                    {tx, r.y + ctx.px(3.0f) + nf.lineHeight(), r.w - ctx.px(40.0f), sf.lineHeight()},
                                    render::HAlign::Left, render::VAlign::Top, ss);
            if (!later)
                m_hits.push_back({r, e.quest->id, 0, -1});
            y += rowH + ctx.px(4.0f);
        }
        if (m_view.entries.empty())
        {
            render::TextStyle es = shadow;
            es.color = t.textDim;
            y += paragraph(ctx, ctx.font(13.0f), "Şimdilik sana bir işi yok.", body.x, y, body.w, es);
        }
        m_bodyH = y + m_scroll - body.y;
    }
    b.popClip();
    // The body scrolls: a thin bar on the right when it does not fit.
    const float maxScroll = std::max(0.0f, m_bodyH - body.h);
    m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);
    if (maxScroll > 0.5f)
    {
        const float trackX = body.right() + ctx.px(5.0f);
        const float th = std::max(ctx.px(18.0f), body.h * body.h / m_bodyH);
        const float ty = body.y + (body.h - th) * (m_scroll / maxScroll);
        b.fillRoundedRect({trackX, ty, ctx.px(3.0f), th}, ctx.pxf(1.5f), t.ornament.withAlpha(0.6f));
    }
    // The message line above the buttons.
    if (!m_view.message.empty())
    {
        render::TextStyle ms = shadow;
        ms.color = m_view.messageError ? kRed : Color::hex(0xE9E2CF);
        render::drawTextAligned(b, ctx.font(12.0f), m_view.message, {c.x, bottom, c.w, ctx.px(kMessageH)}, render::HAlign::Center,
                                render::VAlign::Middle, ms);
    }
    else if (m_view.busy)
    {
        render::TextStyle ms = shadow;
        ms.color = t.textDim;
        render::drawTextAligned(b, ctx.font(12.0f), "Bekleniyor…", {c.x, bottom, c.w, ctx.px(kMessageH)}, render::HAlign::Center,
                                render::VAlign::Middle, ms);
    }
}

void QuestDialog::draw(ui::DrawContext& ctx)
{
    if (!isVisible())
        return;
    ui::Panel::draw(ctx);
    if (context() == nullptr || context()->hasModal())
        return;
    if (!isHovered())
        return;
    const float mx = m_mouse.x;
    const float my = m_mouse.y;
    for (const Hit& h : m_hits)
        if (h.item != 0 && h.rect.contains(mx, my))
        {
            drawRewardTooltip(ctx, m_inventory, h.item, h.rect, *context());
            break;
        }
}

bool QuestDialog::onMouseDown(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return true;
    const auto x = static_cast<float>(e.x);
    const auto y = static_cast<float>(e.y);
    for (const Hit& h : m_hits)
    {
        if (!h.rect.contains(x, y))
            continue;
        if (h.quest != 0 && m_hooks.select)
            m_hooks.select(h.quest);
        else if (h.choice >= 0 && m_hooks.choose)
            m_hooks.choose(h.choice);
        break;
    }
    return true;
}

bool QuestDialog::onMouseMove(const platform::InputEvent& e)
{
    m_mouse = {static_cast<float>(e.x), static_cast<float>(e.y)};
    return true;
}

bool QuestDialog::onMouseWheel(const platform::InputEvent& e)
{
    if (context() != nullptr && e.wheel != 0.0f)
        m_scroll = std::max(0.0f, m_scroll - e.wheel * 40.0f * context()->scale());
    return true;
}

// ---------------------------------------------------------------------------------------------
// QuestLogWindow
// ---------------------------------------------------------------------------------------------

QuestLogWindow::QuestLogWindow(const SceneResources& resources, const world::ClientInventory& inventory, Hooks hooks)
    : ui::Panel(ui::Panel::Style::Window, std::string(world::kQuestLogTitle)), m_resources(resources), m_inventory(inventory),
      m_hooks(std::move(hooks))
{
    setName("quest.log");
    setPadding({kPadX, kPadTop, kPadX, kPadBottom});
    setSize(kWidth, kHeight);
    m_abandon = &add<ui::Button>("Bırak", [this]() {
        if (m_view.body.quest != nullptr && m_hooks.abandon)
            m_hooks.abandon(m_view.body.quest->id);
    });
    m_abandon->setFocusable(false);
    m_abandon->setAnchors(anchor::kBottomRight).setSize(120.0f, kButtonsH);
    m_abandon->setName("quest.log.abandon");
    auto& close = add<ui::Button>("X", [this]() {
        if (m_hooks.close)
            m_hooks.close();
    });
    close.setFocusable(false);
    close.setAnchors(anchor::kTopRight).setMargins({0.0f, -34.0f, -6.0f, 0.0f}).setSize(28.0f, 28.0f);
    close.setName("quest.log.close");
    auto probe = std::make_unique<TextProbe>();
    probe->setName("quest.log.text");
    probe->setSize(0.0f, 0.0f);
    m_probe = probe.get();
    addChild(std::move(probe));
}

void QuestLogWindow::setView(View v)
{
    if (m_view.body.quest != v.body.quest)
        m_scroll = 0.0f;
    m_view = std::move(v);
    m_abandon->setVisible(m_view.body.quest != nullptr);
    m_abandon->setEnabled(!m_view.busy);
    m_probe->setText(plainText());
}

void QuestLogWindow::setMaxHeight(float logical)
{
    if (std::fabs(logical - m_maxHeight) < 0.5f)
        return;
    m_maxHeight = logical;
    setSize(kWidth, preferredHeight());
}

float QuestLogWindow::preferredHeight() const noexcept
{
    return m_maxHeight > 0.0f ? std::clamp(m_maxHeight, 260.0f, kHeight) : kHeight;
}

std::string QuestLogWindow::plainText() const
{
    std::string out = std::format("Görevler {}/{}", m_view.rows.size(), m_view.limit);
    for (const Row& r : m_view.rows)
        out += std::format("\n{}{}", r.name, r.ready ? " (teslim edilebilir)" : "");
    if (m_view.body.quest != nullptr)
        out += "\n--\n" + bodyPlainText(m_view.body, m_inventory);
    else if (m_view.rows.empty())
        out += "\n" + std::string(world::kQuestLogEmpty);
    return out;
}

void QuestLogWindow::drawSelf(ui::DrawContext& ctx)
{
    ui::Panel::drawSelf(ctx);
    auto& b = ctx.batch;
    const ui::Theme& t = ctx.theme;
    const Rect c = contentRect();
    m_hits.clear();
    render::TextStyle shadow;
    shadow.shadow = true;
    shadow.shadowColor = t.textShadow;
    // "Etkin görevler 3 / 10", then one row per active quest, a rule, then the selected quest.
    const render::Font& hf = ctx.font(12.0f, render::FontWeight::Bold);
    render::TextStyle hs = shadow;
    hs.color = t.textDim;
    render::drawTextAligned(b, hf, std::format("Etkin görevler {} / {}", m_view.rows.size(), m_view.limit),
                            {c.x, c.y, c.w, hf.lineHeight()}, render::HAlign::Left, render::VAlign::Top, hs);
    float y = c.y + hf.lineHeight() + ctx.px(4.0f);
    const float rowH = ctx.px(26.0f);
    const render::Font& nf = ctx.font(13.0f, render::FontWeight::Bold);
    // At most 5 rows show (the rest: scroll the whole window body below).
    const std::size_t shownRows = std::min<std::size_t>(m_view.rows.size(), 5);
    for (std::size_t i = 0; i < shownRows; ++i)
    {
        const Row& row = m_view.rows[i];
        const Rect r = Rect{c.x, y, c.w, rowH}.snapped();
        const bool sel = m_view.body.quest != nullptr && m_view.body.quest->id == row.quest;
        const bool hot = isHovered() && r.contains(m_mouse.x, m_mouse.y);
        b.fillRoundedRect(r, ctx.pxf(4.0f), sel ? t.accent.withAlpha(0.25f) : (hot ? t.rowHover : Color::hex(0x0C0F13, 0.6f)));
        b.strokeRoundedRect(r, ctx.pxf(4.0f), std::max(1.0f, ctx.pxf(1.0f)), sel ? t.accentBright : t.insetBorder);
        drawQuestMarker(b, {r.x + ctx.px(12.0f), r.centerY() - ctx.px(1.0f)}, ctx.px(14.0f),
                        row.ready ? game::QuestMarker::Ready : game::QuestMarker::InProgress, ctx.time);
        render::TextStyle ns = shadow;
        ns.color = row.ready ? kGreen : t.text;
        render::drawTextAligned(b, nf, row.name, {r.x + ctx.px(26.0f), r.y, r.w - ctx.px(30.0f), r.h}, render::HAlign::Left,
                                render::VAlign::Middle, ns);
        m_hits.push_back({r, row.quest, 0});
        y += rowH + ctx.px(3.0f);
    }
    if (m_view.rows.size() > shownRows)
    {
        render::TextStyle ms = shadow;
        ms.color = t.textDim;
        render::drawTextAligned(b, ctx.font(12.0f), std::format("… ve {} görev daha", m_view.rows.size() - shownRows),
                                {c.x, y, c.w, ctx.px(16.0f)}, render::HAlign::Left, render::VAlign::Top, ms);
        y += ctx.px(16.0f);
    }
    y += ctx.px(4.0f);
    b.drawLine({c.x, std::round(y) + 0.5f}, {c.right(), std::round(y) + 0.5f}, std::max(1.0f, ctx.pxf(1.0f)), t.ornament.withAlpha(0.3f));
    y += ctx.px(6.0f);
    const float bottom = c.bottom() - ctx.px(kButtonsH + 6.0f);
    const Rect body{c.x, y, c.w, std::max(1.0f, bottom - y)};
    b.pushClip(body);
    if (m_view.body.quest != nullptr)
    {
        std::vector<BodyHit> hits;
        m_bodyH = drawQuestBody(ctx, m_resources, m_inventory, m_view.body, body, m_scroll, hits);
        for (const BodyHit& h : hits)
            m_hits.push_back({h.rect, 0, h.item});
    }
    else
    {
        render::TextStyle es = shadow;
        es.color = t.textDim;
        m_bodyH = paragraph(ctx, ctx.font(13.0f),
                            m_view.rows.empty() ? world::kQuestLogEmpty : std::string_view("Bir görev seç."), body.x,
                            body.y, body.w, es);
    }
    b.popClip();
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, m_bodyH - body.h));
}

void QuestLogWindow::draw(ui::DrawContext& ctx)
{
    if (!isVisible())
        return;
    ui::Panel::draw(ctx);
    if (context() == nullptr || context()->hasModal())
        return;
    if (!isHovered())
        return;
    const float mx = m_mouse.x;
    const float my = m_mouse.y;
    for (const Hit& h : m_hits)
        if (h.item != 0 && h.rect.contains(mx, my))
        {
            drawRewardTooltip(ctx, m_inventory, h.item, h.rect, *context());
            break;
        }
}

bool QuestLogWindow::onMouseDown(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return true;
    for (const Hit& h : m_hits)
        if (h.quest != 0 && h.rect.contains(static_cast<float>(e.x), static_cast<float>(e.y)))
        {
            if (m_hooks.select)
                m_hooks.select(h.quest);
            break;
        }
    return true;
}

bool QuestLogWindow::onMouseMove(const platform::InputEvent& e)
{
    m_mouse = {static_cast<float>(e.x), static_cast<float>(e.y)};
    return true;
}

bool QuestLogWindow::onMouseWheel(const platform::InputEvent& e)
{
    if (context() != nullptr && e.wheel != 0.0f)
        m_scroll = std::max(0.0f, m_scroll - e.wheel * 40.0f * context()->scale());
    return true;
}

// ---------------------------------------------------------------------------------------------
// QuestTracker
// ---------------------------------------------------------------------------------------------

namespace {
constexpr float kTrackerName = 18.0f; // logical line heights (the tracker's own fonts)
constexpr float kTrackerLine = 16.0f;
constexpr float kTrackerGap = 6.0f;
} // namespace

QuestTracker::QuestTracker()
{
    setName("quest.tracker");
    setMouseTransparent(true);
}

void QuestTracker::setEntries(std::vector<Entry> entries)
{
    m_entries = std::move(entries);
}

float QuestTracker::contentHeight() const noexcept
{
    float h = 0.0f;
    for (const Entry& e : m_entries)
        h += kTrackerName + kTrackerLine * static_cast<float>(e.ready ? 1 : e.lines.size()) + kTrackerGap;
    return h > 0.0f ? h + 8.0f : 0.0f;
}

std::string QuestTracker::plainText() const
{
    std::string out;
    for (const Entry& e : m_entries)
    {
        if (e.ready)
            out += std::format("{}{}: {}", out.empty() ? "" : "\n", e.name, e.turnIn);
        for (const world::ObjectiveLine& l : e.lines)
            out += std::format("{}{}: {}{}", out.empty() ? "" : "\n", e.name, l.text, l.done ? " (tamam)" : "");
    }
    return out;
}

void QuestTracker::drawSelf(ui::DrawContext& ctx)
{
    if (m_entries.empty())
        return;
    auto& b = ctx.batch;
    const Rect r = rect();
    // A soft dark plate behind the lines (readable over bright ground), right aligned under the zone name.
    b.fillRoundedRect(r, ctx.pxf(6.0f), Color::hex(0x07090C, 0.42f));
    const render::Font& nf = ctx.font(13.0f, render::FontWeight::Bold);
    const render::Font& lf = ctx.font(12.5f);
    render::TextStyle s;
    s.outline = true;
    s.outlineColor = Color::hex(0x000000, 0.8f);
    float y = r.y + ctx.px(4.0f);
    const float x = r.x + ctx.px(8.0f);
    const float w = r.w - ctx.px(14.0f);
    for (const Entry& e : m_entries)
    {
        s.color = e.ready ? Color::hex(world::kQuestReadyRgb) : Color::hex(0xF2CA64);
        b.fillDiamond({x + ctx.px(4.0f), y + ctx.px(kTrackerName) * 0.5f}, ctx.px(4.0f),
                      e.ready ? Color::hex(world::kQuestReadyRgb) : Color::hex(world::kQuestAvailableRgb));
        render::drawTextAligned(b, nf, e.name, {x + ctx.px(12.0f), y, w - ctx.px(12.0f), ctx.px(kTrackerName)},
                                render::HAlign::Left, render::VAlign::Middle, s);
        y += ctx.px(kTrackerName);
        if (e.ready)
        {
            s.color = Color::hex(world::kQuestReadyRgb);
            render::drawTextAligned(b, lf, e.turnIn, {x + ctx.px(14.0f), y, w - ctx.px(14.0f), ctx.px(kTrackerLine)},
                                    render::HAlign::Left, render::VAlign::Middle, s);
            y += ctx.px(kTrackerLine);
        }
        else
            for (const world::ObjectiveLine& l : e.lines)
            {
                s.color = l.done ? Color::hex(0x9BE07A) : Color::hex(0xE4DDCB);
                render::drawTextAligned(b, lf, l.text,
                                        {x + ctx.px(14.0f), y, w - ctx.px(14.0f), ctx.px(kTrackerLine)}, render::HAlign::Left,
                                        render::VAlign::Middle, s);
                y += ctx.px(kTrackerLine);
            }
        y += ctx.px(kTrackerGap);
    }
}

} // namespace vo::client::scenes
