#include "client/scenes/LoginScene.h"

#include "client/net/GameClient.h"
#include "client/scenes/CharSelectScene.h"
#include "client/scenes/SceneWidgets.h"
#include "client/scenes/UserSettings.h"
#include "client/scenes/Validation.h"
#include "client/ui/Ui.h"
#include "shared/core/Log.h"
#include "shared/game/Names.h"
#include "shared/net/Crypto.h"
#include "shared/net/Messages.h"

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;

namespace {
constexpr float kPanelWidth = 380.0f;
constexpr float kPanelHeight = 318.0f;
constexpr float kTitleHeight = 76.0f;
constexpr float kSubtitleHeight = 26.0f;
constexpr float kGroupGap = 18.0f;
constexpr float kGroupHeight = kTitleHeight + kSubtitleHeight + kGroupGap + kPanelHeight;
const Color kErrorText = Color::hex(0xE58A72);
} // namespace

LoginScene::LoginScene(std::optional<net::UserMessage> notice) : m_notice(std::move(notice)) {}
LoginScene::~LoginScene() = default;

void LoginScene::build()
{
    const ui::Theme& t = theme();
    ctx().net.disconnect(); // a fresh start: never carry a session into the login screen
    ui::Widget& root = ui().root();

    // ---- Title + form, centred as one group ----
    auto& group = root.add<ui::Panel>(ui::Panel::Style::None);
    group.setAnchors(anchor::kCenter).setSize(720.0f, kGroupHeight).setMargins({0.0f, 20.0f, 0.0f, 0.0f});

    auto& title = group.add<ui::Label>("VedatOnline", ui::TextRole::Heading);
    title.setFontSize(54.0f)
        .setWeight(render::FontWeight::Bold)
        .setColor(t.textTitle)
        .setShadow(true)
        .setAlign(render::HAlign::Center, render::VAlign::Middle);
    title.setAnchors(anchor::kFillX | anchor::kTop).setSize(0.0f, kTitleHeight);

    auto& subtitle = group.add<ui::Label>("Aydor ve Kargan diyarlarına hoş geldiniz", ui::TextRole::Large);
    subtitle.setColor(t.text).setOutline(true).setAlign(render::HAlign::Center, render::VAlign::Top);
    subtitle.setAnchors(anchor::kFillX | anchor::kTop)
        .setMargins({0.0f, kTitleHeight, 0.0f, 0.0f})
        .setSize(0.0f, kSubtitleHeight);

    auto& panel = group.add<ui::Panel>(ui::Panel::Style::Window, "Hesabına Giriş Yap");
    panel.setAnchors(anchor::kCenterX | anchor::kTop)
        .setMargins({0.0f, kTitleHeight + kSubtitleHeight + kGroupGap, 0.0f, 0.0f})
        .setSize(kPanelWidth, kPanelHeight);
    panel.setPadding(Insets::all(18.0f));
    panel.setName("login.panel");

    auto& form = panel.add<ui::StackPanel>(ui::Orientation::Vertical, 6.0f);
    form.setAnchors(anchor::kFill);

    form.add<ui::Label>("Kullanıcı adı", ui::TextRole::Small).setColor(t.textDim).setSize(0.0f, 18.0f);
    m_username = &form.add<ui::TextBox>();
    m_username->setPlaceholder("kullanıcı adınız")
        .setMaxLength(game::kUsernameMaxLength + 8) // a little slack so the length message can show
        .setOnSubmit(
            [this](const std::string&)
            {
                if (m_password->text().empty())
                    ui().setFocus(m_password);
                else
                    submit();
            });
    m_username->setName("login.username");
    m_username->setSize(0.0f, t.controlHeight);

    form.add<ui::Label>("Şifre", ui::TextRole::Small).setColor(t.textDim).setSize(0.0f, 18.0f);
    m_password = &form.add<ui::TextBox>();
    m_password->setPassword(true)
        .setPlaceholder("şifreniz")
        .setMaxLength(game::kPasswordMaxBytes + 8)
        .setOnSubmit([this](const std::string&) { submit(); });
    m_password->setName("login.password");
    m_password->setSize(0.0f, t.controlHeight);

    m_remember = &form.add<ui::Checkbox>("Beni hatırla (yalnızca kullanıcı adı)");
    m_remember->setName("login.remember");
    m_remember->setSize(0.0f, 28.0f);

    auto& buttons = form.add<ui::StackPanel>(ui::Orientation::Horizontal, 10.0f);
    buttons.setSize(0.0f, t.controlHeight + 4.0f);
    m_login = &buttons.add<ui::Button>("Giriş", [this]() { submit(); });
    m_login->setName("login.submit");
    m_quit = &buttons.add<ui::Button>("Çıkış", [this]() { ctx().requestQuit(); });
    m_quit->setName("login.quit");

    auto& statusRow = form.add<ui::Widget>();
    statusRow.setSize(0.0f, 0.0f); // flexible: the rest of the panel
    m_spinner = &statusRow.add<Spinner>();
    m_spinner->setAnchors(anchor::kLeft | anchor::kTop).setMargins({0.0f, 6.0f, 0.0f, 0.0f}).setSize(22.0f, 22.0f);
    m_spinner->setVisible(false);
    m_status = &statusRow.add<ui::Label>("", ui::TextRole::Small);
    m_status->setWrap(true).setAlign(render::HAlign::Left, render::VAlign::Top).setColor(t.textDim);
    m_status->setAnchors(anchor::kFill).setMargins({0.0f, 8.0f, 0.0f, 0.0f});
    m_status->setName("login.status");

    // ---- Footer ----
    auto& version = root.add<ui::Label>(ctx().app.versionLabel, ui::TextRole::Small);
    version.setColor(t.textDim).setAlign(render::HAlign::Left);
    version.setAnchors(anchor::kBottomLeft).setMargins({24.0f, 0.0f, 0.0f, 14.0f}).setSize(360.0f, 20.0f);
    auto& server = root.add<ui::Label>("Sunucu: " + ctx().app.serverLabel, ui::TextRole::Small);
    server.setColor(t.textDim).setAlign(render::HAlign::Right);
    server.setAnchors(anchor::kBottomRight).setMargins({0.0f, 0.0f, 24.0f, 14.0f}).setSize(360.0f, 20.0f);

    // ---- Remembered username ----
    UserSettings settings;
    std::string error;
    if (!loadUserSettings(ctx().app.settingsFile, settings, error))
        VO_LOG_WARN("login: user settings: {}", error);
    m_remember->setChecked(settings.rememberUsername);
    if (!settings.username.empty())
    {
        m_username->setText(settings.username);
        ui().setFocus(m_password);
    }
    else
    {
        ui().setFocus(m_username);
    }

    if (m_notice)
    {
        VO_LOG_INFO("login: notice '{}': {}", m_notice->title, m_notice->text);
        setStatus(m_notice->text, true);
        showMessage(*m_notice);
        m_notice.reset();
    }
}

