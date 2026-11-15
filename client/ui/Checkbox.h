// Check box with a text label to its right. Click (box or label) or Space toggles.
#pragma once

#include "client/ui/Widget.h"

#include <functional>
#include <string>

namespace vo::client::ui {

class Checkbox : public Widget
{
public:
    explicit Checkbox(std::string text = {}, bool checked = false);

    Checkbox& setText(std::string text);
    Checkbox& setChecked(bool checked); // does not invoke the callback
    Checkbox& setOnChange(std::function<void(bool)> onChange);
    [[nodiscard]] bool isChecked() const noexcept { return m_checked; }
    void toggle();

    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    bool onKeyDown(const platform::InputEvent& e) override;
    void onCaptureLost() override { m_pressed = false; }
    void drawSelf(DrawContext& ctx) override;

private:
    std::string m_text;
    std::function<void(bool)> m_onChange;
    bool m_checked;
    bool m_pressed = false;
};

} // namespace vo::client::ui
