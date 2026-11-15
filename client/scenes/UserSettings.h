// Per-user client settings persisted between runs ("Beni hatırla"). Only the username is stored,
// never the password. The file is a small TOML document in a user-writable location
// (%LOCALAPPDATA%\VedatOnline\client_settings.toml by default, chosen by main.cpp; --settings
// overrides it). Pure logic + file I/O (vo_client_core), unit-tested.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vo::client::scenes {

struct UserSettings
{
    bool rememberUsername = false;
    std::string username; // only kept if rememberUsername and it is a valid username

    bool operator==(const UserSettings&) const = default;
};

// TOML text for `settings` (an invalid or unremembered username is not written).
[[nodiscard]] std::string serializeUserSettings(const UserSettings& settings);
// Parses TOML text. Unknown keys are ignored; an invalid username is dropped. False (with `error`)
// only if the text is not valid TOML.
[[nodiscard]] bool parseUserSettings(std::string_view toml, UserSettings& out, std::string& error);

// Missing file -> defaults and true. Unreadable/corrupt file -> defaults, false and `error`.
bool loadUserSettings(const std::filesystem::path& file, UserSettings& out, std::string& error);
// Creates the parent directory if needed; writes via a temporary file + rename.
bool saveUserSettings(const std::filesystem::path& file, const UserSettings& settings, std::string& error);

} // namespace vo::client::scenes