void LoginScene::extraStates(std::vector<std::string>& out) const
{
    if (m_phase == Phase::Connecting)
        out.emplace_back("connecting");
    else if (m_phase == Phase::LoggingIn)
        out.emplace_back("loggingin");
}

void LoginScene::setStatus(std::string_view text, bool error)
{
    m_status->setText(text);
    m_status->setColor(error ? kErrorText : theme().textDim);
}

void LoginScene::setPhase(Phase phase)
{
    m_phase = phase;
    const bool idle = phase == Phase::Idle;
    m_username->setEnabled(idle);
    m_password->setEnabled(idle);
    m_remember->setEnabled(idle);
    m_login->setEnabled(idle);
    m_spinner->setVisible(!idle);
    m_status->setMargins({idle ? 0.0f : 30.0f, 8.0f, 0.0f, 0.0f});
    if (phase == Phase::Connecting)
        setStatus("Bağlanıyor… (iptal etmek için Esc)");
    else if (phase == Phase::LoggingIn)
        setStatus("Giriş yapılıyor… (iptal etmek için Esc)");
}

void LoginScene::submit()
{
    if (m_phase != Phase::Idle || leaving())
        return;
    const std::string user = m_username->text();
    if (const auto err = validateUsername(user))
    {
        fail({"Giriş", *err}, false, m_username);
        return;
    }
    if (const auto err = validatePassword(m_password->text()))
    {
        fail({"Giriş", *err}, false, m_password);
        return;
    }
    m_pendingUser = user;
    VO_LOG_INFO("login: connecting as '{}'", user);
    setPhase(Phase::Connecting);
    if (!ctx().net.connect())
        fail({"Bağlantı Hatası", "Bağlantı başlatılamadı. Lütfen tekrar deneyin."}, false, m_login);
}

