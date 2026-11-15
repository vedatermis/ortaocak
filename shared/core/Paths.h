// Filesystem path helpers (UTF-8 aware).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace vo::core {

// UTF-8 representation of a path (safe for logging; std::filesystem::path::string() uses the ANSI
// code page on Windows and can throw on unrepresentable characters).
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view utf8);

// Directory containing the running executable.
[[nodiscard]] std::filesystem::path executableDir();

// Looks for `relative` (e.g. "config/server.toml") in `start` and each of its parent directories.
[[nodiscard]] std::optional<std::filesystem::path> findUpwards(const std::filesystem::path& start,
                                                               const std::filesystem::path& relative);

// Locates a data file by searching upwards from the current directory, then from the executable
// directory. Lets exes run from the repo root or from build/<preset>/bin without extra flags.
[[nodiscard]] std::optional<std::filesystem::path> locateDataFile(const std::filesystem::path& relative);

} // namespace vo::core
