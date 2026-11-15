#include "client/ui/Button.h"

#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"

namespace vo::client::ui {

using platform::InputEvent;

Button::Button(std::string text, std::function<void()> onClick)
    : m_text(std::move(text)), m_onClick(std::move(onClick))
{
    setFocusable(true);
}

Button& Button::setText(std::string text)
{
    m_text = std::move(text);
    return *this;
}

Button& Button::setOnClick(std::function<void()> onClick)
{
    m_onClick = std::move(onClick);
    return *this;
}

void Button::click()
{
    if (!isEnabled())
        return;
    if (m_onClick)
        m_onClick(); // may close/destroy UI elsewhere; UiContext defers removals so `this` stays valid
}

bool Button::onMouseDown(const InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    m_pressed = true;
    return true;
}

bool Button::onMouseUp(const InputEvent& e)
{
    if (e.button != platform::MouseButton::Left || !m_pressed)
        return false;
    m_pressed = false;
    if (rect().contains(static_cast<float>(e.x), static_cast<float>(e.y)))
        click();
    return true;
}

bool Button::onKeyDown(const InputEvent& e)
{
    if ((e.key == platform::keys::kEnter || e.key == platform::keys::kSpace) && !e.repeat && e.mods == 0)
    {
        if (context() != nullptr)
            m_flashUntil = context()->time() + 0.12;
        click();
        return true;
    }
    return false;
}

void Button::drawSelf(DrawContext& ctx)
{
    const Theme& t = ctx.theme;
    const Rect r = rect();
    auto& b = ctx.batch;
    const bool enabled = isEnabled();
    const bool hovered = enabled && isHovered();
    const bool pressed = enabled && ((m_pressed && hovered) || ctx.time < m_flashUntil);
    const bool focused = enabled && isFocused();
    const float radius = ctx.pxf(t.cornerRadius);
    const float border = std::max(1.0f, ctx.pxf(t.borderWidth));

    Color top = t.buttonTop;
    Color bottom = t.buttonBottom;
    if (!enabled)
    {
        top = t.buttonDisabledTop;
        bottom = t.buttonDisabledBottom;
    }
    else if (pressed)
    {
        top = t.buttonPressedTop;
        bottom = t.buttonPressedBottom;
    }
    else if (hovered)
    {
        top = t.buttonHoverTop;
        bottom = t.buttonHoverBottom;
    }

    if (focused)
    {
        const float ring = ctx.px(3.0f);
        b.strokeRoundedRect(r.inset(-ring), radius + ring, std::max(1.0f, ctx.pxf(t.focusRingWidth)),
                            t.focusRing.withAlpha(0.9f));
    }

    if (t.buttonSkin.texture != nullptr)
    {
        Color tint = t.buttonSkin.tint;
        if (!enabled)
            tint = tint.scaledAlpha(0.5f);
        b.drawNineSlice(*t.buttonSkin.texture, r, t.buttonSkin.srcBorder,
                        t.buttonSkin.dstBorder.scaled(ctx.scale), tint);
    }
    else
    {
        b.fillRoundedRectGradient(r, radius, top, bottom);
        // Top bevel highlight (not when pressed).
        if (!pressed && enabled)
            b.fillRect({r.x + radius, r.y + border, r.w - 2.0f * radius, border},
                       Color::hex(0xFFFFFF, 0.10f));
        const Color borderColor =
            !enabled ? t.inputBorder : (hovered || focused ? t.buttonBorderHover : t.buttonBorder);
        b.strokeRoundedRect(r, radius, border, borderColor);
    }

    const render::Font& font = ctx.font(TextRole::Normal, render::FontWeight::Bold);
    render::TextStyle style;
    style.color = enabled ? (hovered ? t.accentBright : t.buttonText) : t.textDisabled;
    style.shadow = enabled;
    style.shadowColor = t.textShadow;
    style.shadowOffset = std::max(1.0f, std::round(ctx.scale));
    const float push = pressed ? ctx.px(1.0f) : 0.0f;
    render::drawTextAligned(b, font, m_text, {r.x, r.y + push, r.w, r.h}, render::HAlign::Center,
                            render::VAlign::Middle, style);
}

} // namespace vo::client::ui
