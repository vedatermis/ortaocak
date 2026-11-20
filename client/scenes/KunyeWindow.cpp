#include "client/scenes/KunyeWindow.h"

#include "client/render/TextRenderer.h"
#include "client/scenes/ItemTooltip.h"
#include "client/ui/DrawContext.h"
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
constexpr float kLeftW = 276.0f;  // attribute column
constexpr float kRightX = 300.0f; // derived column
constexpr float kRightW = KunyeWindow::kWidth - 2.0f * kPadX - kRightX;
constexpr float kColumnHeadY = 52.0f;
constexpr float kRowsY = 74.0f;
constexpr float kRowH = 30.0f;
constexpr float kRowGap = 4.0f;
constexpr float kStep = 26.0f;
constexpr float kUnspentY = kRowsY + 5.0f * (kRowH + kRowGap);
constexpr float kButtonsY = kUnspentY + 28.0f;
constexpr float kButtonH = 32.0f;
constexpr float kMessageY = kButtonsY + kButtonH + 6.0f;
constexpr float kDerivedRowH = 22.0f; // M18: 12 rows (Temel Vuruş, Erim)
constexpr float kDerivedEndY = kRowsY + 12.0f * kDerivedRowH; // the priest's 12 rows end here
constexpr double kMessageSeconds = 5.0;

const Color kBonus = Color::hex(0x9FD27A);   // item bonus (as the tooltips' "Good" lines)
const Color kItemBonus = Color::hex(0x8CC4F2); // M18: an equipment bonus line in a derived row (as the tooltips' "Bonus")
const Color kPending = Color::hex(0xF5C84C); // pending points / previews
const Color kRed = Color::hex(0xE5736A);
// Theme defaults (the window is built before it has a context).
const Color kTitle = Color::hex(0xF2CA64);
const Color kText = Color::hex(0xE9E2CF);
const Color kDim = Color::hex(0xA39B88);
const Color kAccent = Color::hex(0xD9AE4A);

// A Label that is never drawn: carries text for automation (expecttext).
class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

} // namespace

// ---- StatStepButton ----

StatStepButton::StatStepButton(bool plus, std::function<void(bool)> onClick, std::function<void()> onRightClick)
    : ui::Button(std::string(), {}), m_plus(plus), m_onStep(std::move(onClick)), m_onRight(std::move(onRightClick))
{
    setOnClick([this]() {
        if (m_onStep)
            m_onStep(m_shift);
    });
    setFocusable(false); // Space / Enter keep going to the world
}

bool StatStepButton::onMouseDown(const platform::InputEvent& e)
{
    if (e.button == platform::MouseButton::Right)
    {
        if (m_onRight)
            m_onRight();
        return true;
    }
    return ui::Button::onMouseDown(e);
}

bool StatStepButton::onMouseUp(const platform::InputEvent& e)
{
    m_shift = e.shift();
    return ui::Button::onMouseUp(e);
}

void StatStepButton::drawSelf(ui::DrawContext& ctx)
{
    ui::Button::drawSelf(ctx);
    const Rect r = rect();
    const bool enabled = isEnabled();
    const Color c = !enabled ? ctx.theme.textDisabled : (isHovered() ? ctx.theme.accentBright : ctx.theme.buttonText);
    const float len = std::round(r.w * 0.42f);
    const float thick = std::max(2.0f, std::round(ctx.pxf(2.5f)));
    const float cx = std::round(r.centerX());
    const float cy = std::round(r.centerY());
    ctx.batch.fillRect({cx - std::round(len * 0.5f), cy - std::round(thick * 0.5f), len, thick}, c);
    if (m_plus)
        ctx.batch.fillRect({cx - std::round(thick * 0.5f), cy - std::round(len * 0.5f), thick, len}, c);
}

// ---- AttributeRowWidget ----

AttributeRowWidget::AttributeRowWidget(KunyeWindow& owner, std::size_t index)
    : ui::Label(std::string(), ui::TextRole::Normal), m_owner(owner), m_index(index)
{
    setMouseTransparent(false); // hover: the tooltip; right click: one pending point back
}

bool AttributeRowWidget::onMouseDown(const platform::InputEvent& e)
{
    // Right click on a row removes one pending point (as "−").
    if (e.button == platform::MouseButton::Right)
        m_owner.requestRemove(m_index);
    return true;
}

