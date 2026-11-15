// Modal confirmation that only enables its confirm button once the user has typed an exact word
// (used to delete a character: the character's name must be typed). Dims the screen like
// ui::MessageBox; Enter confirms when the text matches, Esc cancels.
//
// Widget names (automation): "confirm.input", "confirm.ok", "confirm.cancel", "confirm.text".
#pragma once

#include "client/ui/Widget.h"

#include <functional>
#include <string>

namespace vo::client::ui {
class Button;
class Label;
class TextBox;
class UiContext;
} // namespace vo::client::ui

namespace vo::client::scenes {

struct TypedConfirmDesc
{
    std::string title;
    std::string text;
    std::string expected; // what has to be typed (exact match)
    std::string confirmLabel = "Sil";
    std::string cancelLabel = "Vazgeç";
    std::function<void(bool confirmed)> onClose;
};

class TypedConfirmDialog : public ui::Widget
{
public:
    explicit TypedConfirmDialog(TypedConfirmDesc desc);

    static TypedConfirmDialog& show(ui::UiContext& ui, TypedConfirmDesc desc);

    void close(bool confirmed);
    void drawSelf(ui::DrawContext& ctx) override;
    bool onKeyDown(const platform::InputEvent& e) override;
    bool onMouseDown(const platform::InputEvent&) override { return true; }
    bool onMouseUp(const platform::InputEvent&) override { return true; }
    bool onMouseWheel(const platform::InputEvent&) override { return true; }

private:
    void onTextChanged(const std::string& text);

    TypedConfirmDesc m_desc;
    ui::TextBox* m_input = nullptr;
    ui::Button* m_ok = nullptr;
    ui::Label* m_hint = nullptr;
    bool m_closed = false;
};

} // namespace vo::client::scenes
