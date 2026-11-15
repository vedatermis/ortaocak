// Scrollable list of selectable text rows. Click selects, double click / Enter activates, mouse
// wheel scrolls, Up/Down/PageUp/PageDown/Home/End move the selection (kept in view). A scrollbar
// appears when the rows overflow; its thumb can be dragged.
#pragma once

#include "client/ui/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace vo::client::ui {

class ListBox : public Widget
{
public:
    ListBox();

    ListBox& setItems(std::vector<std::string> items);
    ListBox& addItem(std::string item);
    ListBox& clearItems();
    [[nodiscard]] const std::vector<std::string>& items() const noexcept { return m_items; }

    // -1 = none. `notify` invokes the select callback.
    ListBox& setSelectedIndex(int index, bool notify = false);
    [[nodiscard]] int selectedIndex() const noexcept { return m_selected; }
    ListBox& setOnSelect(std::function<void(int)> onSelect);
    ListBox& setOnActivate(std::function<void(int)> onActivate);
    ListBox& setRowHeight(float logical);

    void ensureVisible(int index);

    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    bool onMouseWheel(const platform::InputEvent& e) override;
    bool onKeyDown(const platform::InputEvent& e) override;
    void onMouseLeave() override { m_hoverRow = -1; }
    void onCaptureLost() override { m_draggingThumb = false; }
    void drawSelf(DrawContext& ctx) override;

private:
    [[nodiscard]] float rowHeightPx() const;
    [[nodiscard]] Rect innerRect() const;
    [[nodiscard]] float contentHeight() const { return rowHeightPx() * static_cast<float>(m_items.size()); }
    [[nodiscard]] float maxScroll() const;
    [[nodiscard]] bool hasScrollbar() const { return contentHeight() > innerRect().h; }
    [[nodiscard]] Rect scrollbarTrack() const;
    [[nodiscard]] Rect scrollbarThumb() const;
    [[nodiscard]] int rowAt(float y) const;
    void clampScroll();
    void select(int index, bool notify);

    std::vector<std::string> m_items;
    std::function<void(int)> m_onSelect;
    std::function<void(int)> m_onActivate;
    int m_selected = -1;
    int m_hoverRow = -1;
    float m_rowHeight = 0.0f;   // 0 = theme
    float m_scroll = 0.0f;      // physical pixels
    float m_scrollScale = 0.0f; // UI scale m_scroll was computed at
    bool m_draggingThumb = false;
    float m_dragOffset = 0.0f;
};

} // namespace vo::client::ui
