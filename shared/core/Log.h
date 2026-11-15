// Thread-safe logger: levels, timestamps, console and optional file sink.
//
// Usage:
//   VO_LOG_INFO("listening on port {}", port);
//   VO_LOG_ERROR("failed: {}", reason);
//
// Formatting uses std::format; format strings are checked at compile time.
#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vo::log {

enum class Level : std::uint8_t
{
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
};

// Minimum level that is emitted. Default: Info (Debug builds: Debug).
void setLevel(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] bool isEnabled(Level level) noexcept;

// Console sink (stdout for < Warn, stderr for >= Warn). Enabled by default.
void setConsoleEnabled(bool enabled) noexcept;

// Also forward every line to OutputDebugString (Windows). Disabled by default.
void setDebuggerOutputEnabled(bool enabled) noexcept;

// Opens (appends to) a log file. Returns false if the file cannot be opened.
bool openFile(const std::filesystem::path& path, bool truncate = false);
void closeFile() noexcept;
void flush() noexcept;

[[nodiscard]] std::string_view toString(Level level) noexcept;
// Accepts trace|debug|info|warn|warning|error|fatal|off (case-insensitive).
[[nodiscard]] std::optional<Level> parseLevel(std::string_view text) noexcept;

// Writes an already formatted message. Thread-safe.
void writeMessage(Level level, std::string_view message, const char* file, int line) noexcept;

template <class... Args>
void write(Level lvl, const char* file, int line, std::format_string<Args...> fmt, Args&&... args)
{
    if (!isEnabled(lvl))
        return;
    writeMessage(lvl, std::format(fmt, std::forward<Args>(args)...), file, line);
}

} // namespace vo::log

#define VO_LOG_AT(lvl, ...) ::vo::log::write((lvl), __FILE__, __LINE__, __VA_ARGS__)
#define VO_LOG_TRACE(...) VO_LOG_AT(::vo::log::Level::Trace, __VA_ARGS__)
#define VO_LOG_DEBUG(...) VO_LOG_AT(::vo::log::Level::Debug, __VA_ARGS__)
#define VO_LOG_INFO(...) VO_LOG_AT(::vo::log::Level::Info, __VA_ARGS__)
#define VO_LOG_WARN(...) VO_LOG_AT(::vo::log::Level::Warn, __VA_ARGS__)
#define VO_LOG_ERROR(...) VO_LOG_AT(::vo::log::Level::Error, __VA_ARGS__)
#define VO_LOG_FATAL(...) VO_LOG_AT(::vo::log::Level::Fatal, __VA_ARGS__)
