// Monotonic clock helpers. Always use these (steady_clock) for durations, timeouts and ticks;
// never wall-clock time (system_clock), which can jump.
#pragma once

#include <chrono>
#include <cstdint>

namespace vo::core {

using MonoClock = std::chrono::steady_clock;
using MonoTime = MonoClock::time_point;
using Nanoseconds = std::chrono::nanoseconds;
using Milliseconds = std::chrono::milliseconds;

static_assert(MonoClock::is_steady, "steady_clock must be monotonic");

[[nodiscard]] inline MonoTime monoNow() noexcept
{
    return MonoClock::now();
}

// Nanoseconds on the monotonic clock. The epoch is arbitrary: only differences are meaningful.
[[nodiscard]] inline std::int64_t monoNowNs() noexcept
{
    return std::chrono::duration_cast<Nanoseconds>(MonoClock::now().time_since_epoch()).count();
}

[[nodiscard]] inline double toSeconds(MonoClock::duration d) noexcept
{
    return std::chrono::duration<double>(d).count();
}

// Measures elapsed time since construction or the last reset().
class Stopwatch
{
public:
    Stopwatch() noexcept : m_start(monoNow()) {}

    void reset() noexcept { m_start = monoNow(); }

    [[nodiscard]] MonoClock::duration elapsed() const noexcept { return monoNow() - m_start; }
    [[nodiscard]] double elapsedSeconds() const noexcept { return toSeconds(elapsed()); }
    [[nodiscard]] std::int64_t elapsedMs() const noexcept
    {
        return std::chrono::duration_cast<Milliseconds>(elapsed()).count();
    }

    // Returns elapsed time and restarts the stopwatch (frame delta helper).
    MonoClock::duration lap() noexcept
    {
        const MonoTime now = monoNow();
        const auto d = now - m_start;
        m_start = now;
        return d;
    }

private:
    MonoTime m_start;
};

} // namespace vo::core
