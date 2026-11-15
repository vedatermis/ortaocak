// Default scene placeholder: title + "waiting for the network layer" note. The real login screen
// (M4) replaces the body of this scene using the ui toolkit.
#pragma once

#include "client/scenes/Scene.h"

#include <memory>

namespace vo::client::ui {
class UiContext;
}

namespace vo::client::scenes {

class LoginScene final : public Scene
{
public:
    LoginScene();
    ~LoginScene() override;

    [[nodiscard]] const char* name() const noexcept override { return "login"; }
    void onEnter(SceneContext& ctx) override;
    void onExit() override;
    void update(double dt) override;
    void render() override;
    void onResize(float width, float height, float uiScale) override;

private:
    SceneContext* m_ctx = nullptr;
    std::unique_ptr<ui::UiContext> m_ui;
};

} // namespace vo::client::scenes