void AttributeRowWidget::drawSelf(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const world::StatSheet& sheet = m_owner.sheet();
    const std::uint16_t pending = sheet.pending(m_index);
    const float radius = ctx.pxf(4.0f);
    const bool hovered = isHovered();
    b.fillRoundedRect(r, radius,
                      pending > 0 ? Color::hex(0x4A3B1C, 0.85f) : (hovered ? t.rowHover.withAlpha(0.9f) : Color::hex(0x0C0F13, 0.6f)));
    b.strokeRoundedRect(r, radius, std::max(1.0f, ctx.pxf(1.0f)),
                        pending > 0 ? kPending.withAlpha(0.55f) : (hovered ? t.buttonBorderHover : t.insetBorder));

    const auto attribute = static_cast<game::Attribute>(m_index);
    const render::Font& nf = ctx.font(ui::TextRole::Normal, render::FontWeight::Bold);
    render::TextStyle ns;
    ns.color = t.text;
    ns.shadow = true;
    ns.shadowColor = t.textShadow;
    const float pad = ctx.px(16.0f);
    render::drawTextAligned(b, nf, game::attributeDisplayName(attribute), {r.x + pad, r.y, r.w, r.h}, render::HAlign::Left,
                            render::VAlign::Middle, ns);
    // Its class's attack attribute: a small gold diamond before the name.
    if (const game::ClassTable* ct = game::classTable(m_owner.charClass()); ct != nullptr && ct->attackAttribute == attribute)
        b.fillDiamond({r.x + ctx.pxf(8.0f), r.centerY()}, ctx.pxf(3.0f), t.ornament);

    // Value: base, "(+bonus)" in green, "+pending" in gold.
    const std::string base = std::to_string(sheet.base(m_index));
    const std::string bonus = sheet.bonus(m_index) > 0 ? std::format(" (+{})", sheet.bonus(m_index)) : std::string();
    const std::string pend = pending > 0 ? std::format("  +{}", pending) : std::string();
    const render::Font& vf = ctx.font(ui::TextRole::Normal, render::FontWeight::Bold);
    float x = r.x + ctx.px(120.0f);
    const float y = std::round(r.y + (r.h - vf.lineHeight()) * 0.5f);
    render::TextStyle vs = ns;
    vs.color = sheet.known() ? t.textTitle : t.textDim;
    x += render::drawText(b, vf, sheet.known() ? std::string_view(base) : std::string_view("—"), {x, y}, vs);
    if (!bonus.empty())
    {
        vs.color = kBonus;
        x += render::drawText(b, vf, bonus, {x, y}, vs);
    }
    if (!pend.empty())
    {
        vs.color = kPending;
        (void)render::drawText(b, vf, pend, {x, y}, vs);
    }
}

// ---- KunyeWindow ----

