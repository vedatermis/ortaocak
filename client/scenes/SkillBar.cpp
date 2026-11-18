#include "client/scenes/SkillBar.h"

#include "client/render/TextRenderer.h"
#include "client/render/Texture2D.h"
#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"
#include "client/world/WorldMath.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;

namespace {

constexpr float kDragThreshold = 5.0f; // logical pixels
const Color kHelpful = Color::hex(0x5BD45E);
const Color kHarmful = Color::hex(0xE2483A);

// Art slot geometry (docs/SKILL_ART.md §2): 64 px piece, icon window (6, 6)-(58, 58), radius 7.
Rect iconWindow(const Rect& r)
{
    const float k = r.w / 64.0f;
    return Rect{r.x + 6.0f * k, r.y + 6.0f * k, 52.0f * k, 52.0f * k}.snapped();
}

} // namespace

void drawCooldownSweep(render::SpriteBatch& b, const Rect& icon, float sweep, Color color)
{
    // Clockwise sweep of the remaining time: radial strokes clipped to the icon (as the M9 button).
    b.pushClip(icon);
    const Vec2 c{icon.centerX(), icon.centerY()};
    const float len = std::max(icon.w, icon.h) * 0.75f;
    constexpr int kSteps = 96;
    const int lit = static_cast<int>(std::ceil(static_cast<float>(kSteps) * std::clamp(sweep, 0.0f, 1.0f)));
    for (int i = kSteps - lit; i < kSteps; ++i)
    {
        const float a = (static_cast<float>(i) + 0.5f) / static_cast<float>(kSteps) * world::kTwoPi;
        const Vec2 e{c.x + std::sin(a) * len, c.y - std::cos(a) * len};
        b.drawLine(c, e, len * world::kTwoPi / static_cast<float>(kSteps) * 1.25f, color);
    }
    b.popClip();
}

