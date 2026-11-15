// Automation script format (.voscript): one command per line, '#' starts a comment.
//
//   wait <frames>                 let n frames render before the next command
//   move <x> <y>                  mouse move (client pixels)
//   click <x> <y> [left|right|middle]
//   dblclick <x> <y>              double click (left)
//   mousedown <x> <y> [button]    / mouseup <x> <y> [button]   (drag selections)
//   wheel <notches>               mouse wheel at the current position (+ = up)
//   key <chord>                   press+release, e.g. key tab, key shift+tab, key ctrl+a, key VK_RETURN
//   keydown <chord> / keyup <chord>
//   text <utf8 string>            WM_CHAR-style text input (rest of the line; optional "quotes")
//   screenshot <file.png>         capture this frame (relative paths: --screenshot-dir or cwd)
//   resize <w> <h>                set the window client size
//   uiscale <factor>              override the UI scale multiplier (1.0 = DPI scale only)
//   log <message>                 write a line to the client log
//   quit                          close the client (exit code 0 unless a screenshot failed)
//
// Parsing is pure logic (unit-tested); execution lives in automation::ScriptPlayer.
#pragma once

#include "client/platform/InputEvent.h"
#include "client/platform/KeyNames.h"

#include <string>
#include <string_view>
#include <vector>

namespace vo::client::automation {

enum class ScriptOp : std::uint8_t
{
    Wait,
    Move,
    Click,
    DoubleClick,
    MouseDown,
    MouseUp,
    Wheel,
    Key,
    KeyDown,
    KeyUp,
    Text,
    Screenshot,
    Resize,
    UiScale,
    Log,
    Quit,
};

struct ScriptCommand
{
    ScriptOp op = ScriptOp::Wait;
    int line = 0;
    int a = 0;          // frames / x / width
    int b = 0;          // y / height
    float value = 0.0f; // wheel notches / ui scale
    platform::MouseButton button = platform::MouseButton::Left;
    platform::KeyChord chord;
    std::string text; // text / screenshot path / log message
};

// Parses the whole script; on failure `error` names the line. `out` is replaced.
[[nodiscard]] bool parseScript(std::string_view source, std::vector<ScriptCommand>& out, std::string& error);

} // namespace vo::client::automation
