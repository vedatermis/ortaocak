// Single-line text input built on TextEditModel.
//
// Caret with blink, selection by mouse drag / double click (word) and Shift+arrows, Home/End,
// Ctrl+Left/Right (words), Ctrl+A/C/X/V, Backspace/Delete (Ctrl: whole word) on code point
// boundaries, max length, password mode (masks with bullets, disables copy/cut), horizontal
// scrolling that keeps the caret visible, placeholder text, Enter -> submit callback.
// Tab/Shift+Tab are left to UiContext for focus traversal.
#pragma once

#include "client/ui/TextEditModel.h"
#include "client/ui/Widget.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vo::client::ui {

class TextBox : public Widget
{
public:
    TextBox();

    TextBox& setText(std::string_view text);
    TextBox& setPlaceholder(std::string text);
    TextBox& setPassword(bool password);
    TextBox& setMaxLength(std::size_t codePoints);
    TextBox& setOnSubmit(std::function<void(const std::string&)> onSubmit);
    TextBox& setOnChange(std::function<void(const std::string&)> onChange);

    [[nodiscard]] const std::string& text() const noexcept { return m_model.text(); }
    [[nodiscard]] bool isPassword() const noexcept { return m_password; }
    [[nodiscard]] const TextEditModel& model() const noexcept { return m_model; }
    TextEditModel& model() noexcept { return m_model; }

    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    bool onKeyDown(const platform::InputEvent& e) override;
    bool onChar(const platform::InputEvent& e) override;
    void onFocusGained() override;
    void onFocusLost() override;
    void onCaptureLost() override { m_dragging = false; }
    void drawSelf(DrawContext& ctx) override;

private:
    void touch() noexcept; // restart caret blink
    void changed();
    void refreshStops(DrawContext& ctx);
    [[nodiscard]] Rect textArea() const;
    [[nodiscard]] std::size_t offsetAtX(float x) const;
    [[nodiscard]] std::size_t stopIndexOf(std::size_t byteOffset) const;
    [[nodiscard]] const std::string& displayText() const noexcept
    {
        return m_password ? m_mask : m_model.text();
    }

    TextEditModel m_model;
    std::string m_placeholder;
    std::string m_mask; // bullets, password mode
    std::function<void(const std::string&)> m_onSubmit;
    std::function<void(const std::string&)> m_onChange;
    bool m_password = false;
    bool m_dragging = false;
    double m_lastActivity = 0.0;
    float m_scroll = 0.0f;

    // Caret stop cache (x per code point boundary of the display text).
    std::vector<float> m_stops;
    std::uint64_t m_stopsRevision = ~std::uint64_t{0};
    int m_stopsFontPx = 0;
    bool m_stopsPassword = false;
};

} // namespace vo::client::ui