void LoginScene::cancel()
{
    VO_LOG_INFO("login: cancelled by the user");
    ctx().net.disconnect();
    setPhase(Phase::Idle);
    setStatus("Bağlantı iptal edildi.");
    ui().setFocus(m_login);
}

void LoginScene::fail(const net::UserMessage& message, bool clearPassword, ui::Widget* focusAfter)
{
    if (m_phase != Phase::Idle)
        ctx().net.disconnect();
    setPhase(Phase::Idle);
    if (clearPassword)
        m_password->setText("");
    setStatus(message.text, true);
    // Focus first: the message box restores it when it closes.
    ui().setFocus(focusAfter);
    showMessage(message);
}

void LoginScene::onNetEvent(const net::NetEvent& e)
{
    switch (e.type)
    {
    case net::NetEventType::Connected:
    {
        if (m_phase != Phase::Connecting)
            return;
        vn::LoginReq req;
        req.username = m_pendingUser;
        req.password = m_password->text();
        const bool sent = ctx().net.request(req, vn::Opcode::LoginRes);
        vn::secureZero(req.password.data(), req.password.size());
        if (!sent)
        {
            fail({"Bağlantı Hatası", "Giriş isteği gönderilemedi. Lütfen tekrar deneyin."}, false, m_login);
            return;
        }
        setPhase(Phase::LoggingIn);
        return;
    }
    case net::NetEventType::ConnectFailed:
        if (m_phase == Phase::Idle)
            return;
        fail(e.connectFailure == net::ConnectFailure::MissingServerKey ? net::missingServerKeyMessage()
                                                                      : net::connectFailureMessage(e.connectResult),
             false, m_login);
        return;
    case net::NetEventType::Disconnected:
        if (m_phase == Phase::Idle)
            return;
        fail(net::disconnectMessage(e.disconnect), false, m_login);
        return;
    case net::NetEventType::Message:
        break;
    }

    if (!e.is(vn::Opcode::LoginRes) || m_phase != Phase::LoggingIn)
        return;
    vn::LoginRes res;
    if (!vn::decodeMessage(e.payload, res))
    {
        fail(net::connectFailureMessage(vn::ConnectResult::ProtocolError), false, m_login);
        return;
    }
    if (res.result == vn::LoginResult::Ok)
    {
        onLoggedIn(res.accountId);
        return;
    }
    VO_LOG_INFO("login: rejected (result {})", static_cast<int>(res.result));
    const bool wrong = res.result == vn::LoginResult::InvalidCredentials;
    fail(net::loginFailureMessage(res.result), wrong, wrong ? static_cast<ui::Widget*>(m_password) : m_login);
}

void LoginScene::onLoggedIn(std::uint32_t accountId)
{
    VO_LOG_INFO("login: logged in as '{}' (account {})", m_pendingUser, accountId);
    UserSettings settings;
    settings.rememberUsername = m_remember->isChecked();
    if (settings.rememberUsername)
        settings.username = m_pendingUser;
    std::string error;
    if (!saveUserSettings(ctx().app.settingsFile, settings, error))
        VO_LOG_WARN("login: cannot save user settings: {}", error);

    m_password->setText(""); // do not keep the password around
    net::AccountState& account = ctx().net.account();
    account.username = m_pendingUser;
    account.accountId = accountId;
    m_phase = Phase::Idle;
    CharSelectScene::Options opts;
    opts.autoCreateWhenEmpty = true;
    changeScene(std::make_unique<CharSelectScene>(opts));
}

void LoginScene::askQuit()
{
    confirm("Çıkış", "Oyundan çıkmak istiyor musunuz?", "Evet", "Hayır",
            [this](bool yes)
            {
                if (yes)
                    ctx().requestQuit();
            });
}

void LoginScene::onUnhandledKey(const platform::InputEvent& e)
{
    if (e.key == platform::keys::kEscape && !e.repeat)
    {
        if (m_phase != Phase::Idle)
            cancel();
        else
            askQuit();
    }
    else if (e.key == platform::keys::kEnter && !e.repeat && m_phase == Phase::Idle)
    {
        submit();
    }
}

} // namespace vo::client::scenes
