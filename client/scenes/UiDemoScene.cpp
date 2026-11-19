#include "client/scenes/UiDemoScene.h"

#include "client/platform/Input.h"
#include "client/render/RenderDevice.h"
#include "client/scenes/Backdrop.h"
#include "client/ui/Ui.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace vo::client::scenes {
namespace {

namespace anchor = ui::anchor;

// A round gold-ringed crest with a four-pointed star, generated in code (exercises
// Texture2D::createFromRgba, premultiplication and mip generation).
std::vector<std::uint8_t> makeEmblemPixels(int size)
{
    std::vector<std::uint8_t> px(static_cast<std::size_t>(size * size * 4));
    const float c = static_cast<float>(size) * 0.5f;
    const float outer = c - 2.0f;
    const float ringInner = outer * 0.84f;
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const float dx = static_cast<float>(x) + 0.5f - c;
            const float dy = static_cast<float>(y) + 0.5f - c;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float alpha = std::clamp(outer - r + 0.5f, 0.0f, 1.0f);
            float cr = 0.0f;
            float cg = 0.0f;
            float cb = 0.0f;
            // Inner field: dark crimson radial gradient.
            const float t = std::clamp(r / ringInner, 0.0f, 1.0f);
            cr = 0.45f - 0.25f * t;
            cg = 0.08f - 0.04f * t;
            cb = 0.10f - 0.05f * t;
            // Four-pointed star (astroid) in gold.
            const float nx = std::fabs(dx) / (ringInner * 0.8f);
            const float ny = std::fabs(dy) / (ringInner * 0.8f);
            const float star = std::sqrt(nx) + std::sqrt(ny);
            const float starCov = std::clamp((1.0f - star) * ringInner * 0.35f + 0.5f, 0.0f, 1.0f);
            const float shade = 0.75f + 0.25f * (-dy / c);
            cr = cr + (0.95f * shade - cr) * starCov;
            cg = cg + (0.76f * shade - cg) * starCov;
            cb = cb + (0.30f * shade - cb) * starCov;
            // Gold ring with a dark separating line.
            const float ringCov = std::clamp(r - ringInner + 0.5f, 0.0f, 1.0f);
            const float ringShade = 0.7f + 0.3f * (-dy / c);
            cr = cr + (0.85f * ringShade - cr) * ringCov;
            cg = cg + (0.66f * ringShade - cg) * ringCov;
            cb = cb + (0.25f * ringShade - cb) * ringCov;
            const float line = std::clamp(1.0f - std::fabs(r - ringInner) / 1.2f, 0.0f, 1.0f);
            cr *= 1.0f - 0.7f * line;
            cg *= 1.0f - 0.7f * line;
            cb *= 1.0f - 0.7f * line;

            std::uint8_t* p = px.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) +
                                           static_cast<std::size_t>(x)) *
                                              4;
            auto toByte = [](float v)
            { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            p[0] = toByte(cr);
            p[1] = toByte(cg);
            p[2] = toByte(cb);
            p[3] = toByte(alpha);
        }
    }
    return px;
}

constexpr const char* kTurkishParagraph =
    "Pijamalı hasta yağız şoföre çabucak güvendi. PİJAMALI HASTA YAĞIZ ŞOFÖRE ÇABUCAK GÜVENDİ. "
    "Küçük harfler: ç ğ ı i ö ş ü — büyük harfler: Ç Ğ I İ Ö Ş Ü. Noktalama: “tırnak” ‘tek’ … € ₺. "
    "Bilinmeyen/geçersiz: \xF0\x9F\x90\x89 \xFF \xC3 sonu.";

} // namespace

UiDemoScene::UiDemoScene() = default;
UiDemoScene::~UiDemoScene() = default;

