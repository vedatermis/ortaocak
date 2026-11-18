#include "client/scenes/SkillWindow.h"

#include "client/render/TextRenderer.h"
#include "client/render/Texture2D.h"
#include "client/scenes/KunyeWindow.h"
#include "client/ui/DrawContext.h"
#include "client/ui/TextLayout.h"
#include "client/ui/UiContext.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;

namespace {

// Layout (logical units, relative to the content rect).
constexpr float kPadX = 14.0f;
constexpr float kPadTop = 8.0f;
constexpr float kPadBottom = 12.0f;
constexpr float kContentW = SkillWindow::kWidth - 2.0f * kPadX; // 628
constexpr float kColW = 306.0f;
constexpr float kColX[2] = {0.0f, kContentW - kColW};
constexpr float kPlaqueY = 28.0f;
constexpr float kPlaqueH = 40.0f;
constexpr float kPlaqueW = kPlaqueH * 4.0f; // the plaque art is 256 x 64
constexpr float kRowsY = 76.0f;
constexpr float kCellW = 150.0f;
constexpr float kCellH = 52.0f;
constexpr float kRowGap = 24.0f; // the connectors, and the lock note of a locked tier
constexpr float kIcon = 38.0f;
constexpr float kPlus = 20.0f;
constexpr float kSpentY = kRowsY + 3.0f * kCellH + 2.0f * kRowGap + 6.0f; // 286
constexpr float kBarSlot = 34.0f;
constexpr float kBarGap = 5.0f;
constexpr float kBarY = kSpentY + 24.0f;    // 310
constexpr float kMessageY = kBarY + kBarSlot + 4.0f; // 348
constexpr float kDragThreshold = 5.0f;
constexpr double kMessageSeconds = 5.0;

const Color kGold = Color::hex(0xF5C84C);
const Color kRed = Color::hex(0xE5736A);
const Color kText = Color::hex(0xE9E2CF);
const Color kDim = Color::hex(0xA39B88);
const Color kTitle = Color::hex(0xF2CA64);

float rowY(std::uint8_t tier)
{
    return kRowsY + static_cast<float>(std::clamp<int>(tier, 1, 3) - 1) * (kCellH + kRowGap);
}

// A Label that is never drawn: carries text for automation (expecttext).
class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

// The band over a locked tier row: dark, a padlock, "Bu yolda N puan gerekli".
class GateLabel final : public ui::Label
{
public:
    explicit GateLabel(SkillUiHost& host) : ui::Label(std::string(), ui::TextRole::Small), m_host(host)
    {
        setMouseTransparent(true); // hovering the greyed cells under it still shows their tooltips
    }
    void drawSelf(ui::DrawContext& ctx) override
    {
        const Rect r = rect();
        auto& b = ctx.batch;
        const render::Font& f = ctx.font(12.0f, render::FontWeight::Bold);
        const float tw = render::measureText(f, text());
        const float lock = std::round(r.h * 0.8f);
        const float pillW = std::round(tw + lock + ctx.pxf(6.0f) + ctx.pxf(16.0f));
        const Rect pill = Rect{std::round(r.centerX() - pillW * 0.5f), r.y, pillW, r.h}.snapped();
        b.fillRoundedRect(pill, r.h * 0.5f, Color::hex(0x07090C, 0.88f));
        b.strokeRoundedRect(pill, r.h * 0.5f, std::max(1.0f, ctx.pxf(1.0f)), Color::hex(0x7A6236, 0.75f));
        const float x0 = std::round(r.centerX() - (tw + lock + ctx.pxf(6.0f)) * 0.5f);
        if (const render::Texture2D* l = m_host.skillImage("ui/skillbar/lock_icon.png"))
            b.drawImage(*l, Rect{x0, std::round(r.centerY() - lock * 0.5f), lock, lock}.snapped());
        render::TextStyle s;
        s.color = Color::hex(0xE8C77A);
        s.shadow = true;
        render::drawTextAligned(b, f, text(), {x0 + lock + ctx.pxf(6.0f), r.y, tw + 2.0f, r.h}, render::HAlign::Left,
                                render::VAlign::Middle, s);
    }

private:
    SkillUiHost& m_host;
};

} // namespace

// ---- Skill name fitting (fix1) ----

