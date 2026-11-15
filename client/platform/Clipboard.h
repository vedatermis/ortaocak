// System clipboard text helpers (CF_UNICODETEXT <-> UTF-8).
#pragma once

#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace vo::client::platform {

// Returns the clipboard text, or nullopt if there is none / the clipboard is locked.
[[nodiscard]] std::optional<std::string> getClipboardText(HWND owner);
bool setClipboardText(HWND owner, std::string_view utf8);

} // namespace vo::client::platform
