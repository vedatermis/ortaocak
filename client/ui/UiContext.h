// Owns a widget tree and routes input to it: hover, keyboard focus, mouse capture, Tab/Shift+Tab
// focus traversal and modal layers (a modal blocks input to everything beneath it). No global
// state: a scene owns its UiContext, and several can coexist.
//
// Per frame: handleEvent() for each queued input event, update(dt), then draw(batch).
#pragma once

#include "client/platform/InputEvent.h"
#include "client/ui/Theme.h"
#include "client/ui/Widget.h"

#include <memory>
#include <vector>

namespace vo::client::render {
class FontLibrary;
class SpriteBatch;
} // namespace vo::client::render

namespace vo::client::ui {

class ClipboardProvider;

class UiContext
{
public:
    UiContext(render::FontLibrary& fonts, const Theme& theme);
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    [[nodiscard]] Widget& root() noexcept { return *m_root; }

    // Viewport in physical pixels; `scale` = UI scale (DPI scale x user factor).
    void setViewport(float width, float height, float scale);
    [[nodiscard]] float scale() const noexcept { return m_scale; }
    [[nodiscard]] float viewportWidth() const noexcept { return m_width; }
    [[nodiscard]] float viewportHeight() const noexcept { return m_height; }

    // Returns true if a widget consumed the event (scenes use this to decide what reaches the game).
    bool handleEvent(const platform::InputEvent& event);
    void update(double dt);
    void draw(render::SpriteBatch& batch);

    // ---- Focus / hover / capture ----
    void setFocus(Widget* widget);
    [[nodiscard]] Widget* focus() const noexcept { return m_focus; }
    [[nodiscard]] Widget* hover() const noexcept { return m_hover; }
    [[nodiscard]] Widget* capture() const noexcept { return m_capture; }
    // Moves focus to the next/previous focusable, visible, enabled widget of the active layer.
    void focusNext(bool reverse);

    // ---- Modal layers ----
    // The layer covers the whole viewport; input goes only to the topmost layer.
    Widget& pushModal(std::unique_ptr<Widget> layer);
    // Deferred to the end of the current event/update so a layer can close itself from a callback.
    void closeModal(Widget& layer);
    [[nodiscard]] bool hasModal() const noexcept { return !m_modals.empty(); }

    // First widget with this name (Widget::setName), searching the topmost modal layer first, then
    // the lower layers and the root. For automation and tests; nullptr if none.
    [[nodiscard]] Widget* findByName(std::string_view name) const noexcept;

    // ---- Services ----
    void setClipboard(ClipboardProvider* clipboard) noexcept { m_clipboard = clipboard; }
    [[nodiscard]] ClipboardProvider* clipboard() const noexcept { return m_clipboard; }
    [[nodiscard]] render::FontLibrary& fonts() const noexcept { return m_fonts; }
    [[nodiscard]] const Theme& theme() const noexcept { return m_theme; }
    [[nodiscard]] double time() const noexcept { return m_time; }

    void markLayoutDirty() noexcept { m_layoutDirty = true; }
    // Lays out now if needed (normally done by update()).
    void layoutIfNeeded();

    // Called by Widget when a subtree is removed / hidden / disabled.
    void onWidgetDetached(Widget& widget) noexcept;
    void onWidgetHidden(Widget& widget) noexcept;

private:
    [[nodiscard]] Widget& activeLayer() noexcept;
    void setHover(Widget* widget);
    void updateHover(float x, float y);
    void releaseCapture();
    void applyPendingCloses();
    void collectFocusable(Widget& w, std::vector<Widget*>& out);
    void layoutLayer(Widget& layer);

    render::FontLibrary& m_fonts;
    const Theme& m_theme;
    ClipboardProvider* m_clipboard = nullptr;
    std::unique_ptr<Widget> m_root;
    std::vector<std::unique_ptr<Widget>> m_modals;
    std::vector<Widget*> m_savedFocus; // focus to restore when the matching modal closes
    std::vector<Widget*> m_pendingClose;
    std::vector<Widget*> m_scratch;
    Widget* m_focus = nullptr;
    Widget* m_hover = nullptr;
    Widget* m_capture = nullptr;
    float m_width = 0.0f;
    float m_height = 0.0f;
    float m_scale = 1.0f;
    float m_mouseX = -1.0f;
    float m_mouseY = -1.0f;
    double m_time = 0.0;
    bool m_layoutDirty = true;
};

} // namespace vo::client::ui