float SkillCellWidget::skillNameFontSize(render::FontLibrary& fonts, const ui::Theme& theme, float scale,
                                         std::string_view name, float widthPx)
{
    float size = theme.fontSize(ui::TextRole::Small);
    while (size > kMinNameFont &&
           render::measureText(fonts.get(render::FontWeight::Bold, size * scale), name) > widthPx)
        size -= 0.5f;
    return std::max(size, kMinNameFont);
}

bool SkillCellWidget::checkAllNamesFit(render::FontLibrary& fonts, const ui::Theme& theme, float scale,
                                       std::string& error)
{
    // The same pixel arithmetic as drawSelf (DrawContext::px).
    const auto px = [scale](float v) { return std::max(1.0f, std::round(v * scale)); };
    const float cellW = px(kCellW);
    const float cellH = px(kCellH);
    const float tw = cellW - px(3.0f) - px(kIcon) - px(6.0f) - px(3.0f);
    const render::Font& rf = fonts.get(render::FontWeight::Regular, 12.0f * scale);
    std::size_t checked = 0;
    for (const game::SkillDef& s : game::skills().skills())
    {
        const float size = skillNameFontSize(fonts, theme, scale, s.name, tw);
        const render::Font& nf = fonts.get(render::FontWeight::Bold, size * scale);
        const float w = render::measureText(nf, s.name);
        if (w > tw)
        {
            error = std::format("'{}' is {:.0f} px wide even at {:.1f} (room {:.0f} px, scale {:.3f})", s.name, w, size, tw, scale);
            return false;
        }
        // Name line, rank line and the paddings inside the cell.
        if (nf.lineHeight() + rf.lineHeight() + 2.0f * px(3.0f) > cellH)
        {
            error = std::format("'{}': name and rank lines do not fit the cell height at scale {:.3f}", s.name, scale);
            return false;
        }
        ++checked;
    }
    // The rank line left of the "+" button.
    for (std::uint8_t rank = 0; rank <= game::kMaxSkillRank; ++rank)
        if (render::measureText(rf, world::rankText(rank)) > tw - px(kPlus) - px(3.0f))
        {
            error = std::format("rank line '{}' runs under the + button at scale {:.3f}", world::rankText(rank), scale);
            return false;
        }
    if (checked == 0)
    {
        error = "no skills loaded";
        return false;
    }
    return true;
}

// ---- SkillCellWidget ----

SkillCellWidget::SkillCellWidget(SkillUiHost& host, const game::SkillDef& skill) : m_host(host), m_skill(skill) {}

bool SkillCellWidget::onMouseDown(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return true;
    m_pressed = true;
    m_dragged = false;
    m_pressX = static_cast<float>(e.x);
    m_pressY = static_cast<float>(e.y);
    return true;
}

bool SkillCellWidget::onMouseMove(const platform::InputEvent& e)
{
    if (!m_pressed || !hasCapture())
        return false;
    const auto x = static_cast<float>(e.x);
    const auto y = static_cast<float>(e.y);
    if (!m_dragged && m_host.skillBook().rank(m_skill.id) >= 1 &&
        std::hypot(x - m_pressX, y - m_pressY) > kDragThreshold * layoutScale())
    {
        m_dragged = true;
        m_host.beginSkillDrag(m_skill.id, -1);
    }
    if (m_dragged)
        m_host.moveSkillDrag(x, y);
    return true;
}

bool SkillCellWidget::onMouseUp(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return true;
    m_pressed = false;
    if (m_dragged)
    {
        m_dragged = false;
        m_host.endSkillDrag(static_cast<float>(e.x), static_cast<float>(e.y), true);
    }
    return true;
}

void SkillCellWidget::onCaptureLost()
{
    m_pressed = false;
    if (m_dragged)
    {
        m_dragged = false;
        m_host.endSkillDrag(0.0f, 0.0f, false);
    }
}

