// Executes a parsed .voscript frame by frame: input commands are injected into platform::Input
// (the same queue real input uses); window/screenshot/quit commands are returned to the frame loop.
#pragma once

#include "client/automation/Script.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vo::client::platform {
class Input;
}

namespace vo::client::automation {

class ScriptPlayer
{
public:
    struct FrameActions
    {
        std::optional<std::filesystem::path> screenshot; // capture this frame
        std::optional<std::pair<int, int>> resize;       // client size
        std::optional<float> uiScale;
        bool quit = false;
    };

    bool load(const std::filesystem::path& path, std::string& error);
    // Relative screenshot paths are resolved against this directory.
    void setScreenshotDir(std::filesystem::path dir) { m_screenshotDir = std::move(dir); }

    // Call once per frame before the scene update.
    FrameActions step(platform::Input& input);
    [[nodiscard]] bool finished() const noexcept { return m_index >= m_commands.size() && m_wait == 0; }
    [[nodiscard]] std::size_t commandCount() const noexcept { return m_commands.size(); }

private:
    void injectKey(platform::Input& input, const platform::KeyChord& chord, bool down, bool up);

    std::vector<ScriptCommand> m_commands;
    std::filesystem::path m_screenshotDir;
    std::size_t m_index = 0;
    int m_wait = 0;
    int m_mouseX = 0;
    int m_mouseY = 0;
};

} // namespace vo::client::automation
