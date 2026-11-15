// Base class of the retained-mode UI toolkit.
//
// A widget owns its children (unique_ptr), has a LayoutSpec (anchors, margins, size in logical
// units) from which its parent computes rect() in physical pixels, plus padding that insets the
// area its own children are laid out in. Visibility, enabled state and focusability are per
// widget; input arrives through UiContext, which handles hover/focus/capture and routes events.
// Event handlers return true when they consumed the event.
#pragma once

#include "client/core/Geometry.h"
#include "client/platform/InputEvent.h"
#include "client/ui/Layout.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vo::client::ui {

class UiContext;
struct DrawContext;

class Widget
{
public:
    Widget() = default;
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // ---- Tree ----
    template <typename T, typename... Args>
    T& add(Args&&... args)
    {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *child;
        addChild(std::move(child));
        return ref;
    }
    Widget& addChild(std::unique_ptr<Widget> child);
    // Detaches and returns the child (UiContext forgets hover/focus/capture inside it).
    std::unique_ptr<Widget> removeChild(Widget& child);
    void clearChildren();
    [[nodiscard]] Widget* parent() const noexcept { return m_parent; }
    [[nodiscard]] const std::vector<std::unique_ptr<Widget>>& children() const noexcept { return m_children; }
    [[nodiscard]] UiContext* context() const noexcept { return m_context; }
    [[nodiscard]] bool isAncestorOf(const Widget& other) const noexcept;

    // ---- Layout (fluent setters) ----
    Widget& setLayout(const LayoutSpec& spec);
    Widget& setAnchors(std::uint8_t anchors);
    Widget& setMargins(const Insets& margins);
    Widget& setSize(float width, float height);
    Widget& setPadding(const Insets& padding);
    [[nodiscard]] const LayoutSpec& layoutSpec() const noexcept { return m_layout; }
    [[nodiscard]] const Insets& padding() const noexcept { return m_padding; }

    // Physical pixels, valid after the context's layout pass.
    [[nodiscard]] const Rect& rect() const noexcept { return m_rect; }
    // Area children are laid out in (rect minus padding, scaled).
    [[nodiscard]] virtual Rect contentRect() const;

    // ---- State ----
    Widget& setVisible(bool visible);
    Widget& setEnabled(bool enabled);
    Widget& setFocusable(bool focusable) noexcept;
    Widget& setName(std::string name);
    [[nodiscard]] bool isVisible() const noexcept { return m_visible; }
    [[nodiscard]] bool isEnabledSelf() const noexcept { return m_enabled; }
    [[nodiscard]] bool isEnabled() const noexcept; // this and every ancestor enabled
    [[nodiscard]] bool isFocusable() const noexcept { return m_focusable; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    // Mouse-transparent widgets (labels, images) let clicks through to what is behind them.
    [[nodiscard]] bool isMouseTransparent() const noexcept { return m_mouseTransparent; }

    [[nodiscard]] bool isHovered() const noexcept;
    [[nodiscard]] bool isFocused() const noexcept;
    [[nodiscard]] bool hasCapture() const noexcept;

    // Deepest visible, non-transparent widget under the point (this included), or nullptr.
    [[nodiscard]] Widget* hitTest(float x, float y);

    // ---- Overridables ----
    // Positions children inside contentRect(); default: anchored layout.
    virtual void layoutChildren(float scale);
    // Called after rect() changed (before children are laid out).
    virtual void onRectChanged() {}
    // Default draws self, then children.
    virtual void draw(DrawContext& ctx);
    virtual void drawSelf(DrawContext&) {}
    virtual void update(double dt);

    virtual bool onMouseDown(const platform::InputEvent&) { return false; }
    virtual bool onMouseUp(const platform::InputEvent&) { return false; }
    virtual bool onMouseMove(const platform::InputEvent&) { return false; }
    virtual bool onMouseWheel(const platform::InputEvent&) { return false; }
    virtual bool onKeyDown(const platform::InputEvent&) { return false; }
    virtual bool onKeyUp(const platform::InputEvent&) { return false; }
    virtual bool onChar(const platform::InputEvent&) { return false; }
    virtual void onMouseEnter() {}
    virtual void onMouseLeave() {}
    virtual void onFocusGained() {}
    virtual void onFocusLost() {}
    virtual void onCaptureLost() {}

protected:
    void setMouseTransparent(bool transparent) noexcept { m_mouseTransparent = transparent; }
    // Containers overriding layoutChildren() call this first so contentRect() uses the right scale.
    void recordLayoutScale(float scale) noexcept { m_layoutScale = scale; }
    [[nodiscard]] float layoutScale() const noexcept { return m_layoutScale; }
    void markLayoutDirty() noexcept;
    void drawChildren(DrawContext& ctx);
    // Sets rect() (calls onRectChanged if it changed). Used by containers' layoutChildren().
    static void placeChild(Widget& child, const Rect& rect, float scale);

private:
    friend class UiContext;
    void attach(UiContext* context) noexcept;

    UiContext* m_context = nullptr;
    Widget* m_parent = nullptr;
    std::vector<std::unique_ptr<Widget>> m_children;
    LayoutSpec m_layout;
    Insets m_padding;
    Rect m_rect;
    std::string m_name;
    bool m_visible = true;
    bool m_enabled = true;
    bool m_focusable = false;
    bool m_mouseTransparent = false;
    float m_layoutScale = 1.0f;
};

} // namespace vo::client::ui