void SkillCellWidget::drawSelf(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const world::SkillBook& book = m_host.skillBook();
    const std::uint8_t rank = book.rank(m_skill.id);
    const bool gate = book.gateMet(m_skill);
    const bool hovered = isHovered() && !m_host.skillDragActive();
    const float radius = ctx.pxf(5.0f);
    Color bg = Color::hex(0x0C0F13, 0.72f);
    Color edge = t.insetBorder;
    if (rank >= 1)
    {
        bg = Color::hex(0x2A2214, 0.9f);
        edge = Color::hex(0xC9A247, 0.85f);
    }
    if (hovered)
        edge = t.buttonBorderHover;
    b.fillRoundedRect(r, radius, bg);
    b.strokeRoundedRect(r, radius, std::max(1.0f, ctx.pxf(1.0f)), edge);

    const Rect icon = Rect{r.x + ctx.px(3.0f), r.y + ctx.px(3.0f), ctx.px(kIcon), ctx.px(kIcon)}.snapped();
    Color tint = colors::kWhite;
    if (!gate)
        tint = Color{0.36f, 0.36f, 0.38f, 1.0f}; // greyed: the tier is locked
    else if (rank == 0)
        tint = Color{0.6f, 0.6f, 0.62f, 1.0f}; // dimmed: not learned
    if (const render::Texture2D* img = m_host.skillImage(world::skillIconPath(m_skill)))
        b.drawImage(*img, icon, tint);
    if (const render::Texture2D* pips = m_host.skillImage(std::format("ui/skillbar/rank_pips_{}.png", rank)))
    {
        // Under the icon (the art's 64 x 16 strip, three diamonds, `rank` of them lit).
        const float pw = std::round(icon.w + ctx.pxf(6.0f));
        const float ph = std::round(pw / 4.0f);
        b.drawImage(*pips, Rect{std::round(icon.centerX() - pw * 0.5f), icon.bottom() + ctx.pxf(1.0f), pw, ph}.snapped(),
                    gate ? colors::kWhite : Color{0.5f, 0.5f, 0.5f, 1.0f});
    }

    const float tx = icon.right() + ctx.px(6.0f);
    const float tw = r.right() - tx - ctx.px(3.0f);
    // fix1: the name on one line, shrunk to fit (a wrapped second line ran into the rank line).
    const float nameSize = skillNameFontSize(ctx.fonts, t, ctx.scale, m_skill.name, tw);
    const render::Font& nf = ctx.fonts.get(render::FontWeight::Bold, nameSize * ctx.scale);
    std::vector<ui::TextLine> lines;
    ui::wrapText(nf, m_skill.name, measureText(nf, m_skill.name) <= tw ? 0.0f : tw, lines);
    if (lines.size() > 2)
        lines.resize(2);
    render::TextStyle ns;
    ns.color = rank >= 1 ? t.textTitle : (gate ? t.text : t.textDisabled);
    ns.shadow = true;
    ns.shadowColor = t.textShadow;
    render::drawTextLines(b, nf, m_skill.name, lines,
                          {tx, r.y + ctx.px(3.0f), tw, nf.lineHeight() * static_cast<float>(lines.size())},
                          render::HAlign::Left, render::VAlign::Top, ns);
    const render::Font& rf = ctx.font(12.0f);
    render::TextStyle rs;
    rs.color = rank >= 1 ? t.text : t.textDim;
    rs.shadow = true;
    render::drawTextAligned(b, rf, world::rankText(rank), {tx, r.bottom() - rf.lineHeight() - ctx.px(3.0f), tw, rf.lineHeight()},
                            render::HAlign::Left, render::VAlign::Top, rs);
}

// ---- SkillWindow ----

