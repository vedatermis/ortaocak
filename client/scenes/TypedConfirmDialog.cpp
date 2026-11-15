#include "client/scenes/TypedConfirmDialog.h"

#include "client/ui/Ui.h"

namespace vo::client::scenes {

namespace anchor = ui::anchor;

TypedConfirmDialog::TypedConfirmDialog(TypedConfirmDesc desc) : m_desc(std::move(desc))
{
    setName("confirm");
    auto& panel = add<ui::Panel>(ui::Panel::Style::Window, m_desc.title);
    panel.setAnchors(anchor::kCenter).setSize(470.0f, 286.0f);
    panel.setPadding(Insets::all(20.0f));
    panel.setName("confirm.panel");

    auto& stack = panel.add<ui::StackPanel>(ui::Orientation::Vertical, 8.0f);
    stack.setAnchors(anchor::kFill);
    auto& text = stack.add<ui::Label>(m_desc.text);
    text.setWrap(true).setAlign(render::HAlign::Left, render::VAlign::Top);
    text.setSize(0.0f, 84.0f);
    text.setName("confirm.text");

    m_input = &stack.add<ui::TextBox>();
    m_input->setPlaceholder(m_desc.expected)
        .setMaxLength(32)
        .setOnChange([this](const std::string& s) { onTextChanged(s); })
        .setOnSubmit(
            [this](const std::string& s)
            {
                if (s == m_desc.expected)
                    close(true);
            });
    m_input->setSize(0.0f, 34.0f);
    m_input->setName("confirm.input");

    m_hint = &stack.add<ui::Label>("", ui::TextRole::Small);
    m_hint->setSize(0.0f, 18.0f);
    m_hint->setName("confirm.hint");

    auto& row = stack.add<ui::StackPanel>(ui::Orientation::Horizontal, 10.0f);
    row.setSize(0.0f, 36.0f);
    m_ok = &row.add<ui::Button>(m_desc.confirmLabel, [this]() { close(true); });
    m_ok->setName("confirm.ok");
    auto& cancel = row.add<ui::Button>(m_desc.cancelLabel, [this]() { close(false); });
    cancel.setName("confirm.cancel");
    onTextChanged({});
}

TypedConfirmDialog& TypedConfirmDialog::show(ui::UiContext& ui, TypedConfirmDesc desc)
{
    auto dialog = std::make_unique<TypedConfirmDialog>(std::move(desc));
    TypedConfirmDialog& ref = *dialog;
    ui.pushModal(std::move(dialog));
    ui.setFocus(ref.m_input);
    return ref;
}

void TypedConfirmDialog::onTextChanged(const std::string& text)
{
    const bool match = text == m_desc.expected;
    m_ok->setEnabled(match);
    if (text.empty())
    {
        m_hint->setText("Onay için adı tam olarak yazın.");
        m_hint->setColor(Color::hex(0xA39B88));
    }
    else if (match)
    {
        m_hint->setText("Ad eşleşti; silmek için «Sil» düğmesine basın.");
        m_hint->setColor(Color::hex(0x8FD18B));
    }
    else
    {
        m_hint->setText("Ad eşleşmiyor (büyük/küçük harf dahil).");
        m_hint->setColor(Color::hex(0xE58A72));
    }
}

void TypedConfirmDialog::close(bool confirmed)
{
    if (m_closed)
        return;
    if (confirmed && m_input->text() != m_desc.expected)
        return;
    m_closed = true;
    if (m_desc.onClose)
        m_desc.onClose(confirmed);
    if (context() != nullptr)
        context()->closeModal(*this);
}

void TypedConfirmDialog::drawSelf(ui::DrawContext& ctx)
{
    ctx.batch.fillRect(rect(), ctx.theme.overlayDim);
}

bool TypedConfirmDialog::onKeyDown(const platform::InputEvent& e)
{
    if (e.key == platform::keys::kEscape)
    {
        close(false);
        return true;
    }
    return false;
}

} // namespace vo::client::scenes
