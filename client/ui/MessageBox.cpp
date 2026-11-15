#include "client/ui/MessageBox.h"

#include "client/ui/Button.h"
#include "client/ui/DrawContext.h"
#include "client/ui/Label.h"
#include "client/ui/Panel.h"
#include "client/ui/StackPanel.h"
#include "client/ui/UiContext.h"

#include <algorithm>
#include <cmath>

namespace vo::client::ui {

namespace {
constexpr float kButtonWidth = 128.0f;
} // namespace

MessageBox::MessageBox(MessageBoxDesc desc) : m_desc(std::move(desc))
{
    if (m_desc.buttons.empty())
        m_desc.buttons.emplace_back("Tamam");
    const int count = static_cast<int>(m_desc.buttons.size());
    if (m_desc.cancelButton < 0 || m_desc.cancelButton >= count)
        m_desc.cancelButton = count - 1;
    m_desc.defaultButton = std::clamp(m_desc.defaultButton, 0, count - 1);

    m_panel = &add<Panel>(Panel::Style::Window, m_desc.title);
    m_panel->setAnchors(anchor::kCenter);
    m_label = &m_panel->add<Label>(m_desc.text);
    m_label->setWrap(true).setAlign(render::HAlign::Center, render::VAlign::Top);
    m_label->setAnchors(anchor::kFillX | anchor::kTop);
    m_row = &m_panel->add<StackPanel>(Orientation::Horizontal, 10.0f);
    m_row->setAnchors(anchor::kCenterX | anchor::kBottom);
    for (int i = 0; i < count; ++i)
    {
        Button& b =
            m_row->add<Button>(m_desc.buttons[static_cast<std::size_t>(i)], [this, i]() { close(i); });
        b.setSize(kButtonWidth, 0.0f);
        m_buttons.push_back(&b);
    }
}

MessageBox& MessageBox::show(UiContext& ui, MessageBoxDesc desc)
{
    auto box = std::make_unique<MessageBox>(std::move(desc));
    MessageBox& ref = *box;
    ui.pushModal(std::move(box));
    ui.setFocus(ref.m_buttons[static_cast<std::size_t>(ref.m_desc.defaultButton)]);
    return ref;
}

void MessageBox::close(int buttonIndex)
{
    if (m_closed)
        return;
    m_closed = true;
    if (m_desc.onClose)
        m_desc.onClose(buttonIndex);
    if (context() != nullptr)
        context()->closeModal(*this);
}

void MessageBox::layoutChildren(float scale)
{
    UiContext* ui = context();
    if (ui == nullptr)
        return;
    const Theme& t = ui->theme();
    const float margin = 24.0f;
    const float width = std::min(m_desc.width, rect().w / scale - 2.0f * margin);
    const float pad = t.padding * 1.4f;
    const float contentW = std::round((width - 2.0f * pad) * scale);
    const float textH = m_label->measureHeight(contentW) / scale;
    const float height = t.titleBarHeight + pad + textH + t.spacing * 2.0f + t.controlHeight + pad;

    m_panel->setPadding({pad, pad, pad, pad});
    m_panel->setSize(width, height);
    m_label->setSize(0.0f, textH + 2.0f);
    const auto n = static_cast<float>(m_buttons.size());
    m_row->setSize(n * kButtonWidth + (n - 1.0f) * 10.0f, t.controlHeight);
    Widget::layoutChildren(scale);
}

void MessageBox::drawSelf(DrawContext& ctx)
{
    ctx.batch.fillRect(rect(), ctx.theme.overlayDim);
}

bool MessageBox::onKeyDown(const platform::InputEvent& e)
{
    if (e.key == platform::keys::kEscape)
    {
        close(m_desc.cancelButton);
        return true;
    }
    if (e.key == platform::keys::kEnter)
    {
        close(m_desc.defaultButton);
        return true;
    }
    return false; // Tab etc. continue to UiContext (focus traversal stays inside the modal)
}

} // namespace vo::client::ui