SkillWindow::SkillWindow(SkillUiHost& host, std::uint8_t charClass)
    : ui::Panel(ui::Panel::Style::Window, "Yetenekler"), m_host(host), m_class(charClass)
{
    setName("skills.window");
    setPadding({kPadX, kPadTop, kPadX, kPadBottom});
    setSize(kWidth, kHeight);

    m_points = &add<ui::Label>("", ui::TextRole::Large);
    m_points->setWeight(render::FontWeight::Bold).setColor(kDim).setShadow(true);
    m_points->setAnchors(anchor::kTopLeft).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(280.0f, 24.0f);
    m_points->setName("skills.points");
    m_classLine = &add<ui::Label>("", ui::TextRole::Small);
    m_classLine->setColor(kDim).setShadow(true).setAlign(render::HAlign::Right, render::VAlign::Middle);
    m_classLine->setAnchors(anchor::kTopLeft).setMargins({kContentW - 330.0f, 2.0f, 0.0f, 0.0f}).setSize(330.0f, 20.0f);
    m_classLine->setName("skills.class");

    const game::SkillCatalog& catalog = game::skills();
    const world::SkillBook& book = host.skillBook();
    for (std::uint8_t br = 0; br < 2; ++br)
    {
        // Branch name over its plaque (the plaque itself is drawn in drawSelf).
        const float plaqueX = kColX[br] + (kColW - kPlaqueW) * 0.5f;
        auto& name = add<ui::Label>(std::string(catalog.branchName(charClass, br)), ui::TextRole::Large);
        name.setWeight(render::FontWeight::Bold).setColor(kTitle).setShadow(true).setAlign(render::HAlign::Left,
                                                                                           render::VAlign::Middle);
        name.setAnchors(anchor::kTopLeft)
            .setMargins({plaqueX + kPlaqueW * (70.0f / 256.0f), kPlaqueY, 0.0f, 0.0f})
            .setSize(kPlaqueW * (180.0f / 256.0f), kPlaqueH);
        name.setName(std::format("skills.branch.{}", br));

        std::array<std::vector<const game::SkillDef*>, 3> tiers;
        for (const game::SkillDef* s : book.branchSkills(br))
            tiers[std::clamp<int>(s->tier, 1, 3) - 1].push_back(s);
        for (std::uint8_t tier = 1; tier <= 3; ++tier)
        {
            const auto& list = tiers[tier - 1u];
            const float y = rowY(tier);
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                const game::SkillDef& s = *list[i];
                float x = kColX[br] + (kColW - kCellW) * 0.5f;
                if (list.size() >= 2)
                    x = kColX[br] + static_cast<float>(i) * (kColW - kCellW) / static_cast<float>(list.size() - 1);
                Cell c;
                c.skill = &s;
                c.logical = {x, y, kCellW, kCellH};
                auto& cell = add<SkillCellWidget>(host, s);
                cell.setAnchors(anchor::kTopLeft).setMargins({x, y, 0.0f, 0.0f}).setSize(kCellW, kCellH);
                cell.setName("skills.cell." + s.key);
                c.widget = &cell;
                auto& plus = add<StatStepButton>(true, [this, id = s.id](bool) { m_host.raiseSkill(id); }, []() {});
                plus.setAnchors(anchor::kTopLeft)
                    .setMargins({x + kCellW - kPlus - 3.0f, y + kCellH - kPlus - 3.0f, 0.0f, 0.0f})
                    .setSize(kPlus, kPlus);
                plus.setName("skills.plus." + s.key);
                c.plus = &plus;
                auto rank = std::make_unique<TextProbe>();
                rank->setName("skills.rank." + s.key);
                rank->setSize(0.0f, 0.0f);
                c.rank = rank.get();
                addChild(std::move(rank));
                m_cells.push_back(c);
            }
            if (tier >= 2 && !list.empty())
            {
                auto& gate = add<GateLabel>(host);
                // In the gap above the locked row, on the path between the tiers.
                gate.setAnchors(anchor::kTopLeft)
                    .setMargins({kColX[br], y - kRowGap + (kRowGap - 18.0f) * 0.5f, 0.0f, 0.0f})
                    .setSize(kColW, 18.0f);
                gate.setName(std::format("skills.gate.{}.{}", br, tier));
                gate.setText(world::tierGateText(catalog.rules().tierPoints[tier - 1u]));
                m_gates.push_back({&gate, br, tier});
            }
        }
        m_spent[br] = &add<ui::Label>("", ui::TextRole::Small);
        m_spent[br]->setColor(kText).setShadow(true).setAlign(render::HAlign::Center, render::VAlign::Middle);
        m_spent[br]->setAnchors(anchor::kTopLeft).setMargins({kColX[br], kSpentY, 0.0f, 0.0f}).setSize(kColW, 18.0f);
        m_spent[br]->setName(std::format("skills.spent.{}", br));
    }

    // The bar again, under the tree.
    const float barW = SkillSlotsWidget::widthFor(kBarSlot, kBarGap);
    const float barX = kContentW - barW - 4.0f;
    auto& barLabel = add<ui::Label>("Yetenek çubuğu", ui::TextRole::Small);
    barLabel.setWeight(render::FontWeight::Bold).setColor(Color::hex(0xD9AE4A)).setShadow(true);
    barLabel.setAlign(render::HAlign::Right, render::VAlign::Middle);
    barLabel.setAnchors(anchor::kTopLeft).setMargins({0.0f, kBarY, 0.0f, 0.0f}).setSize(barX - 12.0f, kBarSlot);
    m_bar = &add<SkillSlotsWidget>(host, "skills.bar", kBarSlot, kBarGap, false);
    m_bar->setAnchors(anchor::kTopLeft).setMargins({barX, kBarY, 0.0f, 0.0f});

    m_message = &add<ui::Label>("", ui::TextRole::Small);
    m_message->setShadow(true).setAlign(render::HAlign::Center, render::VAlign::Middle);
    m_message->setAnchors(anchor::kTopLeft).setMargins({0.0f, kMessageY, 0.0f, 0.0f}).setSize(kContentW, 18.0f);
    m_message->setName("skills.message");

    auto& close = add<ui::Button>("X", [this]() { m_host.closeSkillWindow(); });
    close.setFocusable(false);
    close.setAnchors(anchor::kTopRight).setMargins({0.0f, -34.0f, -6.0f, 0.0f}).setSize(28.0f, 28.0f);
    close.setName("skills.close");
}

