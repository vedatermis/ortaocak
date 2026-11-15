#include "client/automation/ScriptPlayer.h"

#include "client/core/Utf8.h"
#include "client/platform/Input.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"

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

ScriptPlayer::FrameActions ScriptPlayer::step(platform::Input& input)
{
    FrameActions actions;
    if (m_wait > 0)
    {
        --m_wait;
        if (m_wait > 0)
            return actions;
    }

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

    while (m_index < m_commands.size())
    {
        const ScriptCommand& c = m_commands[m_index++];
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
            break;
        case ScriptOp::Quit:
            actions.quit = true;
            return actions;
        }
    }
    if (m_index >= m_commands.size())
        actions.quit = true; // end of script
    return actions;
}

} // namespace vo::client::automation