KunyeWindow::KunyeWindow(const world::StatSheet& sheet, Hooks hooks)
    : ui::Panel(ui::Panel::Style::Window, "Künye"), m_sheet(sheet), m_hooks(std::move(hooks))
{
    setName("kunye.window");
    setPadding({kPadX, kPadTop, kPadX, kPadBottom});
    setSize(kWidth, kHeight);
    const float contentW = kWidth - 2.0f * kPadX;

    m_header = &add<ui::Label>("", ui::TextRole::Large);
    m_header->setWeight(render::FontWeight::Bold).setColor(kTitle).setShadow(true);
    m_header->setAnchors(anchor::kTopLeft).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(contentW, 24.0f);
    m_header->setName("kunye.header");
    m_class = &add<ui::Label>("", ui::TextRole::Small);
    m_class->setColor(kDim).setShadow(true);
    m_class->setAnchors(anchor::kTopLeft).setMargins({0.0f, 25.0f, 0.0f, 0.0f}).setSize(contentW * 0.6f, 18.0f);
    m_class->setName("kunye.class");
    m_xp = &add<ui::Label>("", ui::TextRole::Small);
    m_xp->setColor(kText).setShadow(true).setAlign(render::HAlign::Right, render::VAlign::Middle);
    m_xp->setAnchors(anchor::kTopLeft).setMargins({contentW * 0.4f, 25.0f, 0.0f, 0.0f}).setSize(contentW * 0.6f, 18.0f);
    m_xp->setName("kunye.xp");

    auto heading = [&](const char* text, float x, float w) {
        auto& l = add<ui::Label>(text, ui::TextRole::Small);
        l.setWeight(render::FontWeight::Bold).setColor(kAccent).setShadow(true);
        l.setAnchors(anchor::kTopLeft).setMargins({x, kColumnHeadY, 0.0f, 0.0f}).setSize(w, 18.0f);
    };
    heading("Nitelikler", 0.0f, kLeftW);
    heading("Değerler", kRightX, kRightW);

    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
    {
        const float y = kRowsY + static_cast<float>(i) * (kRowH + kRowGap);
        const std::string key(game::attributeKey(static_cast<game::Attribute>(i)));
        auto& row = add<AttributeRowWidget>(*this, i);
        row.setAnchors(anchor::kTopLeft).setMargins({0.0f, y, 0.0f, 0.0f}).setSize(kLeftW - 2.0f * kStep - 10.0f, kRowH);
        row.setName("kunye.attr." + key);
        m_rows[i] = &row;
        auto& minus = add<StatStepButton>(false, [this, i](bool) { requestRemove(i); }, [this, i]() { requestRemove(i); });
        minus.setAnchors(anchor::kTopLeft)
            .setMargins({kLeftW - 2.0f * kStep - 4.0f, y + (kRowH - kStep) * 0.5f, 0.0f, 0.0f})
            .setSize(kStep, kStep);
        minus.setName("kunye.minus." + key);
        m_minus[i] = &minus;
        auto& plus = add<StatStepButton>(true, [this, i](bool shift) { requestAdd(i, shift); },
                                         [this, i]() { requestRemove(i); });
        plus.setAnchors(anchor::kTopLeft)
            .setMargins({kLeftW - kStep, y + (kRowH - kStep) * 0.5f, 0.0f, 0.0f})
            .setSize(kStep, kStep);
        plus.setName("kunye.plus." + key);
        m_plus[i] = &plus;
    }

    m_unspent = &add<ui::Label>("", ui::TextRole::Normal);
    m_unspent->setShadow(true).setAlign(render::HAlign::Left, render::VAlign::Middle);
    m_unspent->setAnchors(anchor::kTopLeft).setMargins({2.0f, kUnspentY, 0.0f, 0.0f}).setSize(kLeftW, 22.0f);
    m_unspent->setName("kunye.unspent");

    const float buttonW = (kLeftW - 8.0f) * 0.5f;
    m_confirm = &add<ui::Button>("Onayla", [this]() {
        if (m_hooks.confirm)
            m_hooks.confirm();
    });
    m_confirm->setFocusable(false);
    m_confirm->setAnchors(anchor::kTopLeft).setMargins({0.0f, kButtonsY, 0.0f, 0.0f}).setSize(buttonW, kButtonH);
    m_confirm->setName("kunye.confirm");
    m_cancel = &add<ui::Button>("Vazgeç", [this]() {
        if (m_hooks.cancel)
            m_hooks.cancel();
    });
    m_cancel->setFocusable(false);
    m_cancel->setAnchors(anchor::kTopLeft).setMargins({buttonW + 8.0f, kButtonsY, 0.0f, 0.0f}).setSize(buttonW, kButtonH);
    m_cancel->setName("kunye.cancel");

    m_message = &add<ui::Label>("", ui::TextRole::Small);
    m_message->setShadow(true).setAlign(render::HAlign::Center, render::VAlign::Middle);
    // M18: under the buttons only (the derived rows reach further down on the right).
    m_message->setAnchors(anchor::kTopLeft).setMargins({0.0f, kMessageY, 0.0f, 0.0f}).setSize(kLeftW, 18.0f);
    m_message->setName("kunye.message");

    auto& close = add<ui::Button>("X", [this]() {
        if (m_hooks.close)
            m_hooks.close();
    });
    close.setFocusable(false);
    close.setAnchors(anchor::kTopRight).setMargins({0.0f, -34.0f, -6.0f, 0.0f}).setSize(28.0f, 28.0f);
    close.setName("kunye.close");

    auto derived = std::make_unique<TextProbe>();
    derived->setName("kunye.derived");
    derived->setSize(0.0f, 0.0f);
    m_derivedProbe = derived.get();
    addChild(std::move(derived));
    auto tip = std::make_unique<TextProbe>();
    tip->setName("kunye.tooltip");
    tip->setSize(0.0f, 0.0f);
    m_tooltipProbe = tip.get();
    addChild(std::move(tip));
    refresh(m_info);
}

