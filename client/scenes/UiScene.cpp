#include "client/scenes/UiScene.h"

#include "client/net/GameClient.h"
#include "client/platform/Input.h"
#include "client/scenes/Backdrop.h"
#include "client/scenes/LoginScene.h"
#include "client/scenes/SceneManager.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/UiScale.h"
#include "client/ui/Ui.h"

namespace vo::client::scenes {

UiScene::UiScene() = default;
UiScene::~UiScene() = default;

const ui::Theme& UiScene::theme() const noexcept
{
    return m_ctx->theme;
}

void UiScene::onEnter(SceneContext& ctx)
{
    m_ctx = &ctx;
    m_leaving = false;
    m_ui = std::make_unique<ui::UiContext>(ctx.fonts, ctx.theme);
    m_ui->setClipboard(&ctx.clipboard);
    build();
}

void UiScene::onExit()
{
    teardown();
    m_ui.reset();
    m_ctx = nullptr;
}

void UiScene::update(double dt)
{
    if (!m_leaving)
    {
        for (const platform::InputEvent& e : m_ctx->input.events())
        {
            if (onPreviewEvent(e))
                continue;
            const bool consumed = m_ui->handleEvent(e);
            if (!consumed && !m_ui->hasModal())
            {
                onUnhandledEvent(e);
                if (e.type == platform::EventType::KeyDown && !m_leaving)
                    onUnhandledKey(e);
            }
            if (m_leaving)
                break;
        }
    }
    while (!m_leaving && !holdNetEvents())
    {
        std::optional<net::NetEvent> e = m_ctx->net.nextEvent();
        if (!e)
            break;
        onNetEvent(*e);
    }
    if (!m_leaving)
        onFrame(dt);
    m_ui->update(dt);
}

void UiScene::onNetEvent(const net::NetEvent& e)
{
    if (e.type == net::NetEventType::Disconnected)
        returnToLogin(net::disconnectMessage(e.disconnect));
}

void UiScene::render()
{
    auto& batch = m_ctx->batch;
    batch.begin(static_cast<std::uint32_t>(m_ctx->viewportWidth),
                static_cast<std::uint32_t>(m_ctx->viewportHeight));
    drawSceneBackground(batch, m_ctx->theme, m_ctx->resources.background(), m_ctx->viewportWidth,
                        m_ctx->viewportHeight, m_ui->scale());
    m_ui->draw(batch);
    batch.end();
}

void UiScene::onResize(float width, float height, float uiScale)
{
    if (m_ui)
        m_ui->setViewport(width, height, fitUiScale(width, height, sceneUiScale(width, height, uiScale)));
}

void UiScene::automationStates(std::vector<std::string>& out) const
{
    out.emplace_back(name());
    const bool busy = isBusy() || m_ctx->net.hasPendingRequests() ||
                      m_ctx->net.state() == net::GameClient::State::Connecting;
    const bool modal = m_ui && m_ui->hasModal();
    if (busy)
        out.emplace_back("busy");
    if (modal)
        out.emplace_back("modal");
    if (!busy && !modal && !m_leaving)
        out.emplace_back("idle");
    if (m_leaving)
        out.emplace_back("leaving");
    extraStates(out);
}

void UiScene::showMessage(const net::UserMessage& message, std::function<void()> onClose)
{
    ui::MessageBoxDesc desc;
    desc.title = message.title;
    desc.text = message.text;
    desc.buttons = {"Tamam"};
    desc.onClose = [cb = std::move(onClose)](int)
    {
        if (cb)
            cb();
    };
    ui::MessageBox::show(*m_ui, std::move(desc));
}

void UiScene::confirm(std::string title, std::string text, std::string yes, std::string no,
                      std::function<void(bool)> onResult)
{
    ui::MessageBoxDesc desc;
    desc.title = std::move(title);
    desc.text = std::move(text);
    desc.buttons = {std::move(yes), std::move(no)};
    desc.defaultButton = 0;
    desc.cancelButton = 1;
    desc.onClose = [cb = std::move(onResult)](int index)
    {
        if (cb)
            cb(index == 0);
    };
    ui::MessageBox::show(*m_ui, std::move(desc));
}

void UiScene::changeScene(std::unique_ptr<Scene> next)
{
    if (m_leaving)
        return;
    m_leaving = true;
    m_ctx->scenes.requestChange(std::move(next));
}

void UiScene::returnToLogin(std::optional<net::UserMessage> notice)
{
    m_ctx->net.disconnect();
    changeScene(std::make_unique<LoginScene>(std::move(notice)));
}

} // namespace vo::client::scenes
