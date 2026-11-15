// Login screen (default scene): game title, username + password, "Beni hatırla" (remembers the
// username only), "Giriş" / "Çıkış", status line with an animated indicator.
//
// Giriş (or Enter) validates the input with the shared rules, connects (GameClient), sends
// LOGIN_REQ and on success switches to the character select screen. While connecting/logging in
// the inputs are disabled and Esc cancels. Every failure shows a Turkish message box and returns to
// a usable idle state. Esc while idle asks whether to quit. A `notice` passed in (e.g. why the
// connection dropped) is shown as a message box on entry.
#pragma once

#include "client/net/NetText.h"
#include "client/scenes/UiScene.h"

#include <optional>

namespace vo::client::ui {
class Button;
class Checkbox;
class Label;
class TextBox;
class Widget;
} // namespace vo::client::ui

namespace vo::client::scenes {

class Spinner;

class LoginScene final : public UiScene
{
public:
    explicit LoginScene(std::optional<net::UserMessage> notice = std::nullopt);
    ~LoginScene() override;

    [[nodiscard]] const char* name() const noexcept override { return "login"; }

protected:
    void build() override;
    void onUnhandledKey(const platform::InputEvent& e) override;
    void onNetEvent(const net::NetEvent& e) override;
    [[nodiscard]] bool isBusy() const override { return m_phase != Phase::Idle; }
    void extraStates(std::vector<std::string>& out) const override;

private:
    enum class Phase : std::uint8_t
    {
        Idle,
        Connecting,
        LoggingIn,
    };

    void submit();
    void cancel();
    void setPhase(Phase phase);
    void setStatus(std::string_view text, bool error = false);
    void fail(const net::UserMessage& message, bool clearPassword, ui::Widget* focusAfter);
    void onLoggedIn(std::uint32_t accountId);
    void askQuit();

    std::optional<net::UserMessage> m_notice;
    Phase m_phase = Phase::Idle;
    std::string m_pendingUser;

    ui::TextBox* m_username = nullptr;
    ui::TextBox* m_password = nullptr;
    ui::Checkbox* m_remember = nullptr;
    ui::Button* m_login = nullptr;
    ui::Button* m_quit = nullptr;
    ui::Label* m_status = nullptr;
    Spinner* m_spinner = nullptr;
};

} // namespace vo::client::scenes