void KunyeWindow::refresh(const Info& info)
{
    m_info = info;
    m_header->setText(info.name);
    m_class->setText(info.classLine);
    m_xp->setText(info.xpToNext > 0 ? std::format("Deneyim: {} / {}", info.xp, info.xpToNext)
                                    : std::string("Deneyim: en yüksek seviye"));
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        m_rows[i]->setText(world::attributeRowText(static_cast<game::Attribute>(i), m_sheet.base(i), m_sheet.bonus(i),
                                                   m_sheet.pending(i)));
    std::string unspent = world::unspentText(m_sheet.unspent());
    if (m_sheet.hasPending())
        unspent += std::format("  ·  seçilen: {}", m_sheet.pendingTotal());
    m_unspent->setText(unspent);
    m_unspent->setColor(m_sheet.unspent() > 0 ? kPending : kDim);
    m_unspent->setWeight(m_sheet.unspent() > 0 ? render::FontWeight::Bold : render::FontWeight::Regular);

    m_derived.clear();
    if (m_sheet.known())
    {
        if (m_sheet.hasPending())
        {
            const vo::net::CharStats preview =
                world::toCharStats(m_sheet.preview(info.charClass, info.equip), m_sheet.level());
            m_derived = world::derivedRows(info.charClass, m_sheet.stats(), &preview);
        }
        else
            m_derived = world::derivedRows(info.charClass, m_sheet.stats(), nullptr);
    }
    m_derivedProbe->setText(world::derivedRowsText(m_derived));
    updateEnabled();
}

void KunyeWindow::setMessage(std::string text, bool error)
{
    m_message->setText(text);
    m_message->setColor(error ? kRed : Color::hex(0xE9E2CF));
    m_messageTime = context() != nullptr ? context()->time() : 0.0;
}

void KunyeWindow::requestAdd(std::size_t i, bool shift)
{
    if (m_hooks.add)
        m_hooks.add(i, shift);
}

void KunyeWindow::requestRemove(std::size_t i)
{
    if (m_hooks.remove)
        m_hooks.remove(i);
}

void KunyeWindow::updateEnabled()
{
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
    {
        m_plus[i]->setEnabled(m_sheet.canAdd(i));
        m_minus[i]->setEnabled(m_sheet.canRemove(i));
        m_minus[i]->setVisible(m_sheet.pending(i) > 0);
    }
    const bool usable = m_sheet.hasPending() && !m_sheet.busy();
    m_confirm->setEnabled(usable);
    m_cancel->setEnabled(usable);
}

void KunyeWindow::update(double dt)
{
    ui::Panel::update(dt);
    updateEnabled();
    if (m_messageTime >= 0.0 && context() != nullptr && context()->time() - m_messageTime > kMessageSeconds)
    {
        m_message->setText("");
        m_messageTime = -1.0;
    }
    const AttributeRowWidget* row = hoveredRow();
    m_tooltipProbe->setText(row != nullptr
                                ? world::attributeTooltip(static_cast<game::Attribute>(row->index()), m_info.charClass)
                                : std::string());
}

bool KunyeWindow::onMouseWheel(const platform::InputEvent&)
{
    return true; // the wheel over the window does not zoom the camera
}

bool KunyeWindow::onMouseDown(const platform::InputEvent&)
{
    return true; // a click on the frame does not walk the character there
}

const AttributeRowWidget* KunyeWindow::hoveredRow() const
{
    if (context() == nullptr)
        return nullptr;
    for (const AttributeRowWidget* r : m_rows)
        if (context()->hover() == r)
            return r;
    return nullptr;
}

