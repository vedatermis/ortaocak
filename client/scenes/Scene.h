// Scene interface and the services a scene can use (SceneContext). One scene is active at a time;
// SceneManager switches between them with deferred transitions.
#pragma once

#include "client/render/SpriteBatch.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::client::net {
class GameClient;
}
namespace vo::client::platform {
class Input;
}
namespace vo::client::render {
class AssetCache;
class FontLibrary;
class RenderDevice;
class RenderStates;
class WorldRenderer;
} // namespace vo::client::render
namespace vo::client::ui {
class ClipboardProvider;
class UiContext;
struct Theme;
} // namespace vo::client::ui

namespace vo::client::scenes {

class SceneManager;
class SceneResources;

// Static facts about this client run, shown or used by the scenes.
struct AppInfo
{
    std::filesystem::path settingsFile; // user settings ("Beni hatırla")
    std::string serverLabel;            // "127.0.0.1:15001"
    std::string versionLabel;           // "v0.1.0 (Debug)"
};

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
    net::GameClient& net;
    const SceneResources& resources;
    const AppInfo& app;
    std::function<void()> requestQuit;

    // ---- 3D world (M6); null if the 3D renderer could not start ----
    render::WorldRenderer* worldRenderer = nullptr;
    render::AssetCache* assets = nullptr;
    int debugGhosts = 0;      // --debug-ghosts N: synthetic remote players in the world
    bool shadows = true;      // --no-shadows disables the sun shadow map
    bool scripted = false;    // a .voscript drives the input (no real cursor warping)
    // Hide + confine the cursor while the camera is dragged / restore it; warp it (client pixels).
    std::function<void(bool captured)> setMouseCaptured;
    std::function<void(int x, int y)> warpMouse;

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

    // ---- Automation (client/automation: waitfor / clickon) ----
    // State tokens that currently hold. Default: just the scene name.
    virtual void automationStates(std::vector<std::string>& out) const { out.emplace_back(name()); }
    // The scene's widget tree, if it has one (for clickon / expecttext).
    [[nodiscard]] virtual ui::UiContext* automationUi() noexcept { return nullptr; }
    // `scene <command>` script lines. Return false with `error` to fail the script.
    virtual bool automationCommand(std::string_view command, std::string& error)
    {
        error = std::string("scene '") + name() + "' has no automation command '" + std::string(command) + "'";
        return false;
    }
};

} // namespace vo::client::scenes