void UiDemoScene::onEnter(SceneContext& ctx)
{
    m_ctx = &ctx;
    std::string error;
    const auto pixels = makeEmblemPixels(128);
    render::Texture2D::Options opts;
    opts.debugName = "uidemo.emblem";
    if (!m_emblem.createFromRgba(ctx.device.device(), ctx.device.context(), 128, 128, pixels.data(), opts,
                                 error))
        VO_LOG_ERROR("uidemo: emblem texture: {}", error);
    else
        VO_LOG_INFO("uidemo: procedural emblem 128x128, {} mip levels", m_emblem.mipLevels());

    if (const auto path = vo::core::locateDataFile("assets_src/ui/demo_emblem.png"))
    {
        render::Texture2D::Options diskOpts;
        diskOpts.debugName = "uidemo.disk_icon";
        if (m_diskIcon.loadFromFile(ctx.device.device(), ctx.device.context(), *path, diskOpts, error))
            VO_LOG_INFO("uidemo: loaded {} ({}x{}, {} mips)", vo::core::pathToUtf8(*path), m_diskIcon.width(),
                        m_diskIcon.height(), m_diskIcon.mipLevels());
        else
            VO_LOG_WARN("uidemo: {}", error);
    }
    else
    {
        VO_LOG_WARN("uidemo: assets_src/ui/demo_emblem.png not found");
    }

    m_ui = std::make_unique<ui::UiContext>(ctx.fonts, ctx.theme);
    m_ui->setClipboard(&ctx.clipboard);
    build();
}

