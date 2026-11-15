#include "client/ui/UiContext.h"

#include "client/render/Font.h"
#include "client/render/SpriteBatch.h"
#include "client/ui/DrawContext.h"

#include <algorithm>

namespace vo::client::ui {

using platform::EventType;
using platform::InputEvent;
using platform::MouseButton;

UiContext::UiContext(render::FontLibrary& fonts, const Theme& theme)
    : m_fonts(fonts), m_theme(theme), m_root(std::make_unique<Widget>())
{
    m_root->attach(this);
    m_root->setMouseTransparent(true); // clicks on empty screen space are not consumed
    m_scratch.reserve(64);
}

UiContext::~UiContext()
{
    m_focus = m_hover = m_capture = nullptr;
    m_modals.clear();
    m_root.reset();
}

void UiContext::setViewport(float width, float height, float scale)
{
    if (width == m_width && height == m_height && scale == m_scale)
        return;
    m_width = width;
    m_height = height;
    m_scale = scale;
    m_layoutDirty = true;
}

Widget& UiContext::activeLayer() noexcept
{
    return m_modals.empty() ? *m_root : *m_modals.back();
}

void UiContext::layoutLayer(Widget& layer)
{
    const Rect full{0.0f, 0.0f, m_width, m_height};
    const bool changed = !(layer.m_rect == full) || layer.m_layoutScale != m_scale;
    layer.m_rect = full;
    layer.m_layoutScale = m_scale;
    if (changed)
        layer.onRectChanged();
    layer.layoutChildren(m_scale);
}

void UiContext::layoutIfNeeded()
{
    if (!m_layoutDirty)
        return;
    m_layoutDirty = false;
    layoutLayer(*m_root);
    for (auto& m : m_modals)
        layoutLayer(*m);
}

void UiContext::setHover(Widget* widget)
{
    if (widget == m_hover)
        return;
    Widget* old = m_hover;
    m_hover = widget;
    if (old != nullptr)
        old->onMouseLeave();
    if (widget != nullptr)
        widget->onMouseEnter();
}

void UiContext::updateHover(float x, float y)
{
    Widget* hit = activeLayer().hitTest(x, y);
    if (m_capture != nullptr)
        hit = (hit == m_capture) ? m_capture : nullptr; // while captured only the captor can be hot
    setHover(hit);
}

void UiContext::releaseCapture()
{
    Widget* old = m_capture;
    m_capture = nullptr;
    if (old != nullptr)
        old->onCaptureLost();
}

void UiContext::setFocus(Widget* widget)
{
    if (widget != nullptr && (!widget->isFocusable() || !widget->isEnabled() || !widget->isVisible()))
        widget = nullptr;
    if (widget == m_focus)
        return;
    Widget* old = m_focus;
    m_focus = widget;
    if (old != nullptr)
        old->onFocusLost();
    if (widget != nullptr)
        widget->onFocusGained();
}

void UiContext::collectFocusable(Widget& w, std::vector<Widget*>& out)
{
    if (!w.isVisible() || !w.isEnabledSelf())
        return;
    if (w.isFocusable())
        out.push_back(&w);
    for (auto& c : w.m_children)
        collectFocusable(*c, out);
}

void UiContext::focusNext(bool reverse)
{
    m_scratch.clear();
    collectFocusable(activeLayer(), m_scratch);
    if (m_scratch.empty())
        return;
    const auto it = std::find(m_scratch.begin(), m_scratch.end(), m_focus);
    std::size_t index = 0;
    if (it == m_scratch.end())
        index = reverse ? m_scratch.size() - 1 : 0;
    else
    {
        const auto cur = static_cast<std::size_t>(it - m_scratch.begin());
        index = reverse ? (cur + m_scratch.size() - 1) % m_scratch.size() : (cur + 1) % m_scratch.size();
    }
    setFocus(m_scratch[index]);
}

bool UiContext::handleEvent(const InputEvent& e)
{
    layoutIfNeeded();
    bool consumed = false;
    Widget& layer = activeLayer();
    const bool modal = !m_modals.empty();

    switch (e.type)
    {
    case EventType::MouseMove:
    {
        m_mouseX = static_cast<float>(e.x);
        m_mouseY = static_cast<float>(e.y);
        updateHover(m_mouseX, m_mouseY);
        Widget* target = m_capture != nullptr ? m_capture : m_hover;
        if (target != nullptr && target->isEnabled())
            target->onMouseMove(e);
        consumed = target != nullptr || modal;
        break;
    }

    case EventType::MouseDown:
    {
        m_mouseX = static_cast<float>(e.x);
        m_mouseY = static_cast<float>(e.y);
        if (m_capture == nullptr)
            updateHover(m_mouseX, m_mouseY);
        Widget* target = m_capture != nullptr ? m_capture : layer.hitTest(m_mouseX, m_mouseY);
        if (e.button == MouseButton::Left && m_capture == nullptr)
            setFocus(target != nullptr && target->isFocusable() ? target : nullptr);
        if (target != nullptr && target->isEnabled())
        {
            if (m_capture == nullptr)
                m_capture = target;
            target->onMouseDown(e);
        }
        consumed = target != nullptr || modal;
        break;
    }

    case EventType::MouseUp:
    {
        m_mouseX = static_cast<float>(e.x);
        m_mouseY = static_cast<float>(e.y);
        Widget* target = m_capture != nullptr ? m_capture : layer.hitTest(m_mouseX, m_mouseY);
        if (target != nullptr && target->isEnabled())
            target->onMouseUp(e);
        if (m_capture != nullptr)
        {
            m_capture = nullptr; // normal release: no onCaptureLost
        }
        updateHover(m_mouseX, m_mouseY);
        consumed = target != nullptr || modal;
        break;
    }

    case EventType::MouseWheel:
    {
        for (Widget* w = layer.hitTest(static_cast<float>(e.x), static_cast<float>(e.y)); w != nullptr;
             w = w->parent())
        {
            if (w->isEnabled() && w->onMouseWheel(e))
            {
                consumed = true;
                break;
            }
            if (w == &layer)
                break;
        }
        consumed = consumed || modal;
        break;
    }

    case EventType::KeyDown:
    case EventType::KeyUp:
    {
        const bool down = e.type == EventType::KeyDown;
        // Focused widget first, then bubble up to the layer root.
        Widget* start = (m_focus != nullptr && layer.isAncestorOf(*m_focus)) ? m_focus : &layer;
        for (Widget* w = start; w != nullptr; w = w->parent())
        {
            if (w->isEnabled() && (down ? w->onKeyDown(e) : w->onKeyUp(e)))
            {
                consumed = true;
                break;
            }
            if (w == &layer)
                break;
        }
        if (!consumed && down && e.key == platform::keys::kTab &&
            (e.mods & (platform::mods::kCtrl | platform::mods::kAlt)) == 0)
        {
            focusNext(e.shift());
            consumed = true;
        }
        consumed = consumed || modal;
        break;
    }

    case EventType::Char:
    {
        if (m_focus != nullptr && layer.isAncestorOf(*m_focus) && m_focus->isEnabled())
            consumed = m_focus->onChar(e);
        consumed = consumed || modal;
        break;
    }

    case EventType::FocusLost:
        releaseCapture();
        break;
    }

    applyPendingCloses();
    return consumed;
}

void UiContext::update(double dt)
{
    m_time += dt;
    applyPendingCloses();
    layoutIfNeeded();
    m_root->update(dt);
    for (auto& m : m_modals)
        m->update(dt);
    applyPendingCloses();
    layoutIfNeeded();
}

void UiContext::draw(render::SpriteBatch& batch)
{
    layoutIfNeeded();
    DrawContext ctx{batch, m_fonts, m_theme, m_scale, m_time};
    m_root->draw(ctx);
    for (auto& m : m_modals)
        m->draw(ctx);
}

Widget& UiContext::pushModal(std::unique_ptr<Widget> layer)
{
    releaseCapture();
    m_savedFocus.push_back(m_focus);
    setFocus(nullptr);
    setHover(nullptr);
    Widget& ref = *layer;
    ref.attach(this);
    m_modals.push_back(std::move(layer));
    m_layoutDirty = true;
    layoutIfNeeded();
    updateHover(m_mouseX, m_mouseY);
    return ref;
}

void UiContext::closeModal(Widget& layer)
{
    if (std::find(m_pendingClose.begin(), m_pendingClose.end(), &layer) == m_pendingClose.end())
        m_pendingClose.push_back(&layer);
}

void UiContext::applyPendingCloses()
{
    while (!m_pendingClose.empty())
    {
        Widget* layer = m_pendingClose.back();
        m_pendingClose.pop_back();
        const auto it = std::find_if(m_modals.begin(), m_modals.end(),
                                     [&](const std::unique_ptr<Widget>& m) { return m.get() == layer; });
        if (it == m_modals.end())
            continue;
        const auto index = static_cast<std::size_t>(it - m_modals.begin());
        onWidgetDetached(*layer);
        Widget* restore = index < m_savedFocus.size() ? m_savedFocus[index] : nullptr;
        if (index < m_savedFocus.size())
            m_savedFocus.erase(m_savedFocus.begin() + static_cast<std::ptrdiff_t>(index));
        std::unique_ptr<Widget> dead = std::move(*it);
        m_modals.erase(it);
        dead->attach(nullptr);
        dead.reset();
        if (index == m_modals.size()) // closed the top layer: restore what had focus beneath it
            setFocus(restore);
        updateHover(m_mouseX, m_mouseY);
        m_layoutDirty = true;
    }
}

namespace {
Widget* findInTree(Widget& w, std::string_view name) noexcept
{
    if (w.name() == name)
        return &w;
    for (const auto& c : w.children())
    {
        if (Widget* hit = findInTree(*c, name))
            return hit;
    }
    return nullptr;
}
} // namespace

Widget* UiContext::findByName(std::string_view name) const noexcept
{
    for (auto it = m_modals.rbegin(); it != m_modals.rend(); ++it)
    {
        if (Widget* hit = findInTree(**it, name))
            return hit;
    }
    return findInTree(*m_root, name);
}

void UiContext::onWidgetDetached(Widget& widget) noexcept
{
    if (m_hover != nullptr && widget.isAncestorOf(*m_hover))
        m_hover = nullptr;
    if (m_capture != nullptr && widget.isAncestorOf(*m_capture))
        m_capture = nullptr;
    if (m_focus != nullptr && widget.isAncestorOf(*m_focus))
        m_focus = nullptr;
    for (Widget*& saved : m_savedFocus)
    {
        if (saved != nullptr && widget.isAncestorOf(*saved))
            saved = nullptr;
    }
    m_pendingClose.erase(std::remove_if(m_pendingClose.begin(), m_pendingClose.end(),
                                        [&](Widget* w) { return w != &widget && widget.isAncestorOf(*w); }),
                         m_pendingClose.end());
}

void UiContext::onWidgetHidden(Widget& widget) noexcept
{
    if (m_focus != nullptr && widget.isAncestorOf(*m_focus))
        setFocus(nullptr);
    if (m_capture != nullptr && widget.isAncestorOf(*m_capture))
        releaseCapture();
    if (m_hover != nullptr && widget.isAncestorOf(*m_hover))
        setHover(nullptr);
}

} // namespace vo::client::ui