void SkillWindow::setClassLine(std::string text)
{
    m_classLine->setText(text);
}

void SkillWindow::setMessage(std::string text, bool error)
{
    m_message->setText(text);
    m_message->setColor(error ? kRed : kText);
    m_messageTime = context() != nullptr ? context()->time() : 0.0;
}

void SkillWindow::update(double dt)
{
    const world::SkillBook& book = m_host.skillBook();
    m_points->setText(world::skillPointsText(book.unspent()));
    m_points->setColor(book.unspent() > 0 ? kGold : kDim);
    for (std::uint8_t br = 0; br < 2; ++br)
        m_spent[br]->setText(world::branchSpentText(book.branchSpent(br)));
    for (const Cell& c : m_cells)
    {
        const vo::net::SkillLearnResult r = book.check(c.skill->id);
        // "+" while the skill can be raised; disabled (not hidden) while a request is in flight.
        const bool show = book.known() && r == vo::net::SkillLearnResult::Ok;
        if (c.plus->isVisible() != show)
            c.plus->setVisible(show);
        c.plus->setEnabled(!book.learnBusy());
        c.rank->setText(world::rankText(book.rank(c.skill->id)));
    }
    for (const Gate& g : m_gates)
    {
        const std::uint16_t need = game::skills().rules().tierPoints[g.tier - 1u];
        const bool locked = book.branchSpent(g.branch) < need;
        if (g.label->isVisible() != locked)
            g.label->setVisible(locked);
    }
    if (m_messageTime >= 0.0 && context() != nullptr && context()->time() - m_messageTime > kMessageSeconds)
    {
        m_message->setText("");
        m_messageTime = -1.0;
    }
    ui::Panel::update(dt);
}

bool SkillWindow::onMouseWheel(const platform::InputEvent&)
{
    return true; // the wheel over the window does not zoom the camera
}

bool SkillWindow::onMouseDown(const platform::InputEvent&)
{
    return true; // a click on the frame does not walk the character there
}

