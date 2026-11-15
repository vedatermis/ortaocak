#include "client/ui/Widget.h"

#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"

#include <algorithm>

namespace vo::client::ui {

Widget::~Widget() = default;

Widget& Widget::addChild(std::unique_ptr<Widget> child)
{
    Widget& ref = *child;
    ref.m_parent = this;
    ref.attach(m_context);
    m_children.push_back(std::move(child));
    markLayoutDirty();
    return ref;
}

std::unique_ptr<Widget> Widget::removeChild(Widget& child)
{
    const auto it = std::find_if(m_children.begin(), m_children.end(),
                                 [&](const std::unique_ptr<Widget>& c) { return c.get() == &child; });
    if (it == m_children.end())
        return nullptr;
    if (m_context != nullptr)
        m_context->onWidgetDetached(child);
    std::unique_ptr<Widget> out = std::move(*it);
    m_children.erase(it);
    out->m_parent = nullptr;
    out->attach(nullptr);
    markLayoutDirty();
    return out;
}

void Widget::clearChildren()
{
    while (!m_children.empty())
        removeChild(*m_children.back());
}

bool Widget::isAncestorOf(const Widget& other) const noexcept
{
    for (const Widget* w = &other; w != nullptr; w = w->m_parent)
    {
        if (w == this)
            return true;
    }
    return false;
}

void Widget::attach(UiContext* context) noexcept
{
    m_context = context;
    for (auto& c : m_children)
        c->attach(context);
}

void Widget::markLayoutDirty() noexcept
{
    if (m_context != nullptr)
        m_context->markLayoutDirty();
}

Widget& Widget::setLayout(const LayoutSpec& spec)
{
    if (m_layout.anchors == spec.anchors && m_layout.margins == spec.margins && m_layout.size == spec.size)
        return *this;
    m_layout = spec;
    markLayoutDirty();
    return *this;
}

Widget& Widget::setAnchors(std::uint8_t anchors)
{
    if (m_layout.anchors == anchors)
        return *this;
    m_layout.anchors = anchors;
    markLayoutDirty();
    return *this;
}

Widget& Widget::setMargins(const Insets& margins)
{
    if (m_layout.margins == margins)
        return *this;
    m_layout.margins = margins;
    markLayoutDirty();
    return *this;
}

Widget& Widget::setSize(float width, float height)
{
    if (m_layout.size == Vec2{width, height})
        return *this;
    m_layout.size = {width, height};
    markLayoutDirty();
    return *this;
}

Widget& Widget::setPadding(const Insets& padding)
{
    if (m_padding == padding)
        return *this;
    m_padding = padding;
    markLayoutDirty();
    return *this;
}

Rect Widget::contentRect() const
{
    const Insets p = m_padding.scaled(m_layoutScale);
    return m_rect.inset(std::round(p.left), std::round(p.top), std::round(p.right), std::round(p.bottom));
}

Widget& Widget::setVisible(bool visible)
{
    if (m_visible != visible)
    {
        m_visible = visible;
        if (!visible && m_context != nullptr)
            m_context->onWidgetHidden(*this);
        markLayoutDirty();
    }
    return *this;
}

Widget& Widget::setEnabled(bool enabled)
{
    if (m_enabled != enabled)
    {
        m_enabled = enabled;
        if (!enabled && m_context != nullptr)
            m_context->onWidgetHidden(*this);
    }
    return *this;
}

Widget& Widget::setFocusable(bool focusable) noexcept
{
    m_focusable = focusable;
    return *this;
}

Widget& Widget::setName(std::string name)
{
    m_name = std::move(name);
    return *this;
}

bool Widget::isEnabled() const noexcept
{
    for (const Widget* w = this; w != nullptr; w = w->m_parent)
    {
        if (!w->m_enabled)
            return false;
    }
    return true;
}

bool Widget::isHovered() const noexcept
{
    return m_context != nullptr && m_context->hover() == this;
}

bool Widget::isFocused() const noexcept
{
    return m_context != nullptr && m_context->focus() == this;
}

bool Widget::hasCapture() const noexcept
{
    return m_context != nullptr && m_context->capture() == this;
}

Widget* Widget::hitTest(float x, float y)
{
    if (!m_visible || !m_rect.contains(x, y))
        return nullptr;
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
    {
        if (Widget* hit = (*it)->hitTest(x, y))
            return hit;
    }
    return m_mouseTransparent ? nullptr : this;
}

void Widget::placeChild(Widget& child, const Rect& rect, float scale)
{
    const bool changed = !(child.m_rect == rect) || child.m_layoutScale != scale;
    child.m_rect = rect;
    child.m_layoutScale = scale;
    if (changed)
        child.onRectChanged();
    child.layoutChildren(scale);
}

void Widget::layoutChildren(float scale)
{
    m_layoutScale = scale;
    const Rect content = contentRect();
    for (auto& c : m_children)
    {
        if (!c->m_visible)
            continue;
        placeChild(*c, computeAnchoredRect(content, c->m_layout, scale), scale);
    }
}

void Widget::draw(DrawContext& ctx)
{
    if (!m_visible)
        return;
    drawSelf(ctx);
    drawChildren(ctx);
}

void Widget::drawChildren(DrawContext& ctx)
{
    for (auto& c : m_children)
        c->draw(ctx);
}

void Widget::update(double dt)
{
    for (auto& c : m_children)
    {
        if (c->m_visible)
            c->update(dt);
    }
}

} // namespace vo::client::ui