void drawSkillSlot(ui::DrawContext& ctx, const SkillUiHost& host, const Rect& r, const SkillUiHost::SlotView& v,
                   int keyNumber, bool hovered, bool pressed, bool dropTarget)
{
    auto& b = ctx.batch;
    const ui::Theme& t = ctx.theme;
    const float k = r.w / 64.0f;
    const Rect icon = iconWindow(r);
    if (v.item != nullptr)
    {
        // M14 (docs/PROTOCOL_SHOP.md §8.4): a potion: its icon, the count of that template in the bag in
        // the lower right corner (red 0, the icon greyed, the slot stays), the shared cooldown sweep.
        if (const render::Texture2D* back = host.skillImage("ui/skillbar/slot_back.png"))
            b.drawImage(*back, r);
        else
            b.fillRoundedRect(r, 7.0f * k, Color::hex(0x0C0F13, 0.9f));
        const bool none = v.count == 0;
        const Color tint = none ? Color{0.42f, 0.42f, 0.44f, 0.75f}
                                : (v.look == world::SlotLook::Dead ? Color{0.4f, 0.4f, 0.42f, 1.0f} : colors::kWhite);
        if (v.itemIcon != nullptr)
            b.drawImage(*v.itemIcon, icon.inset(std::round(2.0f * k)),
                        Color{tint.r * v.itemTint.r, tint.g * v.itemTint.g, tint.b * v.itemTint.b, tint.a});
        else
            b.fillRoundedRect(icon, 7.0f * k, Color::hex(0x3A2020));
        if (v.sweep > 0.0f)
        {
            drawCooldownSweep(b, icon, v.sweep, Color::hex(0x05070A, 0.66f));
            if (v.remaining > 0.6)
            {
                render::TextStyle st;
                st.color = Color::hex(0xFFF3D0);
                st.outline = true;
                render::drawTextAligned(b, ctx.fonts.get(render::FontWeight::Bold, std::round(icon.h * 0.42f)),
                                        std::format("{}", static_cast<int>(std::ceil(v.remaining))), icon,
                                        render::HAlign::Center, render::VAlign::Middle, st);
            }
        }
        if (v.readyFlash > 0.0f)
            if (const render::Texture2D* flash = host.skillImage("ui/skillbar/slot_ready_flash.png"))
                b.drawImage(*flash, r, Color{1.0f, 1.0f, 1.0f, v.readyFlash});
        const char* frame = pressed ? "ui/skillbar/slot_frame_pressed.png"
                                    : (hovered || dropTarget ? "ui/skillbar/slot_frame_hover.png"
                                                             : "ui/skillbar/slot_frame_normal.png");
        if (const render::Texture2D* f = host.skillImage(frame))
            b.drawImage(*f, r);
        else
            b.strokeRoundedRect(r, 7.0f * k, std::max(1.0f, ctx.pxf(1.0f)), t.buttonBorder);
        render::TextStyle cs;
        cs.color = none ? Color::hex(0xFF5A4A) : Color::hex(0xFFFFFF);
        cs.outline = true;
        cs.outlineColor = Color::hex(0x000000, 0.95f);
        const Rect corner{icon.x, icon.y, icon.w - std::round(1.0f * k), icon.h + std::round(1.0f * k)};
        render::drawTextAligned(b, ctx.fonts.get(render::FontWeight::Bold, std::max(10.0f, std::round(r.h * 0.3f))),
                                std::to_string(v.count), corner, render::HAlign::Right, render::VAlign::Bottom, cs);
    }
    else if (v.skill == nullptr)
    {
        if (const render::Texture2D* empty = host.skillImage("ui/skillbar/slot_empty.png"))
            b.drawImage(*empty, r);
        else
        {
            b.fillRoundedRect(r, 7.0f * k, Color::hex(0x0C0F13, 0.85f));
            b.strokeRoundedRect(r, 7.0f * k, std::max(1.0f, ctx.pxf(1.0f)), t.insetBorder);
        }
    }
    else
    {
        if (const render::Texture2D* back = host.skillImage("ui/skillbar/slot_back.png"))
            b.drawImage(*back, r);
        else
            b.fillRoundedRect(r, 7.0f * k, Color::hex(0x0C0F13, 0.9f));
        Color tint = colors::kWhite;
        switch (v.look)
        {
        case world::SlotLook::NoMana: tint = Color{1.0f, 0.5f, 0.48f, 1.0f}; break;
        case world::SlotLook::NoTarget: tint = Color{0.52f, 0.52f, 0.56f, 1.0f}; break;
        case world::SlotLook::Dead: tint = Color{0.4f, 0.4f, 0.42f, 1.0f}; break;
        default: break;
        }
        if (const render::Texture2D* img = host.skillImage(world::skillIconPath(*v.skill)))
            b.drawImage(*img, icon, tint);
        else
            b.fillRoundedRect(icon, 7.0f * k, Color::hex(0x3A3020));
        if (v.look == world::SlotLook::NoMana)
            b.fillRoundedRect(icon, 7.0f * k, Color::hex(0x9A1208, 0.34f)); // red: not enough mana
        else if (v.look == world::SlotLook::NoTarget)
            b.fillRoundedRect(icon, 7.0f * k, Color::hex(0x000000, 0.3f)); // darker: no / far target
        if (v.sweep > 0.0f)
        {
            drawCooldownSweep(b, icon, v.sweep, Color::hex(0x05070A, 0.66f));
            if (v.remaining > 0.6)
            {
                render::TextStyle st;
                st.color = Color::hex(0xFFF3D0);
                st.outline = true;
                const std::string s = std::format("{}", static_cast<int>(std::ceil(v.remaining)));
                render::drawTextAligned(b, ctx.fonts.get(render::FontWeight::Bold, std::round(icon.h * 0.42f)), s, icon,
                                        render::HAlign::Center, render::VAlign::Middle, st);
            }
        }
        if (v.readyFlash > 0.0f)
        {
            if (const render::Texture2D* flash = host.skillImage("ui/skillbar/slot_ready_flash.png"))
                b.drawImage(*flash, r, Color{1.0f, 1.0f, 1.0f, v.readyFlash});
        }
        const char* frame = pressed ? "ui/skillbar/slot_frame_pressed.png"
                                    : (hovered || dropTarget ? "ui/skillbar/slot_frame_hover.png"
                                                             : "ui/skillbar/slot_frame_normal.png");
        if (const render::Texture2D* f = host.skillImage(frame))
            b.drawImage(*f, r);
        else
            b.strokeRoundedRect(r, 7.0f * k, std::max(1.0f, ctx.pxf(1.0f)), t.buttonBorder);
        if (v.active)
            b.strokeRoundedRect(icon, 7.0f * k, std::max(2.0f, ctx.pxf(2.0f)), t.accentBright);
    }
    if (dropTarget)
        b.strokeRoundedRect(r.inset(-ctx.pxf(1.0f)), 8.0f * k, std::max(2.0f, ctx.pxf(2.0f)), t.accentBright);
    if (keyNumber > 0)
    {
        const float badge = std::round(std::max(14.0f, r.w * 0.36f));
        const Rect br = Rect{r.x - std::round(badge * 0.12f), r.y - std::round(badge * 0.12f), badge, badge}.snapped();
        if (const render::Texture2D* kb = host.skillImage("ui/skillbar/key_badge.png"))
            b.drawImage(*kb, br);
        else
            b.fillRoundedRect(br, badge * 0.25f, Color::hex(0x101317, 0.9f));
        render::TextStyle ks;
        ks.color = t.text;
        render::drawTextAligned(b, ctx.fonts.get(render::FontWeight::Bold, std::round(badge * 0.62f)),
                                std::to_string(keyNumber), br, render::HAlign::Center, render::VAlign::Middle, ks);
    }
}