void UiDemoScene::build()
{
    const ui::Theme& theme = m_ctx->theme;
    ui::Widget& root = m_ui->root();

    // ---- Header ----
    auto& header = root.add<ui::Label>("Ortaocak · Arayüz Deneme Sahnesi", ui::TextRole::Large);
    header.setWeight(render::FontWeight::Bold)
        .setColor(theme.textTitle)
        .setShadow(true)
        .setAlign(render::HAlign::Center);
    header.setAnchors(anchor::kFillX | anchor::kTop)
        .setMargins({200.0f, 18.0f, 200.0f, 0.0f})
        .setSize(0.0f, 32.0f);

    m_fps = &root.add<ui::Label>("FPS: --", ui::TextRole::Small);
    m_fps->setColor(theme.textDim).setAlign(render::HAlign::Right);
    m_fps->setAnchors(anchor::kTopRight).setMargins({0.0f, 22.0f, 24.0f, 0.0f}).setSize(380.0f, 20.0f);

    // ---- Login panel (left) ----
    auto& login = root.add<ui::Panel>(ui::Panel::Style::Window, "Giriş");
    login.setAnchors(anchor::kLeft | anchor::kCenterY)
        .setMargins({60.0f, 0.0f, 0.0f, 0.0f})
        .setSize(370.0f, 336.0f);
    login.setPadding(Insets::all(18.0f));
    auto& form = login.add<ui::StackPanel>(ui::Orientation::Vertical, 6.0f);
    form.setAnchors(anchor::kFill);

    form.add<ui::Label>("Kullanıcı adı", ui::TextRole::Small).setColor(theme.textDim).setSize(0.0f, 18.0f);
    m_username = &form.add<ui::TextBox>();
    m_username->setPlaceholder("kullanıcı adınız")
        .setMaxLength(48); // long limit: lets the demo show scrolling
    m_username->setName("username");
    m_username->setSize(0.0f, theme.controlHeight);

    form.add<ui::Label>("Şifre", ui::TextRole::Small).setColor(theme.textDim).setSize(0.0f, 18.0f);
    m_password = &form.add<ui::TextBox>();
    m_password->setPassword(true)
        .setPlaceholder("şifreniz")
        .setMaxLength(32)
        .setOnSubmit([this](const std::string&) { submitLogin(); });
    m_password->setName("password");
    m_password->setSize(0.0f, theme.controlHeight);

    m_remember = &form.add<ui::Checkbox>("Beni hatırla");
    m_remember->setOnChange([this](bool on)
                            { setStatus(on ? "Beni hatırla: açık" : "Beni hatırla: kapalı"); });
    m_remember->setSize(0.0f, 28.0f);

    auto& buttons = form.add<ui::StackPanel>(ui::Orientation::Horizontal, 10.0f);
    buttons.setSize(0.0f, theme.controlHeight + 2.0f);
    buttons.add<ui::Button>("Giriş", [this]() { submitLogin(); });
    buttons.add<ui::Button>("Çıkış", [this]() { m_ctx->requestQuit(); });

    m_status = &form.add<ui::Label>("Durum: hazır", ui::TextRole::Small);
    m_status->setColor(theme.textDim).setWrap(true).setAlign(render::HAlign::Left, render::VAlign::Top);
    m_status->setSize(0.0f, 0.0f); // flexible: takes the remaining height

    // ---- Character list (right) ----
    auto& listPanel = root.add<ui::Panel>(ui::Panel::Style::Window, "Karakterler");
    listPanel.setAnchors(anchor::kTopRight).setMargins({0.0f, 92.0f, 60.0f, 0.0f}).setSize(340.0f, 368.0f);
    listPanel.setPadding(Insets::all(16.0f));
    m_list = &listPanel.add<ui::ListBox>();
    m_list->setItems({"Ayşe Yılmaz", "Çağrı Öztürk", "Gülşen Doğan", "İsmail Çelik", "Şükrü Aydın",
                      "Özge Kılıç", "Ümit Şahin", "Gökhan Ertuğrul", "Ilgaz Dağlı", "Büşra Güneş",
                      "Oğuz Kağan", "Ece Çınar"});
    m_list->setOnSelect(
        [this](int index)
        {
            if (index >= 0)
                setStatus(
                    std::format("Seçilen karakter: {}", m_list->items()[static_cast<std::size_t>(index)]));
        });
    m_list->setOnActivate(
        [this](int index)
        {
            if (index >= 0)
                setStatus(std::format("Oyuna giriş: {}", m_list->items()[static_cast<std::size_t>(index)]));
        });
    m_list->setName("characters");
    m_list->setAnchors(anchor::kFill).setMargins({0.0f, 0.0f, 0.0f, theme.controlHeight + 10.0f});
    auto& open = listPanel.add<ui::Button>("Mesaj kutusunu aç", [this]() { openMessageBox(); });
    open.setAnchors(anchor::kFillX | anchor::kBottom).setSize(0.0f, theme.controlHeight);

    // ---- Centre column: images ----
    auto& emblem = root.add<ui::ImageWidget>(&m_emblem);
    emblem.setAnchors(anchor::kCenterX | anchor::kTop)
        .setMargins({0.0f, 104.0f, 0.0f, 0.0f})
        .setSize(112.0f, 112.0f);
    // Outlined text drawn over the image (TextStyle::outline).
    auto& emblemText = root.add<ui::Label>("Arma", ui::TextRole::Large);
    emblemText.setWeight(render::FontWeight::Bold)
        .setColor(Color::hex(0xFFFFFF))
        .setOutline(true)
        .setAlign(render::HAlign::Center);
    emblemText.setAnchors(anchor::kCenterX | anchor::kTop)
        .setMargins({0.0f, 186.0f, 0.0f, 0.0f})
        .setSize(112.0f, 26.0f);
    auto& emblemCaption = root.add<ui::Label>("Prosedürel doku", ui::TextRole::Small);
    emblemCaption.setColor(theme.textDim).setAlign(render::HAlign::Center);
    emblemCaption.setAnchors(anchor::kCenterX | anchor::kTop)
        .setMargins({0.0f, 222.0f, 0.0f, 0.0f})
        .setSize(240.0f, 18.0f);
    if (m_diskIcon.valid())
    {
        auto& icon = root.add<ui::ImageWidget>(&m_diskIcon);
        icon.setAnchors(anchor::kCenterX | anchor::kTop)
            .setMargins({0.0f, 256.0f, 0.0f, 0.0f})
            .setSize(64.0f, 64.0f);
        auto& iconCaption = root.add<ui::Label>("PNG dosyası", ui::TextRole::Small);
        iconCaption.setColor(theme.textDim).setAlign(render::HAlign::Center);
        iconCaption.setAnchors(anchor::kCenterX | anchor::kTop)
            .setMargins({0.0f, 324.0f, 0.0f, 0.0f})
            .setSize(240.0f, 18.0f);
    }

    // ---- Paragraph (bottom) ----
    auto& paragraphPanel = root.add<ui::Panel>(ui::Panel::Style::Inset);
    paragraphPanel.setAnchors(anchor::kFillX | anchor::kBottom)
        .setMargins({60.0f, 0.0f, 60.0f, 28.0f})
        .setSize(0.0f, 104.0f);
    paragraphPanel.setPadding({16.0f, 10.0f, 16.0f, 10.0f});
    auto& paragraph = paragraphPanel.add<ui::Label>(kTurkishParagraph);
    paragraph.setWrap(true).setAlign(render::HAlign::Left, render::VAlign::Top);
    paragraph.setAnchors(anchor::kFill);

    m_ui->setFocus(m_username);
}

