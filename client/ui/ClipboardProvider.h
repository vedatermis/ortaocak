// Clipboard access as seen by the UI (TextBox copy/cut/paste). The client wires the system
// clipboard; automation runs use an in-memory one so scripts never touch the user's clipboard.
#pragma once

#include <string>
#include <string_view>

namespace vo::client::ui {

class ClipboardProvider
{
public:
    virtual ~ClipboardProvider() = default;
    [[nodiscard]] virtual std::string getText() = 0;
    virtual void setText(std::string_view utf8) = 0;
};

class MemoryClipboard final : public ClipboardProvider
{
public:
    [[nodiscard]] std::string getText() override { return m_text; }
    void setText(std::string_view utf8) override { m_text = utf8; }

private:
    std::string m_text;
};

} // namespace vo::client::ui
