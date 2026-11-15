#include "client/ui/TextBox.h"

#include "client/core/Utf8.h"
#include "client/ui/ClipboardProvider.h"
#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"

#include <algorithm>
#include <cmath>

namespace vo::client::ui {

using platform::InputEvent;
namespace keys = platform::keys;

namespace {

constexpr char32_t kMaskChar = 0x2022; // bullet
constexpr double kBlinkPeriod = 1.06;

const render::Font& boxFont(const UiContext& ui)
{
    return ui.fonts().get(render::FontWeight::Regular, ui.theme().fontNormal * ui.scale());
}

} // namespace

TextBox::TextBox()
{
    setFocusable(true);
    m_stops.reserve(64);
}

TextBox& TextBox::setText(std::string_view text)
{
    m_model.setText(text);
    changed();
    return *this;
}

TextBox& TextBox::setPlaceholder(std::string text)
{
    m_placeholder = std::move(text);
    return *this;
}

TextBox& TextBox::setPassword(bool password)
{
    m_password = password;
    changed();
    return *this;
}

TextBox& TextBox::setMaxLength(std::size_t codePoints)
{
    m_model.setMaxCodePoints(codePoints);
    changed();
    return *this;
}

TextBox& TextBox::setOnSubmit(std::function<void(const std::string&)> onSubmit)
{
    m_onSubmit = std::move(onSubmit);
    return *this;
}

TextBox& TextBox::setOnChange(std::function<void(const std::string&)> onChange)
{
    m_onChange = std::move(onChange);
    return *this;
}

void TextBox::touch() noexcept
{
    if (context() != nullptr)
        m_lastActivity = context()->time();
}

void TextBox::changed()
{
    if (m_password)
    {
        m_mask.clear();
        for (std::size_t i = 0; i < m_model.codePointCount(); ++i)
            utf8::append(m_mask, kMaskChar);
    }
    touch();
}

Rect TextBox::textArea() const
{
    const float scale = context() != nullptr ? context()->scale() : 1.0f;
    const float padX = context() != nullptr ? std::round(context()->theme().textPaddingX * scale) : 8.0f;
    const Rect r = rect();
    return {r.x + padX, r.y, std::max(0.0f, r.w - 2.0f * padX), r.h};
}

void TextBox::refreshStops(DrawContext& ctx)
{
    const render::Font& font = ctx.font(TextRole::Normal);
    if (m_stopsRevision == m_model.revision() && m_stopsFontPx == font.pixelSize() &&
        m_stopsPassword == m_password)
        return;
    caretStops(font, displayText(), m_stops);
    m_stopsRevision = m_model.revision();
    m_stopsFontPx = font.pixelSize();
    m_stopsPassword = m_password;
}

std::size_t TextBox::stopIndexOf(std::size_t byteOffset) const
{
    return utf8::countCodePoints(std::string_view(m_model.text()).substr(0, byteOffset));
}

std::size_t TextBox::offsetAtX(float x) const
{
    // Called from mouse handlers: make sure the stops match the current text first.
    if (context() == nullptr)
        return 0;
    std::vector<float> stops;
    const std::vector<float>* use = &m_stops;
    const render::Font& font = boxFont(*context());
    if (m_stopsRevision != m_model.revision() || m_stopsFontPx != font.pixelSize() ||
        m_stopsPassword != m_password)
    {
        caretStops(font, displayText(), stops);
        use = &stops;
    }
    const float local = x - textArea().x + m_scroll;
    std::size_t best = 0;
    float bestDist = 1e30f;
    for (std::size_t i = 0; i < use->size(); ++i)
    {
        const float d = std::fabs((*use)[i] - local);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return utf8::offsetOfIndex(m_model.text(), best);
}

bool TextBox::onMouseDown(const InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    const std::size_t offset = offsetAtX(static_cast<float>(e.x));
    if (e.clickCount >= 2)
    {
        if (m_password)
            m_model.selectAll();
        else
            m_model.selectWordAt(offset);
        m_dragging = false;
    }
    else
    {
        m_model.setCaret(offset, e.shift());
        m_dragging = true;
    }
    touch();
    return true;
}

bool TextBox::onMouseMove(const InputEvent& e)
{
    if (!m_dragging)
        return false;
    m_model.setCaret(offsetAtX(static_cast<float>(e.x)), true);
    touch();
    return true;
}

bool TextBox::onMouseUp(const InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    m_dragging = false;
    return true;
}

bool TextBox::onKeyDown(const InputEvent& e)
{
    const bool shift = e.shift();
    const bool ctrl = e.ctrlOnly();
    if (e.alt() && !ctrl && (e.mods & platform::mods::kCtrl) == 0)
        return false; // Alt+key belongs to the game / system
    const std::uint64_t before = m_model.revision();
    bool handled = true;

    switch (e.key)
    {
    case keys::kLeft:
        if (ctrl)
            m_model.moveWordLeft(shift);
        else
            m_model.moveLeft(shift);
        break;
    case keys::kRight:
        if (ctrl)
            m_model.moveWordRight(shift);
        else
            m_model.moveRight(shift);
        break;
    case keys::kHome:
        m_model.moveHome(shift);
        break;
    case keys::kEnd:
        m_model.moveEnd(shift);
        break;
    case keys::kBackspace:
        if (ctrl && !m_password)
            m_model.deleteWordBackward();
        else
            m_model.backspace();
        break;
    case keys::kDelete:
        if (shift && !ctrl && !m_password && m_model.hasSelection()) // Shift+Del = cut
        {
            if (auto* clip = context()->clipboard())
                clip->setText(m_model.selectedText());
        }
        m_model.deleteForward();
        break;
    case keys::kEnter:
        if (m_onSubmit)
            m_onSubmit(m_model.text());
        else
            handled = false;
        break;
    default:
        handled = false;
        if (ctrl)
        {
            ClipboardProvider* clip = context() != nullptr ? context()->clipboard() : nullptr;
            handled = true;
            if (e.key == keys::letter('a'))
                m_model.selectAll();
            else if (e.key == keys::letter('c'))
            {
                if (!m_password && clip != nullptr && m_model.hasSelection())
                    clip->setText(m_model.selectedText());
            }
            else if (e.key == keys::letter('x'))
            {
                if (!m_password && clip != nullptr && m_model.hasSelection())
                {
                    clip->setText(m_model.selectedText());
                    m_model.deleteSelection();
                }
            }
            else if (e.key == keys::letter('v'))
            {
                if (clip != nullptr)
                {
                    std::string pasted = clip->getText();
                    // Single-line box: newlines become spaces (control chars are dropped by the model).
                    std::replace(pasted.begin(), pasted.end(), '\n', ' ');
                    m_model.insert(pasted);
                }
            }
            else
                handled = false;
        }
        break;
    }

    if (handled)
    {
        if (m_model.revision() != before)
        {
            changed();
            if (m_onChange)
                m_onChange(m_model.text());
        }
        touch();
    }
    return handled;
}

bool TextBox::onChar(const InputEvent& e)
{
    if (m_model.insertCodePoint(e.codePoint))
    {
        changed();
        if (m_onChange)
            m_onChange(m_model.text());
    }
    touch();
    return true;
}

void TextBox::onFocusGained()
{
    touch();
}

void TextBox::onFocusLost()
{
    m_dragging = false;
}

void TextBox::drawSelf(DrawContext& ctx)
{
    const Theme& t = ctx.theme;
    const Rect r = rect();
    auto& b = ctx.batch;
    const bool enabled = isEnabled();
    const bool focused = enabled && isFocused();
    const bool hovered = enabled && isHovered();
    const float radius = ctx.pxf(t.cornerRadius * 0.8f);
    const float border = std::max(1.0f, ctx.pxf(t.borderWidth));

    if (t.inputSkin.texture != nullptr)
        b.drawNineSlice(*t.inputSkin.texture, r, t.inputSkin.srcBorder,
                        t.inputSkin.dstBorder.scaled(ctx.scale), t.inputSkin.tint);
    else
    {
        b.fillRoundedRect(r, radius, hovered && !focused ? t.inputFillHover : t.inputFill);
        b.fillRect({r.x + radius, r.y + border, r.w - 2.0f * radius, border}, Color::hex(0x000000, 0.5f));
        const Color bc = focused ? t.focusRing : (hovered ? t.inputBorderHover : t.inputBorder);
        b.strokeRoundedRect(r, radius, focused ? std::max(1.0f, ctx.pxf(t.focusRingWidth)) : border, bc);
    }

    refreshStops(ctx);
    const render::Font& font = ctx.font(TextRole::Normal);
    const Rect area = textArea();
    const float caretW = std::max(1.0f, std::round(ctx.pxf(t.caretWidth)));

    // Keep the caret inside the visible area.
    const std::size_t caretIndex = std::min(stopIndexOf(m_model.caret()), m_stops.size() - 1);
    const float caretX = std::round(m_stops[caretIndex]);
    const float total = std::round(m_stops.back());
    if (caretX - m_scroll > area.w - caretW)
        m_scroll = caretX - area.w + caretW;
    if (caretX - m_scroll < 0.0f)
        m_scroll = caretX;
    const float maxScroll = std::max(0.0f, total - area.w + caretW);
    m_scroll = std::clamp(m_scroll, 0.0f, maxScroll);

    const float textY = std::round(area.y + (area.h - font.lineHeight()) * 0.5f);
    const float originX = area.x - m_scroll;

    b.pushClip(area.inset(-caretW, 0.0f, -caretW, 0.0f));
    if (focused && m_model.hasSelection())
    {
        const float x0 = std::round(m_stops[stopIndexOf(m_model.selectionBegin())]);
        const float x1 = std::round(m_stops[stopIndexOf(m_model.selectionEnd())]);
        b.fillRect({originX + x0, textY, x1 - x0, font.lineHeight()}, t.selection);
    }

    render::TextStyle style;
    if (m_model.empty())
    {
        if (!m_placeholder.empty())
        {
            style.color = t.placeholder;
            render::drawText(b, font, m_placeholder, {area.x, textY}, style);
        }
    }
    else
    {
        style.color = enabled ? t.text : t.textDisabled;
        render::drawText(b, font, displayText(), {originX, textY}, style);
    }

    if (focused)
    {
        const double phase = std::fmod(ctx.time - m_lastActivity, kBlinkPeriod);
        if (phase < kBlinkPeriod * 0.5)
        {
            const float inset = std::round(font.lineHeight() * 0.12f);
            b.fillRect({originX + caretX, textY + inset, caretW, font.lineHeight() - 2.0f * inset}, t.caret);
        }
    }
    b.popClip();
}

} // namespace vo::client::ui
