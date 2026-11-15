#include "client/automation/ScriptPlayer.h"

#include "client/core/Utf8.h"
#include "client/platform/Input.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"

#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

namespace vo::client::automation {

using platform::EventType;
using platform::InputEvent;
using platform::MouseButton;

bool ScriptPlayer::load(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "cannot open script " + vo::core::pathToUtf8(path);
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    if (!parseScript(buffer.str(), m_commands, error))
        return false;
    m_index = 0;
    m_wait = 0;
    return true;
}

void ScriptPlayer::injectKey(platform::Input& input, const platform::KeyChord& chord, bool down, bool up)
{
    struct Mod
    {
        std::uint8_t flag;
        std::uint16_t key;
    };
    constexpr Mod kMods[] = {{platform::mods::kCtrl, platform::keys::kControl},
                             {platform::mods::kShift, platform::keys::kShift},
                             {platform::mods::kAlt, platform::keys::kAlt}};
    InputEvent e;
    if (down)
    {
        for (const Mod& m : kMods)
        {
            if ((chord.mods & m.flag) == 0)
                continue;
            e.type = EventType::KeyDown;
            e.key = m.key;
            input.inject(e);
        }
        e.type = EventType::KeyDown;
        e.key = chord.key;
        input.inject(e);
    }
    if (up)
    {
        e.type = EventType::KeyUp;
        e.key = chord.key;
        input.inject(e);
        for (auto it = std::rbegin(kMods); it != std::rend(kMods); ++it)
        {
            if ((chord.mods & it->flag) == 0)
                continue;
            e.type = EventType::KeyUp;
            e.key = it->key;
            input.inject(e);
        }
    }
}

bool ScriptPlayer::stateHolds(std::string_view state) const
{
    if (!m_hooks.hasState)
        return false;
    for (const std::string_view token : splitStateTokens(state))
    {
        if (!m_hooks.hasState(token))
            return false;
    }
    return true;
}

std::string ScriptPlayer::stateDescription() const
{
    return m_hooks.describeState ? m_hooks.describeState() : std::string("unknown");
}

ScriptPlayer::FrameActions ScriptPlayer::step(platform::Input& input)
{
    FrameActions actions;
    auto mouse = [&](EventType type, MouseButton button, int x, int y, std::uint8_t clicks)
    {
        InputEvent e;
        e.type = type;
        e.button = button;
        e.x = x;
        e.y = y;
        e.clickCount = clicks;
        input.inject(e);
        m_mouseX = x;
        m_mouseY = y;
    };

    if (m_wait > 0)
    {
        --m_wait;
        if (m_wait > 0)
            return actions;
    }
    if (m_holdFrames > 0)
    {
        if (--m_holdFrames > 0)
            return actions;
        injectKey(input, m_holdChord, false, true);
    }
    if (m_dragFrames > 0)
    {
        ++m_dragStep;
        const float t = static_cast<float>(m_dragStep) / static_cast<float>(m_dragFrames);
        const int x = m_dragFrom.first + static_cast<int>(std::lround(t * static_cast<float>(m_dragTo.first - m_dragFrom.first)));
        const int y = m_dragFrom.second + static_cast<int>(std::lround(t * static_cast<float>(m_dragTo.second - m_dragFrom.second)));
        mouse(EventType::MouseMove, m_dragButton, x, y, 1);
        if (m_dragStep < m_dragFrames)
            return actions;
        mouse(EventType::MouseUp, m_dragButton, x, y, 1);
        m_dragFrames = 0;
    }

    while (m_index < m_commands.size())
    {
        const ScriptCommand& c = m_commands[m_index++];
        const bool queriesState = c.op == ScriptOp::WaitFor || c.op == ScriptOp::Expect ||
                                  c.op == ScriptOp::ClickOn || c.op == ScriptOp::ExpectText ||
                                  c.op == ScriptOp::Scene;
        if (queriesState && !input.events().empty())
        {
            // Input injected earlier this frame has not reached the scene yet: look at the state
            // only after it has been processed.
            --m_index;
            return actions;
        }
        switch (c.op)
        {
        case ScriptOp::Wait:
            if (c.a > 0)
            {
                m_wait = c.a;
                return actions;
            }
            break;
        case ScriptOp::Move:
            mouse(EventType::MouseMove, MouseButton::Left, c.a, c.b, 1);
            break;
        case ScriptOp::Click:
            mouse(EventType::MouseMove, c.button, c.a, c.b, 1);
            mouse(EventType::MouseDown, c.button, c.a, c.b, 1);
            mouse(EventType::MouseUp, c.button, c.a, c.b, 1);
            break;
        case ScriptOp::DoubleClick:
            mouse(EventType::MouseMove, MouseButton::Left, c.a, c.b, 1);
            mouse(EventType::MouseDown, MouseButton::Left, c.a, c.b, 1);
            mouse(EventType::MouseUp, MouseButton::Left, c.a, c.b, 1);
            mouse(EventType::MouseDown, MouseButton::Left, c.a, c.b, 2);
            mouse(EventType::MouseUp, MouseButton::Left, c.a, c.b, 1);
            break;
        case ScriptOp::MouseDown:
            mouse(EventType::MouseMove, c.button, c.a, c.b, 1);
            mouse(EventType::MouseDown, c.button, c.a, c.b, 1);
            break;
        case ScriptOp::MouseUp:
            mouse(EventType::MouseMove, c.button, c.a, c.b, 1);
            mouse(EventType::MouseUp, c.button, c.a, c.b, 1);
            break;
        case ScriptOp::Wheel:
        {
            InputEvent e;
            e.type = EventType::MouseWheel;
            e.wheel = c.value;
            input.inject(e);
            break;
        }
        case ScriptOp::Key:
            injectKey(input, c.chord, true, true);
            break;
        case ScriptOp::KeyDown:
            injectKey(input, c.chord, true, false);
            break;
        case ScriptOp::KeyUp:
            injectKey(input, c.chord, false, true);
            break;
        case ScriptOp::Text:
            for (std::size_t pos = 0; pos < c.text.size();)
            {
                const utf8::Decoded d = utf8::decode(c.text, pos);
                pos += d.length;
                InputEvent e;
                e.type = EventType::Char;
                e.codePoint = d.codePoint;
                input.inject(e);
            }
            break;
        case ScriptOp::Screenshot:
        {
            std::filesystem::path p = vo::core::pathFromUtf8(c.text);
            if (p.is_relative() && !m_screenshotDir.empty())
                p = m_screenshotDir / p;
            actions.screenshot = p;
            return actions; // capture this frame; continue next frame
        }
        case ScriptOp::Resize:
            actions.resize = std::make_pair(c.a, c.b);
            break;
        case ScriptOp::UiScale:
            actions.uiScale = c.value;
            break;
        case ScriptOp::Log:
            VO_LOG_INFO("script: {}", c.text);
            vo::log::flush(); // external drivers watch the log for these lines (run-ui-flows.ps1 hooks)
            break;
        case ScriptOp::Quit:
            actions.quit = true;
            return actions;
        case ScriptOp::WaitFor:
            if (stateHolds(c.text))
            {
                VO_LOG_INFO("script: line {}: '{}' reached after {} frame(s)", c.line, c.text, m_blockedFrames);
                m_blockedFrames = 0;
                break;
            }
            if (m_blockedFrames >= c.a)
            {
                actions.failure = std::format("line {}: waitfor '{}' timed out after {} frames (state: {})",
                                              c.line, c.text, c.a, stateDescription());
                m_index = m_commands.size();
                return actions;
            }
            ++m_blockedFrames;
            --m_index; // retry next frame
            return actions;
        case ScriptOp::Expect:
            if (!stateHolds(c.text))
            {
                actions.failure =
                    std::format("line {}: expected '{}' (state: {})", c.line, c.text, stateDescription());
                m_index = m_commands.size();
                return actions;
            }
            break;
        case ScriptOp::ClickOn:
        {
            std::optional<WidgetInfo> w;
            if (m_hooks.findWidget)
                w = m_hooks.findWidget(c.text);
            if (w && w->visible && w->enabled && !w->rect.empty())
            {
                m_blockedFrames = 0;
                const int x = static_cast<int>(w->rect.centerX());
                const int y = static_cast<int>(w->rect.centerY());
                mouse(EventType::MouseMove, MouseButton::Left, x, y, 1);
                mouse(EventType::MouseDown, MouseButton::Left, x, y, 1);
                mouse(EventType::MouseUp, MouseButton::Left, x, y, 1);
                break;
            }
            if (m_blockedFrames >= c.a)
            {
                const char* why = !w ? "not found" : !w->visible ? "not visible" : !w->enabled ? "disabled"
                                                                                              : "empty rect";
                actions.failure = std::format("line {}: clickon '{}' failed: widget {} after {} frames (state: {})",
                                              c.line, c.text, why, c.a, stateDescription());
                m_index = m_commands.size();
                return actions;
            }
            ++m_blockedFrames;
            --m_index;
            return actions;
        }
        case ScriptOp::Hold:
            injectKey(input, c.chord, true, false);
            m_holdChord = c.chord;
            m_holdFrames = c.frames;
            return actions;
        case ScriptOp::Drag:
            mouse(EventType::MouseMove, c.button, c.a, c.b, 1);
            mouse(EventType::MouseDown, c.button, c.a, c.b, 1);
            m_dragFrom = {c.a, c.b};
            m_dragTo = {c.c, c.d};
            m_dragButton = c.button;
            m_dragFrames = c.frames;
            m_dragStep = 0;
            return actions;
        case ScriptOp::Scene:
        {
            std::string error = "no active scene";
            if (!m_hooks.sceneCommand || !m_hooks.sceneCommand(c.text, error))
            {
                actions.failure = std::format("line {}: scene '{}' failed: {}", c.line, c.text, error);
                m_index = m_commands.size();
                return actions;
            }
            VO_LOG_INFO("script: line {}: scene '{}' ok", c.line, c.text);
            break;
        }
        case ScriptOp::ExpectText:
        {
            std::optional<WidgetInfo> w;
            if (m_hooks.findWidget)
                w = m_hooks.findWidget(c.text);
            if (!w || w->text.find(c.arg) == std::string::npos)
            {
                actions.failure = std::format("line {}: expecttext '{}': wanted '{}', found {}", c.line, c.text,
                                              c.arg, w ? "'" + w->text + "'" : std::string("no such widget"));
                m_index = m_commands.size();
                return actions;
            }
            VO_LOG_INFO("script: line {}: '{}' shows '{}'", c.line, c.text, c.arg);
            break;
        }
        }
    }
    if (m_index >= m_commands.size())
        actions.quit = true; // end of script
    return actions;
}

} // namespace vo::client::automation
