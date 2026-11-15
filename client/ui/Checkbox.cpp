#include "client/ui/Checkbox.h"

#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"

namespace vo::client::ui {

using platform::InputEvent;

Checkbox::Checkbox(std::string text, bool checked) : m_text(std::move(text)), m_checked(checked)
{
    setFocusable(true);
}

Checkbox& Checkbox::setText(std::string text)
{
    m_text = std::move(text);
    return *this;
}

Checkbox& Checkbox::setChecked(bool checked)
{
    m_checked = checked;
    return *this;
}

Checkbox& Checkbox::setOnChange(std::function<void(bool)> onChange)
{
    m_onChange = std::move(onChange);
    return *this;
}

void Checkbox::toggle()
{
    if (!isEnabled())
        return;
    m_checked = !m_checked;
    if (m_onChange)
        m_onChange(m_checked);
}

bool Checkbox::onMouseDown(const InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    m_pressed = true;
    return true;
}

bool Checkbox::onMouseUp(const InputEvent& e)
{
    if (e.button != platform::MouseButton::Left || !m_pressed)
        return false;
    m_pressed = false;
    if (rect().contains(static_cast<float>(e.x), static_cast<float>(e.y)))
        toggle();
    return true;
}

bool Checkbox::onKeyDown(const InputEvent& e)
{
    if (e.key == platform::keys::kSpace && !e.repeat && e.mods == 0)
    {
        toggle();
        return true;
    }
    return false;
}

void Checkbox::drawSelf(DrawContext& ctx)
{
    const Theme& t = ctx.theme;
    const Rect r = rect();
    auto& b = ctx.batch;
    const bool enabled = isEnabled();
    const bool hovered = enabled && isHovered();
    const bool focused = enabled && isFocused();
    const float size = ctx.px(t.checkboxSize);
    const Rect box{r.x, std::round(r.y + (r.h - size) * 0.5f), size, size};
    const float radius = ctx.pxf(3.0f);
    const float border = std::max(1.0f, ctx.pxf(t.borderWidth));

    if (focused)
    {
        const float ring = ctx.px(3.0f);
        b.strokeRoundedRect(box.inset(-ring), radius + ring, std::max(1.0f, ctx.pxf(t.focusRingWidth)),
                            t.focusRing);
    }
    b.fillRoundedRect(box, radius, hovered ? t.inputFillHover : t.inputFill);
    b.strokeRoundedRect(box, radius, border,
                        !enabled ? t.inputBorder
                                 : (hovered || m_checked ? t.buttonBorderHover : t.buttonBorder));
    if (m_checked)
    {
        // Check mark: two round-capped strokes.
        const float w = std::max(1.5f, ctx.pxf(2.2f));
        const Color c = enabled ? t.checkMark : t.textDisabled;
        const Vec2 p0{box.x + size * 0.24f, box.y + size * 0.52f};
        const Vec2 p1{box.x + size * 0.43f, box.y + size * 0.72f};
        const Vec2 p2{box.x + size * 0.78f, box.y + size * 0.30f};
        b.drawLine(p0, p1, w, c, true);
        b.drawLine(p1, p2, w, c, true);
    }

    const render::Font& font = ctx.font(TextRole::Normal);
    render::TextStyle style;
    style.color = enabled ? (hovered ? t.accentBright : t.text) : t.textDisabled;
    const float gap = ctx.px(8.0f);
    render::drawTextAligned(b, font, m_text, {box.right() + gap, r.y, r.w - size - gap, r.h},
                            render::HAlign::Left, render::VAlign::Middle, style);
}

} // namespace vo::client::ui
