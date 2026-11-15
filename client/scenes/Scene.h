// Scene interface and the services a scene can use (SceneContext). One scene is active at a time;
// SceneManager switches between them with deferred transitions.
#pragma once

#include "client/render/SpriteBatch.h"

#include <functional>
#include <span>

namespace vo::client::platform {
class Input;
}
namespace vo::client::render {
class FontLibrary;
class RenderDevice;
class RenderStates;
} // namespace vo::client::render
namespace vo::client::ui {
class ClipboardProvider;
struct Theme;
} // namespace vo::client::ui

namespace vo::client::scenes {

class SceneManager;

// Owned by the frame loop; lives longer than every scene.
struct SceneContext
{
    render::RenderDevice& device;
    render::RenderStates& states;
    render::SpriteBatch& batch;
    render::FontLibrary& fonts;
    const ui::Theme& theme;
    platform::Input& input;
    ui::ClipboardProvider& clipboard;
    SceneManager& scenes;
    std::function<void()> requestQuit;

    // Updated by the frame loop every frame.
    float viewportWidth = 0.0f; // physical pixels
    float viewportHeight = 0.0f;
    float uiScale = 1.0f; // DPI scale x --ui-scale
    double fps = 0.0;
    std::uint64_t frame = 0;
    render::SpriteBatch::Stats lastBatchStats;
};

class Scene
{
public:
    virtual ~Scene() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // Called when the scene becomes active (the context reference stays valid until onExit).
    virtual void onEnter(SceneContext& ctx) = 0;
    virtual void onExit() {}
    // Consume ctx.input.events() and advance by dt seconds.
    virtual void update(double dt) = 0;
    // Draw the frame (the back buffer is bound and cleared).
    virtual void render() = 0;
    // Viewport size (physical pixels) or UI scale changed; also called right after onEnter.
    virtual void onResize(float width, float height, float uiScale) = 0;
};

} // namespace vo::client::scenes
