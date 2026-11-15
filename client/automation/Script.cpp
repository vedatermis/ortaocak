#include "client/automation/Script.h"

#include <charconv>
#include <format>

namespace vo::client::automation {
namespace {

std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// Splits off the first whitespace-delimited token.
std::string_view nextToken(std::string_view& rest) noexcept
{
    rest = trim(rest);
    std::size_t end = 0;
    while (end < rest.size() && rest[end] != ' ' && rest[end] != '\t')
        ++end;
    const std::string_view token = rest.substr(0, end);
    rest.remove_prefix(end);
    rest = trim(rest);
    return token;
}

bool parseInt(std::string_view s, int& out) noexcept
{
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size() && !s.empty();
}

bool parseFloat(std::string_view s, float& out) noexcept
{
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size() && !s.empty();
}

bool parseButton(std::string_view s, platform::MouseButton& out) noexcept
{
    if (s.empty() || s == "left")
        out = platform::MouseButton::Left;
    else if (s == "right")
        out = platform::MouseButton::Right;
    else if (s == "middle")
        out = platform::MouseButton::Middle;
    else
        return false;
    return true;
}

std::string_view unquote(std::string_view s) noexcept
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

} // namespace

bool parseScript(std::string_view source, std::vector<ScriptCommand>& out, std::string& error)
{
    out.clear();
    int lineNo = 0;
    while (!source.empty())
    {
        ++lineNo;
        const std::size_t nl = source.find('\n');
        std::string_view line = source.substr(0, nl);
        source.remove_prefix(nl == std::string_view::npos ? source.size() : nl + 1);
        if (lineNo == 1 && line.starts_with("\xEF\xBB\xBF"))
            line.remove_prefix(3); // UTF-8 BOM

        line = trim(line);
        if (line.empty() || line.front() == '#')
            continue;

        std::string_view rest = line;
        const std::string_view cmd = nextToken(rest);
        ScriptCommand c;
        c.line = lineNo;
        auto fail = [&](std::string_view what)
        {
            error = std::format("script line {}: {} ('{}')", lineNo, what, line);
            return false;
        };
        auto twoInts = [&]()
        {
            const std::string_view a = nextToken(rest);
            const std::string_view b = nextToken(rest);
            return parseInt(a, c.a) && parseInt(b, c.b);
        };

        if (cmd == "wait")
        {
            c.op = ScriptOp::Wait;
            if (!parseInt(nextToken(rest), c.a) || c.a < 0)
                return fail("expected a frame count >= 0");
        }
        else if (cmd == "move")
        {
            c.op = ScriptOp::Move;
            if (!twoInts())
                return fail("expected x y");
        }
        else if (cmd == "click" || cmd == "mousedown" || cmd == "mouseup" || cmd == "dblclick")
        {
            c.op = cmd == "click"       ? ScriptOp::Click
                   : cmd == "mousedown" ? ScriptOp::MouseDown
                   : cmd == "mouseup"   ? ScriptOp::MouseUp
                                        : ScriptOp::DoubleClick;
            if (!twoInts())
                return fail("expected x y");
            if (!parseButton(nextToken(rest), c.button))
                return fail("expected left, right or middle");
        }
        else if (cmd == "wheel")
        {
            c.op = ScriptOp::Wheel;
            if (!parseFloat(nextToken(rest), c.value))
                return fail("expected a notch count");
        }
        else if (cmd == "key" || cmd == "keydown" || cmd == "keyup")
        {
            c.op = cmd == "key" ? ScriptOp::Key : cmd == "keydown" ? ScriptOp::KeyDown : ScriptOp::KeyUp;
            const auto chord = platform::parseKeyChord(nextToken(rest));
            if (!chord)
                return fail("unknown key");
            c.chord = *chord;
        }
        else if (cmd == "text")
        {
            c.op = ScriptOp::Text;
            c.text = std::string(unquote(rest));
            rest = {};
        }
        else if (cmd == "screenshot")
        {
            c.op = ScriptOp::Screenshot;
            c.text = std::string(unquote(rest));
            rest = {};
            if (c.text.empty())
                return fail("expected a file path");
        }
        else if (cmd == "resize")
        {
            c.op = ScriptOp::Resize;
            if (!twoInts() || c.a <= 0 || c.b <= 0)
                return fail("expected width height > 0");
        }
        else if (cmd == "uiscale")
        {
            c.op = ScriptOp::UiScale;
            if (!parseFloat(nextToken(rest), c.value) || c.value < 0.5f || c.value > 4.0f)
                return fail("expected a factor in [0.5, 4]");
        }
        else if (cmd == "log")
        {
            c.op = ScriptOp::Log;
            c.text = std::string(rest);
            rest = {};
        }
        else if (cmd == "quit")
        {
            c.op = ScriptOp::Quit;
        }
        else
        {
            return fail("unknown command");
        }
        if (!rest.empty())
            return fail("unexpected trailing arguments");
        out.push_back(std::move(c));
    }
    return true;
}

} // namespace vo::client::automation
