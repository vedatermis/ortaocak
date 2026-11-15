// Mapping between virtual-key names and codes, used by the automation script ("key shift+tab").
// Accepts Win32 names ("VK_RETURN"), short names ("enter", "left", "f5"), single letters/digits.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vo::client::platform {

[[nodiscard]] std::optional<std::uint16_t> keyFromName(std::string_view name);

struct KeyChord
{
    std::uint16_t key = 0;
    std::uint8_t mods = 0; // platform::mods flags
};

// "ctrl+shift+left", "VK_TAB", "a". Modifier names: ctrl/control, shift, alt. Case-insensitive.
[[nodiscard]] std::optional<KeyChord> parseKeyChord(std::string_view text);

} // namespace vo::client::platform
