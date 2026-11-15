// Executes a parsed .voscript frame by frame: input commands are injected into platform::Input
// (the same queue real input uses); window/screenshot/quit commands are returned to the frame loop.
// State-aware commands (waitfor, expect, clickon, expecttext) query the app through Hooks.
#pragma once

#include "client/automation/Script.h"
#include "client/core/Geometry.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::client::platform {
class Input;
}

namespace vo::client::automation {

class ScriptPlayer
{
public:
    struct WidgetInfo
    {
        Rect rect; // physical pixels
        bool visible = false;
        bool enabled = false;
        std::string text; // label/button/text box text, panel title
    };

    // Provided by the frame loop. Missing hooks make the corresponding commands fail.
    struct Hooks
    {
        std::function<bool(std::string_view token)> hasState;
        std::function<std::string()> describeState; // for failure messages
        std::function<std::optional<WidgetInfo>(std::string_view name)> findWidget;
        // `scene <command>`: false (with `error`) fails the script.
        std::function<bool(std::string_view command, std::string& error)> sceneCommand;
    };

    struct FrameActions
    {
        std::optional<std::filesystem::path> screenshot; // capture this frame
        std::optional<std::pair<int, int>> resize;       // client size
        std::optional<float> uiScale;
        std::optional<std::string> failure; // a script assertion failed (exit code 6)
        bool quit = false;
    };

    bool load(const std::filesystem::path& path, std::string& error);
    // Relative screenshot paths are resolved against this directory.
    void setScreenshotDir(std::filesystem::path dir) { m_screenshotDir = std::move(dir); }
    void setHooks(Hooks hooks) { m_hooks = std::move(hooks); }

    // Call once per frame before the scene update.
    FrameActions step(platform::Input& input);
    [[nodiscard]] bool finished() const noexcept
    {
        return m_index >= m_commands.size() && m_wait == 0 && m_holdFrames == 0 && m_dragFrames == 0;
    }
    [[nodiscard]] std::size_t commandCount() const noexcept { return m_commands.size(); }

private:
    void injectKey(platform::Input& input, const platform::KeyChord& chord, bool down, bool up);
    [[nodiscard]] bool stateHolds(std::string_view state) const;
    [[nodiscard]] std::string stateDescription() const;

    std::vector<ScriptCommand> m_commands;
    std::filesystem::path m_screenshotDir;
    Hooks m_hooks;
    std::size_t m_index = 0;
    int m_wait = 0;
    int m_blockedFrames = 0; // frames the current waitfor/clickon has been waiting
    int m_mouseX = 0;
    int m_mouseY = 0;
    // hold / drag in progress
    platform::KeyChord m_holdChord;
    int m_holdFrames = 0;
    std::pair<int, int> m_dragFrom{};
    std::pair<int, int> m_dragTo{};
    platform::MouseButton m_dragButton = platform::MouseButton::Left;
    int m_dragFrames = 0;
    int m_dragStep = 0;
};

} // namespace vo::client::automation
