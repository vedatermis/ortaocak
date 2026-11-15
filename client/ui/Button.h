// Push button: normal / hover / pressed / disabled / focused visuals, click callback on mouse
// release inside the button, Enter or Space activates it while focused.
#pragma once

#include "client/ui/Widget.h"

#include <functional>
#include <string>

namespace vo::client::ui {

class Button : public Widget
{
public:
    explicit Button(std::string text = {}, std::function<void()> onClick = {});

    Button& setText(std::string text);
    Button& setOnClick(std::function<void()> onClick);
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }
    [[nodiscard]] bool isPressed() const noexcept { return m_pressed && isHovered(); }

    // Invokes the click callback as if the user clicked (no-op when disabled).
    void click();

    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    bool onKeyDown(const platform::InputEvent& e) override;
    void onCaptureLost() override { m_pressed = false; }
    void drawSelf(DrawContext& ctx) override;

private:
    std::string m_text;
    std::function<void()> m_onClick;
    bool m_pressed = false;
    double m_flashUntil = 0.0; // keyboard activation shows the pressed look briefly
};

} // namespace vo::client::ui