void SkillWindow::drawSelf(ui::DrawContext& ctx)
{
    // An opaque ground under the (translucent) window skin: the F3 info panel and the HUD no longer
    // show through the skill tree (M12 defect, fixed in M13).
    ctx.batch.fillRoundedRect(rect().inset(ctx.px(5.0f)), ctx.px(6.0f), Color::hex(0x10141A));
    ui::Panel::drawSelf(ctx);
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect c = contentRect();
    const float line = std::max(1.0f, ctx.pxf(1.0f));
    const world::SkillBook& book = m_host.skillBook();
    const auto L = [&](float x, float y, float w, float h) {
        return Rect{c.x + ctx.px(x), c.y + ctx.px(y), ctx.px(w), ctx.px(h)};
    };
    // Gold rule under the header, a divider between the branches, a rule over the bar row.
    const float ruleY = std::round(c.y + ctx.px(25.0f)) + 0.5f;
    b.drawLine({c.x, ruleY}, {c.right(), ruleY}, line, t.ornament.withAlpha(0.45f));
    const float divX = std::round(c.x + ctx.px(kContentW * 0.5f)) + 0.5f;
    b.drawLine({divX, c.y + ctx.px(kPlaqueY)}, {divX, c.y + ctx.px(kSpentY + 18.0f)}, line, t.insetBorder.withAlpha(0.7f));
    const float barRule = std::round(c.y + ctx.px(kBarY - 5.0f)) + 0.5f;
    b.drawLine({c.x, barRule}, {c.right(), barRule}, line, t.ornament.withAlpha(0.25f));

    for (std::uint8_t br = 0; br < 2; ++br)
    {
        // Branch plaque (medallion + name plate; the name is the Label on top).
        const Rect plaque = L(kColX[br] + (kColW - kPlaqueW) * 0.5f, kPlaqueY, kPlaqueW, kPlaqueH);
        if (const render::Texture2D* p =
                m_host.skillImage(std::format("ui/skillbar/branch_{}.png", world::branchArtKey(m_class, br))))
            b.drawImage(*p, plaque.snapped());
        else
            b.fillRoundedRect(plaque, ctx.pxf(6.0f), t.panelTop);

        // Connectors: tier k -> tier k+1, lit once the lower tier's gate is met.
        for (std::uint8_t tier = 1; tier < 3; ++tier)
        {
            std::vector<const Cell*> upper;
            std::vector<const Cell*> lower;
            for (const Cell& cell : m_cells)
            {
                if (cell.skill->branch != br)
                    continue;
                if (cell.skill->tier == tier)
                    upper.push_back(&cell);
                else if (cell.skill->tier == tier + 1)
                    lower.push_back(&cell);
            }
            if (upper.empty() || lower.empty())
                continue;
            const std::uint16_t need = game::skills().rules().tierPoints[tier];
            const bool lit = book.branchSpent(br) >= need;
            const render::Texture2D* v = m_host.skillImage(lit ? "ui/skillbar/tree_v_lit.png" : "ui/skillbar/tree_v_unlit.png");
            const render::Texture2D* h = m_host.skillImage(lit ? "ui/skillbar/tree_h_lit.png" : "ui/skillbar/tree_h_unlit.png");
            const float thick = std::max(6.0f, std::round(ctx.pxf(10.0f)));
            const Color fallback = lit ? Color::hex(0xD9AE4A) : Color::hex(0x4A4234);
            const auto vertical = [&](float x, float y0, float y1) {
                const Rect seg{std::round(x - thick * 0.5f), std::round(y0), thick, std::round(y1 - y0)};
                if (seg.h <= 0.0f)
                    return;
                if (v != nullptr)
                    b.drawImage(*v, seg);
                else
                    b.fillRect({std::round(x - 1.0f), seg.y, 2.0f, seg.h}, fallback);
            };
            const auto horizontal = [&](float x0, float x1, float y) {
                const Rect seg{std::round(x0), std::round(y - thick * 0.5f), std::round(x1 - x0), thick};
                if (seg.w <= 0.0f)
                    return;
                if (h != nullptr)
                    b.drawImage(*h, seg);
                else
                    b.fillRect({seg.x, std::round(y - 1.0f), seg.w, 2.0f}, fallback);
            };
            const float upperBottom = c.y + ctx.px(rowY(tier) + kCellH);
            const float lowerTop = c.y + ctx.px(rowY(static_cast<std::uint8_t>(tier + 1)));
            const float mid = std::round((upperBottom + lowerTop) * 0.5f);
            float minX = 1e9f;
            float maxX = -1e9f;
            for (const Cell* u : upper)
            {
                const float x = c.x + ctx.px(u->logical.x + u->logical.w * 0.5f);
                vertical(x, upperBottom, mid + thick * 0.5f);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
            for (const Cell* l : lower)
            {
                const float x = c.x + ctx.px(l->logical.x + l->logical.w * 0.5f);
                vertical(x, mid - thick * 0.5f, lowerTop);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
            if (maxX > minX)
                horizontal(minX - thick * 0.5f, maxX + thick * 0.5f, mid);
        }
    }
}

} // namespace vo::client::scenes
