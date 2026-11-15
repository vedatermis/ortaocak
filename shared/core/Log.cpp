#include "shared/core/Log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace vo::log {
namespace {

struct LoggerState
{
    std::mutex mutex;
    std::ofstream file;
    std::atomic<Level> minLevel{
#if defined(VO_DEBUG)
        Level::Debug
#else
        Level::Info
#endif
    };
    std::atomic<bool> console{true};
    std::atomic<bool> debugger{false};
};

LoggerState& state()
{
    static LoggerState s;
    return s;
}

std::string_view baseName(const char* path) noexcept
{
    if (path == nullptr)
        return {};
    std::string_view p{path};
    const auto pos = p.find_last_of("/\\");
    return pos == std::string_view::npos ? p : p.substr(pos + 1);
}

std::string timestampNow()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tmLocal{};
#if defined(_WIN32)
    localtime_s(&tmLocal, &t);
#else
    localtime_r(&t, &tmLocal);
#endif
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", tmLocal.tm_year + 1900, tmLocal.tm_mon + 1,
                       tmLocal.tm_mday, tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec, ms.count());
}

// Small sequential per-thread id (T1, T2, ...) - easier to read than OS thread ids.
unsigned threadTag() noexcept
{
    static std::atomic<unsigned> counter{0};
    thread_local const unsigned tag = ++counter;
    return tag;
}

char asciiLower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (asciiLower(a[i]) != asciiLower(b[i]))
            return false;
    }
    return true;
}

} // namespace

void setLevel(Level lvl) noexcept
{
    state().minLevel.store(lvl, std::memory_order_relaxed);
}

Level level() noexcept
{
    return state().minLevel.load(std::memory_order_relaxed);
}

bool isEnabled(Level lvl) noexcept
{
    return lvl != Level::Off && lvl >= level();
}

void setConsoleEnabled(bool enabled) noexcept
{
    state().console.store(enabled, std::memory_order_relaxed);
}

void setDebuggerOutputEnabled(bool enabled) noexcept
{
    state().debugger.store(enabled, std::memory_order_relaxed);
}

bool openFile(const std::filesystem::path& path, bool truncate)
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file.is_open())
        s.file.close();
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    s.file.open(path, std::ios::binary | (truncate ? std::ios::trunc : std::ios::app));
    return s.file.is_open();
}

void closeFile() noexcept
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file.is_open())
        s.file.close();
}

void flush() noexcept
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::fflush(stdout);
    std::fflush(stderr);
    if (s.file.is_open())
        s.file.flush();
}

std::string_view toString(Level lvl) noexcept
{
    static constexpr std::array<std::string_view, 7> kNames{"TRACE", "DEBUG", "INFO", "WARN",
                                                            "ERROR", "FATAL", "OFF"};
    const auto idx = static_cast<std::size_t>(lvl);
    return idx < kNames.size() ? kNames[idx] : std::string_view{"?"};
}

std::optional<Level> parseLevel(std::string_view text) noexcept
{
    struct Entry
    {
        std::string_view name;
        Level level;
    };
    static constexpr std::array<Entry, 8> kEntries{{
        {"trace", Level::Trace},
        {"debug", Level::Debug},
        {"info", Level::Info},
        {"warn", Level::Warn},
        {"warning", Level::Warn},
        {"error", Level::Error},
        {"fatal", Level::Fatal},
        {"off", Level::Off},
    }};
    for (const auto& e : kEntries)
    {
        if (equalsIgnoreCase(text, e.name))
            return e.level;
    }
    return std::nullopt;
}

void writeMessage(Level lvl, std::string_view message, const char* file, int line) noexcept
{
    if (!isEnabled(lvl))
        return;
    try
    {
        std::string out = std::format("{} [{:<5}] [T{}] {}", timestampNow(), toString(lvl),
                                      threadTag(), message);
        if (lvl >= Level::Error && file != nullptr)
            out += std::format(" ({}:{})", baseName(file), line);
        out += '\n';

        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.console.load(std::memory_order_relaxed))
        {
            std::FILE* stream = lvl >= Level::Warn ? stderr : stdout;
            std::fwrite(out.data(), 1, out.size(), stream);
            if (lvl >= Level::Warn)
                std::fflush(stream);
        }
        if (s.file.is_open())
        {
            s.file.write(out.data(), static_cast<std::streamsize>(out.size()));
            if (lvl >= Level::Warn)
                s.file.flush();
        }
#if defined(_WIN32)
        if (s.debugger.load(std::memory_order_relaxed))
            ::OutputDebugStringA(out.c_str());
#endif
    }
    catch (...)
    {
        // Logging must never throw (e.g. std::bad_alloc); drop the line.
    }
}

} // namespace vo::log
