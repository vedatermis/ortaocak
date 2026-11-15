// --scene uidemo: shows every toolkit widget (login-style panel, list, wrapped Turkish paragraph,
// checkbox, images, message box, FPS) for visual verification and automation screenshots.
#pragma once

#include "client/render/Texture2D.h"
#include "client/scenes/Scene.h"

#include <memory>
#include <string>

namespace vo::client::ui {
class Button;
class Checkbox;
class Label;
class ListBox;
class TextBox;
class UiContext;
} // namespace vo::client::ui

namespace vo::client::scenes {

class UiDemoScene final : public Scene
{
public:
    UiDemoScene();
    ~UiDemoScene() override;

    [[nodiscard]] const char* name() const noexcept override { return "uidemo"; }
    void onEnter(SceneContext& ctx) override;
    void onExit() override;
    void update(double dt) override;
    void render() override;
    void onResize(float width, float height, float uiScale) override;
    [[nodiscard]] ui::UiContext* automationUi() noexcept override { return m_ui.get(); }

private:
    void build();
    void submitLogin();
    void openMessageBox();
    void setStatus(std::string text);

    SceneContext* m_ctx = nullptr;
    std::unique_ptr<ui::UiContext> m_ui;
    render::Texture2D m_emblem;   // procedural RGBA (createFromRgba + mips)
    render::Texture2D m_diskIcon; // assets_src/ui/demo_emblem.png via stb_image, if present

    ui::TextBox* m_username = nullptr;
    ui::TextBox* m_password = nullptr;
    ui::Checkbox* m_remember = nullptr;
    ui::ListBox* m_list = nullptr;
    ui::Label* m_status = nullptr;
    ui::Label* m_fps = nullptr;
    double m_fpsShown = -1.0;
    std::uint32_t m_drawCallsShown = 0;
    float m_scaleShown = 0.0f;
};

} // namespace vo::client::scenes
