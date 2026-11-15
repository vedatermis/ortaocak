#include "client/ui/ListBox.h"

#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"

#include <algorithm>
#include <cmath>

namespace vo::client::ui {

using platform::InputEvent;
namespace keys = platform::keys;

ListBox::ListBox()
{
    setFocusable(true);
}

ListBox& ListBox::setItems(std::vector<std::string> items)
{
    m_items = std::move(items);
    if (m_selected >= static_cast<int>(m_items.size()))
        m_selected = -1;
    clampScroll();
    return *this;
}

ListBox& ListBox::addItem(std::string item)
{
    m_items.push_back(std::move(item));
    return *this;
}

ListBox& ListBox::clearItems()
{
    m_items.clear();
    m_selected = -1;
    m_hoverRow = -1;
    m_scroll = 0.0f;
    return *this;
}

ListBox& ListBox::setSelectedIndex(int index, bool notify)
{
    select(index, notify);
    return *this;
}

ListBox& ListBox::setOnSelect(std::function<void(int)> onSelect)
{
    m_onSelect = std::move(onSelect);
    return *this;
}

ListBox& ListBox::setOnActivate(std::function<void(int)> onActivate)
{
    m_onActivate = std::move(onActivate);
    return *this;
}

ListBox& ListBox::setRowHeight(float logical)
{
    m_rowHeight = logical;
    return *this;
}

float ListBox::rowHeightPx() const
{
    if (context() == nullptr)
        return 24.0f;
    const float logical = m_rowHeight > 0.0f ? m_rowHeight : context()->theme().rowHeight;
    return std::max(1.0f, std::round(logical * context()->scale()));
}

Rect ListBox::innerRect() const
{
    const float b = context() != nullptr ? std::max(1.0f, std::round(context()->scale())) : 1.0f;
    return rect().inset(b + 1.0f);
}

float ListBox::maxScroll() const
{
    return std::max(0.0f, contentHeight() - innerRect().h);
}

void ListBox::clampScroll()
{
    // The offset is in physical pixels: keep the same rows in view when the UI scale changes.
    const float scale = context() != nullptr ? context()->scale() : 1.0f;
    if (m_scrollScale > 0.0f && m_scrollScale != scale)
        m_scroll *= scale / m_scrollScale;
    m_scrollScale = scale;
    m_scroll = std::clamp(m_scroll, 0.0f, maxScroll());
}

Rect ListBox::scrollbarTrack() const
{
    const Rect inner = innerRect();
    const float w =
        context() != nullptr ? std::round(context()->theme().scrollbarWidth * context()->scale()) : 8.0f;
    return {inner.right() - w, inner.y, w, inner.h};
}

Rect ListBox::scrollbarThumb() const
{
    const Rect track = scrollbarTrack();
    const float content = contentHeight();
    if (content <= 0.0f)
        return track;
    const float visible = innerRect().h;
    const float h = std::max(track.h * visible / content, track.w * 2.0f);
    const float range = maxScroll();
    const float t = range > 0.0f ? m_scroll / range : 0.0f;
    return {track.x, std::round(track.y + (track.h - h) * t), track.w, std::round(h)};
}

int ListBox::rowAt(float y) const
{
    const Rect inner = innerRect();
    if (y < inner.y || y >= inner.bottom())
        return -1;
    const int row = static_cast<int>(std::floor((y - inner.y + m_scroll) / rowHeightPx()));
    return row >= 0 && row < static_cast<int>(m_items.size()) ? row : -1;
}

void ListBox::select(int index, bool notify)
{
    if (index < -1 || index >= static_cast<int>(m_items.size()))
        index = -1;
    if (index == m_selected)
        return;
    m_selected = index;
    ensureVisible(index);
    if (notify && m_onSelect)
        m_onSelect(index);
}

void ListBox::ensureVisible(int index)
{
    if (index < 0)
        return;
    const float rh = rowHeightPx();
    const float top = rh * static_cast<float>(index);
    const float visible = innerRect().h;
    if (top < m_scroll)
        m_scroll = top;
    else if (top + rh > m_scroll + visible)
        m_scroll = top + rh - visible;
    clampScroll();
}

bool ListBox::onMouseDown(const InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    const auto x = static_cast<float>(e.x);
    const auto y = static_cast<float>(e.y);
    if (hasScrollbar() && scrollbarTrack().contains(x, y))
    {
        const Rect thumb = scrollbarThumb();
        if (thumb.contains(x, y))
            m_dragOffset = y - thumb.y;
        else
            m_dragOffset = thumb.h * 0.5f; // jump so the thumb centres on the click
        m_draggingThumb = true;
        onMouseMove(e);
        return true;
    }
    const int row = rowAt(y);
    if (row >= 0)
    {
        select(row, true);
        if (e.clickCount >= 2 && m_onActivate)
            m_onActivate(row);
    }
    return true;
}

bool ListBox::onMouseMove(const InputEvent& e)
{
    const auto y = static_cast<float>(e.y);
    if (m_draggingThumb)
    {
        const Rect track = scrollbarTrack();
        const Rect thumb = scrollbarThumb();
        const float range = track.h - thumb.h;
        const float t = range > 0.0f ? std::clamp((y - m_dragOffset - track.y) / range, 0.0f, 1.0f) : 0.0f;
        m_scroll = t * maxScroll();
        return true;
    }
    const bool overBar = hasScrollbar() && scrollbarTrack().contains(static_cast<float>(e.x), y);
    m_hoverRow = overBar ? -1 : rowAt(y);
    return true;
}

bool ListBox::onMouseUp(const InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    m_draggingThumb = false;
    return true;
}

bool ListBox::onMouseWheel(const InputEvent& e)
{
    if (maxScroll() <= 0.0f)
        return false;
    m_scroll -= e.wheel * rowHeightPx() * 3.0f;
    clampScroll();
    m_hoverRow = rowAt(static_cast<float>(e.y));
    return true;
}

bool ListBox::onKeyDown(const InputEvent& e)
{
    if (m_items.empty())
        return false;
    const int count = static_cast<int>(m_items.size());
    const int page = std::max(1, static_cast<int>(innerRect().h / rowHeightPx()) - 1);
    int next = m_selected;
    switch (e.key)
    {
    case keys::kUp:
        next = m_selected < 0 ? count - 1 : std::max(0, m_selected - 1);
        break;
    case keys::kDown:
        next = m_selected < 0 ? 0 : std::min(count - 1, m_selected + 1);
        break;
    case keys::kPageUp:
        next = std::max(0, (m_selected < 0 ? 0 : m_selected) - page);
        break;
    case keys::kPageDown:
        next = std::min(count - 1, (m_selected < 0 ? 0 : m_selected) + page);
        break;
    case keys::kHome:
        next = 0;
        break;
    case keys::kEnd:
        next = count - 1;
        break;
    case keys::kEnter:
        if (m_selected >= 0 && m_onActivate)
        {
            m_onActivate(m_selected);
            return true;
        }
        return false;
    default:
        return false;
    }
    select(next, true);
    ensureVisible(next);
    return true;
}

void ListBox::drawSelf(DrawContext& ctx)
{
    clampScroll();
    const Theme& t = ctx.theme;
    const Rect r = rect();
    auto& b = ctx.batch;
    const bool focused = isEnabled() && isFocused();
    const float radius = ctx.pxf(t.cornerRadius * 0.8f);
    const float border = std::max(1.0f, ctx.pxf(t.borderWidth));

    b.fillRoundedRect(r, radius, t.inputFill);
    b.strokeRoundedRect(r, radius, focused ? std::max(1.0f, ctx.pxf(t.focusRingWidth)) : border,
                        focused ? t.focusRing : t.inputBorder);

    const Rect inner = innerRect();
    const bool bar = hasScrollbar();
    const float barW = bar ? scrollbarTrack().w + ctx.px(2.0f) : 0.0f;
    const float rh = rowHeightPx();
    const render::Font& font = ctx.font(TextRole::Normal);
    const float padX = ctx.px(t.textPaddingX);

    b.pushClip(inner);
    const int first = std::max(0, static_cast<int>(std::floor(m_scroll / rh)));
    const int last = std::min(static_cast<int>(m_items.size()) - 1,
                              static_cast<int>(std::ceil((m_scroll + inner.h) / rh)));
    for (int i = first; i <= last; ++i)
    {
        const Rect row{inner.x, std::round(inner.y + rh * static_cast<float>(i) - m_scroll), inner.w - barW,
                       rh};
        const bool selected = i == m_selected;
        if (selected)
        {
            b.fillRect(row, t.rowSelected);
            b.fillRect({row.x, row.y, ctx.px(3.0f), row.h}, t.accent); // gold marker
        }
        else if (i == m_hoverRow && isEnabled())
            b.fillRect(row, t.rowHover);
        render::TextStyle style;
        style.color = !isEnabled() ? t.textDisabled : (selected ? t.rowSelectedText : t.text);
        render::drawTextAligned(b, font, m_items[static_cast<std::size_t>(i)],
                                {row.x + padX, row.y, row.w - 2.0f * padX, row.h}, render::HAlign::Left,
                                render::VAlign::Middle, style);
    }
    b.popClip();

    if (bar)
    {
        const Rect track = scrollbarTrack();
        const Rect thumb = scrollbarThumb();
        const float rr = track.w * 0.5f;
        b.fillRoundedRect(track, rr, t.scrollTrack);
        const bool hot = m_draggingThumb || (isHovered() && context() != nullptr);
        b.fillRoundedRect(thumb.inset(1.0f, 0.0f, 1.0f, 0.0f), rr, hot ? t.scrollThumbHover : t.scrollThumb);
    }
}

} // namespace vo::client::ui