// ---- SkillSlotWidget ----

SkillSlotWidget::SkillSlotWidget(SkillUiHost& host, std::size_t index, bool clickUses)
    : m_host(host), m_index(index), m_clickUses(clickUses)
{
}

bool SkillSlotWidget::onMouseDown(const platform::InputEvent& e)
{
    if (e.button == platform::MouseButton::Right)
    {
        m_host.clearBarSlot(m_index);
        return true;
    }
    if (e.button != platform::MouseButton::Left)
        return false;
    m_pressed = true;
    m_dragged = false;
    m_pressX = static_cast<float>(e.x);
    m_pressY = static_cast<float>(e.y);
    return true;
}

bool SkillSlotWidget::onMouseMove(const platform::InputEvent& e)
{
    if (!m_pressed || !hasCapture())
        return false;
    const auto x = static_cast<float>(e.x);
    const auto y = static_cast<float>(e.y);
    const std::uint16_t skill = m_host.skillBook().slot(m_index);
    if (!m_dragged && skill != 0 && std::hypot(x - m_pressX, y - m_pressY) > kDragThreshold * layoutScale())
    {
        m_dragged = true;
        m_host.beginSkillDrag(skill, static_cast<int>(m_index));
    }
    if (m_dragged)
        m_host.moveSkillDrag(x, y);
    return true;
}

bool SkillSlotWidget::onMouseUp(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return e.button == platform::MouseButton::Right;
    const bool wasPressed = m_pressed;
    m_pressed = false;
    if (m_dragged)
    {
        m_dragged = false;
        m_host.endSkillDrag(static_cast<float>(e.x), static_cast<float>(e.y), true);
        return true;
    }
    if (wasPressed && m_clickUses && rect().contains(static_cast<float>(e.x), static_cast<float>(e.y)))
        m_host.useBarSlot(m_index);
    return true;
}

void SkillSlotWidget::onCaptureLost()
{
    m_pressed = false;
    if (m_dragged)
    {
        m_dragged = false;
        m_host.endSkillDrag(0.0f, 0.0f, false);
    }
}

void SkillSlotWidget::drawSelf(ui::DrawContext& ctx)
{
    const SkillUiHost::SlotView v = m_host.slotView(m_index);
    const bool dragging = m_host.skillDragActive();
    drawSkillSlot(ctx, m_host, rect(), v, static_cast<int>(m_index) + 1, isHovered() && !dragging,
                  m_pressed && !m_dragged && isHovered(), dragging && isHovered());
}

// ---- SkillSlotsWidget ----

SkillSlotsWidget::SkillSlotsWidget(SkillUiHost& host, std::string_view prefix, float slot, float gap, bool clickUses)
{
    setMouseTransparent(true); // the gaps let clicks through to the world
    setSize(widthFor(slot, gap), slot);
    for (std::size_t i = 0; i < m_slots.size(); ++i)
    {
        auto& s = add<SkillSlotWidget>(host, i, clickUses);
        s.setAnchors(anchor::kTopLeft).setMargins({static_cast<float>(i) * (slot + gap), 0.0f, 0.0f, 0.0f}).setSize(slot, slot);
        s.setName(std::format("{}.{}", prefix, i + 1));
        m_slots[i] = &s;
    }
}

void SkillSlotsWidget::setMetrics(float slot, float gap)
{
    if (layoutSpec().size.y == slot && m_slots[1]->layoutSpec().margins.left == slot + gap)
        return;
    setSize(widthFor(slot, gap), slot);
    for (std::size_t i = 0; i < m_slots.size(); ++i)
        m_slots[i]->setMargins({static_cast<float>(i) * (slot + gap), 0.0f, 0.0f, 0.0f}).setSize(slot, slot);
}

int SkillSlotsWidget::slotAt(float x, float y) const
{
    if (!isVisible())
        return -1;
    for (const SkillSlotWidget* s : m_slots)
        if (s->rect().contains(x, y))
            return static_cast<int>(s->index());
    return -1;
}

// ---- Effect icons ----

EffectIconWidget::EffectIconWidget(SkillUiHost& host, bool target, std::size_t index)
    : m_host(host), m_target(target), m_index(index)
{
}