void KunyeWindow::drawSelf(ui::DrawContext& ctx)
{
    ui::Panel::drawSelf(ctx);
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect c = contentRect();
    const float line = std::max(1.0f, ctx.pxf(1.0f));
    // Gold rule under the header, thin rules under the column headings, a divider between columns.
    const float ruleY = std::round(c.y + ctx.px(47.0f)) + 0.5f;
    b.drawLine({c.x, ruleY}, {c.right(), ruleY}, line, t.ornament.withAlpha(0.45f));
    const float headY = std::round(c.y + ctx.px(kColumnHeadY + 19.0f)) + 0.5f;
    b.drawLine({c.x, headY}, {c.x + ctx.px(kLeftW), headY}, line, t.ornament.withAlpha(0.2f));
    b.drawLine({c.x + ctx.px(kRightX), headY}, {c.x + ctx.px(kRightX + kRightW), headY}, line, t.ornament.withAlpha(0.2f));
    const float divX = std::round(c.x + ctx.px((kLeftW + kRightX) * 0.5f)) + 0.5f;
    b.drawLine({divX, c.y + ctx.px(kColumnHeadY)}, {divX, c.y + ctx.px(kDerivedEndY)}, line, t.insetBorder.withAlpha(0.7f));

    // Derived values: label left, value right ("36 → 41" with the preview in gold).
    const render::Font& lf = ctx.font(ui::TextRole::Small);
    const render::Font& vf = ctx.font(ui::TextRole::Small, render::FontWeight::Bold);
    render::TextStyle ls;
    ls.color = t.textDim;
    ls.shadow = true;
    ls.shadowColor = t.textShadow;
    render::TextStyle vs = ls;
    vs.color = t.text;
    render::TextStyle ps = ls;
    ps.color = kPending;
    const float x0 = c.x + ctx.px(kRightX);
    const float w = ctx.px(kRightW);
    if (m_derived.empty())
    {
        render::drawTextAligned(b, lf, "Değerler bekleniyor…", {x0, c.y + ctx.px(kRowsY), w, ctx.px(kDerivedRowH)},
                                render::HAlign::Left, render::VAlign::Middle, ls);
        return;
    }
    for (std::size_t j = 0; j < m_derived.size(); ++j)
    {
        const world::DerivedRow& row = m_derived[j];
        const Rect r{x0, std::round(c.y + ctx.px(kRowsY + static_cast<float>(j) * kDerivedRowH)), w, ctx.px(kDerivedRowH)};
        if (j % 2 == 0)
            b.fillRect(r, Color::hex(0xFFFFFF, 0.025f));
        const Rect inner{r.x + ctx.px(6.0f), r.y, r.w - ctx.px(12.0f), r.h};
        render::drawTextAligned(b, lf, row.label, inner, render::HAlign::Left, render::VAlign::Middle, ls);
        float right = 0.0f; // width used from the right edge
        if (!row.preview.empty())
        {
            const std::string arrow = "  →  " + row.preview;
            right = render::measureText(vf, arrow);
            render::drawTextAligned(b, vf, arrow, inner, render::HAlign::Right, render::VAlign::Middle, ps);
        }
        render::drawTextAligned(b, vf, row.value, {inner.x, inner.y, inner.w - right, inner.h}, render::HAlign::Right,
                                render::VAlign::Middle, vs);
        // M18: the equipment's share left of the value, in the bonus colour ("(+40)").
        if (!row.bonus.empty())
        {
            right += render::measureText(vf, row.value) + ctx.px(5.0f);
            render::TextStyle bs = ls;
            bs.color = kItemBonus;
            render::drawTextAligned(b, lf, "(" + row.bonus + ")", {inner.x, inner.y, inner.w - right, inner.h},
                                    render::HAlign::Right, render::VAlign::Middle, bs);
        }
    }
}

void KunyeWindow::draw(ui::DrawContext& ctx)
{
    if (!isVisible())
        return;
    ui::Panel::draw(ctx);
    const AttributeRowWidget* row = hoveredRow();
    if (row == nullptr || context() == nullptr)
        return;
    const auto a = static_cast<game::Attribute>(row->index());
    std::vector<ShownTipLine> lines;
    ShownTipLine title;
    title.text = std::string(game::attributeDisplayName(a));
    title.color = ctx.theme.textTitle;
    title.title = true;
    lines.push_back(std::move(title));
    ShownTipLine body;
    body.text = world::attributeTooltip(a, m_info.charClass);
    body.color = ctx.theme.text;
    body.wrap = true;
    lines.push_back(std::move(body));
    if (m_sheet.known())
    {
        ShownTipLine values;
        values.text = m_sheet.bonus(row->index()) > 0
                          ? std::format("Temel {} · eşyalardan +{}", m_sheet.base(row->index()), m_sheet.bonus(row->index()))
                          : std::format("Temel {}", m_sheet.base(row->index()));
        values.color = ctx.theme.textDim;
        lines.push_back(std::move(values));
    }
    drawTooltipBox(ctx, lines, row->rect(), context()->viewportWidth(), context()->viewportHeight());
}

} // namespace vo::client::scenes