void UiDemoScene::setStatus(std::string text)
{
    VO_LOG_INFO("uidemo: {}", text);
    m_status->setText(text);
}

void UiDemoScene::submitLogin()
{
    const std::size_t pwLen = m_password->model().codePointCount();
    setStatus(std::format("Giriş denemesi: kullanıcı '{}', şifre {} karakter, beni hatırla: {}",
                          m_username->text(), pwLen, m_remember->isChecked() ? "evet" : "hayır"));
}

void UiDemoScene::openMessageBox()
{
    ui::MessageBoxDesc desc;
    desc.title = "Bilgi";
    desc.text = "Bu bir deneme mesajıdır. Arkadaki pencereler, bu kutu kapanana kadar girdi almaz. "
                "Devam etmek istiyor musunuz?";
    desc.buttons = {"Tamam", "İptal"};
    desc.onClose = [this](int index)
    { setStatus(std::format("Mesaj kutusu kapandı: {}", index == 0 ? "Tamam" : "İptal")); };
    ui::MessageBox::show(*m_ui, std::move(desc));
}

void UiDemoScene::onExit()
{
    m_ui.reset();
    m_emblem.reset();
    m_diskIcon.reset();
    m_ctx = nullptr;
}

void UiDemoScene::update(double dt)
{
    for (const platform::InputEvent& e : m_ctx->input.events())
    {
        const bool consumed = m_ui->handleEvent(e);
        if (!consumed && e.type == platform::EventType::KeyDown && e.key == platform::keys::kEscape)
            m_ctx->requestQuit();
    }

    // Re-format only when a shown value changes (no per-frame string churn).
    const double fps = std::round(m_ctx->fps);
    if (fps != m_fpsShown || m_ctx->lastBatchStats.drawCalls != m_drawCallsShown ||
        m_ctx->uiScale != m_scaleShown)
    {
        m_fpsShown = fps;
        m_drawCallsShown = m_ctx->lastBatchStats.drawCalls;
        m_scaleShown = m_ctx->uiScale;
        const std::string fpsText = fps > 0.0 ? std::format("{:.0f}", fps) : std::string("--");
        m_fps->setText(std::format("FPS: {} · çizim çağrısı: {} · ölçek: {:.2f}", fpsText, m_drawCallsShown,
                                   m_scaleShown));
    }
    m_ui->update(dt);
}

void UiDemoScene::render()
{
    auto& batch = m_ctx->batch;
    batch.begin(static_cast<std::uint32_t>(m_ctx->viewportWidth),
                static_cast<std::uint32_t>(m_ctx->viewportHeight));
    drawBackdrop(batch, m_ctx->theme, m_ctx->viewportWidth, m_ctx->viewportHeight, m_ctx->uiScale);
    m_ui->draw(batch);
    batch.end();
}

void UiDemoScene::onResize(float width, float height, float uiScale)
{
    if (m_ui)
        m_ui->setViewport(width, height, uiScale);
}

} // namespace vo::client::scenes