const world::ShownEffect* EffectIconWidget::effect() const
{
    const auto list = m_host.shownEffects(m_target);
    return m_index < list.size() ? &list[m_index] : nullptr;
}

void EffectIconWidget::drawSelf(ui::DrawContext& ctx)
{
    const world::ShownEffect* e = effect();
    if (e == nullptr)
        return;
    auto& b = ctx.batch;
    const Rect r = rect();
    const float radius = ctx.pxf(4.0f);
    const bool helpful = world::effectHelpful(e->kind);
    b.fillRoundedRect(r.inset(-ctx.pxf(1.0f)), radius + ctx.pxf(1.0f), Color::hex(0x05070A, 0.75f));
    const Rect inner = r.inset(ctx.px(2.0f)).snapped();
    const game::SkillDef* s = game::skills().find(e->skillId);
    const render::Texture2D* icon = s != nullptr ? m_host.skillImage(world::skillIconPath(*s)) : nullptr;
    const render::Texture2D* status = m_host.skillImage(world::effectStatusIcon(e->kind, e->skillId));
    if (icon != nullptr)
        b.drawImage(*icon, inner);
    else if (status != nullptr)
        b.drawImage(*status, inner);
    // The effect's own status icon in the bottom-right corner (several effects of one skill differ).
    if (icon != nullptr && status != nullptr)
    {
        const float badge = std::round(r.w * 0.5f);
        b.drawImage(*status, Rect{r.right() - badge + ctx.pxf(2.0f), r.bottom() - badge + ctx.pxf(2.0f), badge, badge}.snapped());
    }
    const double left = world::effectRemaining(*e, m_host.skillClock());
    // The last two seconds blink the frame a little.
    float frameAlpha = 1.0f;
    if (left < 2.0)
        frameAlpha = 0.55f + 0.45f * std::fabs(std::sin(static_cast<float>(left) * 6.0f));
    b.strokeRoundedRect(r, radius, std::max(2.0f, ctx.pxf(2.0f)), (helpful ? kHelpful : kHarmful).withAlpha(frameAlpha));
    render::TextStyle ts;
    ts.color = Color::hex(0xF6F0DE);
    ts.outline = true;
    ts.outlineColor = Color::hex(0x000000, 0.9f);
    const render::Font& sf = ctx.font(11.0f, render::FontWeight::Bold);
    render::drawTextAligned(b, sf, world::effectSecondsText(left), {r.x - ctx.px(6.0f), r.bottom(), r.w + ctx.px(12.0f), sf.lineHeight()},
                            render::HAlign::Center, render::VAlign::Top, ts);
    if (e->kind == static_cast<std::uint8_t>(game::EffectKind::Shield))
    {
        render::TextStyle as = ts;
        as.color = Color::hex(0xBFE6FF);
        render::drawTextAligned(b, ctx.font(10.0f, render::FontWeight::Bold), std::to_string(e->amount),
                                {r.x - ctx.px(6.0f), r.y - ctx.px(1.0f), r.w + ctx.px(12.0f), r.h * 0.5f},
                                render::HAlign::Center, render::VAlign::Top, as);
    }
}

EffectRowWidget::EffectRowWidget(SkillUiHost& host, bool target, std::size_t perRow)
    : m_host(host), m_target(target), m_perRow(std::max<std::size_t>(perRow, 1))
{
    setMouseTransparent(true);
    setName(target ? "effects.target" : "effects.self");
    for (std::size_t i = 0; i < m_icons.size(); ++i)
    {
        auto& icon = add<EffectIconWidget>(host, target, i);
        icon.setAnchors(anchor::kTopLeft).setSize(kIcon, kIcon);
        icon.setName(std::format("{}.{}", target ? "effects.target" : "effects.self", i));
        icon.setVisible(false);
        m_icons[i] = &icon;
    }
}

void EffectRowWidget::update(double dt)
{
    const std::size_t n = m_host.shownEffects(m_target).size();
    for (std::size_t i = 0; i < m_icons.size(); ++i)
    {
        const bool show = i < n;
        if (show)
        {
            const float x = static_cast<float>(i % m_perRow) * (kIcon + kGap);
            const float y = static_cast<float>(i / m_perRow) * kRowH;
            const Insets& m = m_icons[i]->layoutSpec().margins;
            if (m.left != x || m.top != y)
                m_icons[i]->setMargins({x, y, 0.0f, 0.0f});
        }
        if (m_icons[i]->isVisible() != show)
            m_icons[i]->setVisible(show);
    }
    ui::Widget::update(dt);
}

} // namespace vo::client::scenes
