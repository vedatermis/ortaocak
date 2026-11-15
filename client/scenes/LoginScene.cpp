#include "client/scenes/LoginScene.h"

#include "client/platform/Input.h"
#include "client/scenes/Backdrop.h"
#include "client/ui/Ui.h"

namespace vo::client::scenes {

LoginScene::LoginScene() = default;
LoginScene::~LoginScene() = default;

void LoginScene::onEnter(SceneContext& ctx)
{
    m_ctx = &ctx;
    m_ui = std::make_unique<ui::UiContext>(ctx.fonts, ctx.theme);
    m_ui->setClipboard(&ctx.clipboard);

    ui::Widget& root = m_ui->root();
    auto& title = root.add<ui::Label>("VedatOnline", ui::TextRole::Heading);
    title.setWeight(render::FontWeight::Bold)
        .setColor(ctx.theme.textTitle)
        .setShadow(true)
        .setAlign(render::HAlign::Center, render::VAlign::Bottom);
    title.setAnchors(ui::anchor::kCenter).setSize(700.0f, 64.0f).setMargins({0.0f, 0.0f, 0.0f, 64.0f});

    auto& note = root.add<ui::Label>("bağlantı katmanı bekleniyor", ui::TextRole::Large);
    note.setColor(ctx.theme.textDim).setAlign(render::HAlign::Center, render::VAlign::Top);
    note.setAnchors(ui::anchor::kCenter).setSize(700.0f, 30.0f).setMargins({0.0f, 56.0f, 0.0f, 0.0f});
}

void LoginScene::onExit()
{
    m_ui.reset();
    m_ctx = nullptr;
}

void LoginScene::update(double dt)
{
    for (const platform::InputEvent& e : m_ctx->input.events())
    {
        const bool consumed = m_ui->handleEvent(e);
        if (!consumed && e.type == platform::EventType::KeyDown && e.key == platform::keys::kEscape)
            m_ctx->requestQuit();
    }
    m_ui->update(dt);
}

void LoginScene::render()
{
    auto& batch = m_ctx->batch;
    batch.begin(static_cast<std::uint32_t>(m_ctx->viewportWidth),
                static_cast<std::uint32_t>(m_ctx->viewportHeight));
    drawBackdrop(batch, m_ctx->theme, m_ctx->viewportWidth, m_ctx->viewportHeight, m_ctx->uiScale);
    m_ui->draw(batch);
    batch.end();
}

void LoginScene::onResize(float width, float height, float uiScale)
{
    if (m_ui)
        m_ui->setViewport(width, height, uiScale);
}

} // namespace vo::client::scenes
