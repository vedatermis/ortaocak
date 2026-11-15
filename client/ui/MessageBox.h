// Modal message box: dims the screen, shows a titled panel with wrapped text and 1-2 (or more)
// buttons, and blocks input to everything beneath it until closed. Enter activates the default
// button, Esc the cancel button. The callback receives the index of the button (or the cancel
// index for Esc).
//
//   ui::MessageBox::show(ui, {.title = "Bilgi", .text = "...", .buttons = {"Tamam", "İptal"},
//                             .onClose = [](int i) { ... }});
#pragma once

#include "client/ui/Widget.h"

// <Windows.h> defines MessageBox as MessageBoxW. Include it here first (its include guard keeps it
// from coming back later in the TU) and drop the macro so ui::MessageBox means the same class in
// every translation unit. The Win32 function stays reachable as ::MessageBoxW.
#include <Windows.h>
#undef MessageBox

#include <functional>
#include <string>
#include <vector>

namespace vo::client::ui {

class Button;
class Label;
class Panel;
class StackPanel;

struct MessageBoxDesc
{
    std::string title;
    std::string text;
    std::vector<std::string> buttons{"Tamam"};
    std::function<void(int)> onClose;
    int defaultButton = 0;
    int cancelButton = -1; // -1: the last button
    float width = 440.0f;  // logical
};

class MessageBox : public Widget
{
public:
    explicit MessageBox(MessageBoxDesc desc);

    // Pushes a new message box as a modal layer of `ui` and focuses its default button.
    static MessageBox& show(UiContext& ui, MessageBoxDesc desc);

    // Closes with the given button index (invokes the callback once).
    void close(int buttonIndex);

    void layoutChildren(float scale) override;
    void drawSelf(DrawContext& ctx) override;
    bool onKeyDown(const platform::InputEvent& e) override;
    bool onMouseDown(const platform::InputEvent&) override { return true; }
    bool onMouseUp(const platform::InputEvent&) override { return true; }
    bool onMouseWheel(const platform::InputEvent&) override { return true; }

private:
    MessageBoxDesc m_desc;
    Panel* m_panel = nullptr;
    Label* m_label = nullptr;
    StackPanel* m_row = nullptr;
    std::vector<Button*> m_buttons;
    bool m_closed = false;
};

} // namespace vo::client::ui
