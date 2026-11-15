#include "client/platform/KeyNames.h"

#include "client/platform/InputEvent.h"

#include <array>
#include <string>

namespace vo::client::platform {
namespace {

struct NamedKey
{
    std::string_view name;
    std::uint16_t key;
};

constexpr std::array kNamedKeys{
    NamedKey{"back", keys::kBackspace},    NamedKey{"backspace", keys::kBackspace},
    NamedKey{"tab", keys::kTab},           NamedKey{"return", keys::kEnter},
    NamedKey{"enter", keys::kEnter},       NamedKey{"shift", keys::kShift},
    NamedKey{"control", keys::kControl},   NamedKey{"ctrl", keys::kControl},
    NamedKey{"menu", keys::kAlt},          NamedKey{"alt", keys::kAlt},
    NamedKey{"pause", keys::kPause},       NamedKey{"capital", keys::kCapsLock},
    NamedKey{"capslock", keys::kCapsLock}, NamedKey{"escape", keys::kEscape},
    NamedKey{"esc", keys::kEscape},        NamedKey{"space", keys::kSpace},
    NamedKey{"prior", keys::kPageUp},      NamedKey{"pageup", keys::kPageUp},
    NamedKey{"next", keys::kPageDown},     NamedKey{"pagedown", keys::kPageDown},
    NamedKey{"end", keys::kEnd},           NamedKey{"home", keys::kHome},
    NamedKey{"left", keys::kLeft},         NamedKey{"up", keys::kUp},
    NamedKey{"right", keys::kRight},       NamedKey{"down", keys::kDown},
    NamedKey{"insert", keys::kInsert},     NamedKey{"delete", keys::kDelete},
    NamedKey{"del", keys::kDelete},        NamedKey{"lshift", keys::kLShift},
    NamedKey{"rshift", keys::kRShift},     NamedKey{"lcontrol", keys::kLControl},
    NamedKey{"rcontrol", keys::kRControl}, NamedKey{"lmenu", keys::kLAlt},
    NamedKey{"rmenu", keys::kRAlt},
};

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

} // namespace

std::optional<std::uint16_t> keyFromName(std::string_view name)
{
    std::string n = lower(name);
    if (n.starts_with("vk_"))
        n.erase(0, 3);
    if (n.empty())
        return std::nullopt;
    if (n.size() == 1)
    {
        const char c = n[0];
        if (c >= 'a' && c <= 'z')
            return keys::letter(c);
        if (c >= '0' && c <= '9')
            return static_cast<std::uint16_t>(keys::k0 + (c - '0'));
    }
    if (n[0] == 'f' && n.size() <= 3)
    {
        int number = 0;
        bool digits = n.size() > 1;
        for (std::size_t i = 1; i < n.size(); ++i)
        {
            if (n[i] < '0' || n[i] > '9')
                digits = false;
            else
                number = number * 10 + (n[i] - '0');
        }
        if (digits && number >= 1 && number <= 24)
            return static_cast<std::uint16_t>(keys::kF1 + number - 1);
    }
    for (const NamedKey& k : kNamedKeys)
    {
        if (k.name == n)
            return k.key;
    }
    return std::nullopt;
}

std::optional<KeyChord> parseKeyChord(std::string_view text)
{
    KeyChord chord;
    while (true)
    {
        const std::size_t plus = text.find('+');
        // A trailing '+' with nothing after it is not a separator ("ctrl++" is not supported).
        if (plus == std::string_view::npos || plus + 1 >= text.size())
            break;
        const std::string mod = lower(text.substr(0, plus));
        if (mod == "ctrl" || mod == "control")
            chord.mods |= mods::kCtrl;
        else if (mod == "shift")
            chord.mods |= mods::kShift;
        else if (mod == "alt")
            chord.mods |= mods::kAlt;
        else
            return std::nullopt;
        text.remove_prefix(plus + 1);
    }
    const auto key = keyFromName(text);
    if (!key)
        return std::nullopt;
    chord.key = *key;
    return chord;
}

} // namespace vo::client::platform
