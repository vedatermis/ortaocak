// TickStats: tick-duration histogram of the world loop (50 us buckets up to 250 ms, plus overflow)
// with count, average, percentiles and maximum, and the number of slow ticks (longer than the tick
// budget). Not thread-safe; World guards it.
#pragma once

#include "shared/core/Clock.h"

#include <array>
#include <cstdint>

namespace vo::server::world {

class TickStats
{
public:
    struct Summary
    {
        std::uint64_t ticks = 0;
        std::uint64_t slowTicks = 0;
        double avgMs = 0.0;
        double p50Ms = 0.0;
        double p99Ms = 0.0;
        double maxMs = 0.0;
    };

    explicit TickStats(core::MonoClock::duration budget = std::chrono::milliseconds(50)) : m_budget(budget) {}

    void record(core::MonoClock::duration d) noexcept;
    void reset() noexcept;
    [[nodiscard]] Summary summary() const noexcept;
    // Upper bound (ms) of the bucket containing the q-quantile (0..1); 0 without samples.
    [[nodiscard]] double quantileMs(double q) const noexcept;

    static constexpr double kBucketMs = 0.05;
    static constexpr std::size_t kBuckets = 5000; // 0 .. 250 ms; the last one also holds overflow

private:
    core::MonoClock::duration m_budget;
    std::array<std::uint32_t, kBuckets> m_hist{};
    std::uint64_t m_count = 0;
    std::uint64_t m_slow = 0;
    double m_sumMs = 0.0;
    double m_maxMs = 0.0;
};

} // namespace vo::server::world
