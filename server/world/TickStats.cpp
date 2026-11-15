#include "server/world/TickStats.h"

#include <algorithm>
#include <chrono>

namespace vo::server::world {

void TickStats::record(core::MonoClock::duration d) noexcept
{
    const double ms = std::chrono::duration<double, std::milli>(d).count();
    const auto bucket = std::min(static_cast<std::size_t>(std::max(ms, 0.0) / kBucketMs), kBuckets - 1);
    ++m_hist[bucket];
    ++m_count;
    m_sumMs += ms;
    m_maxMs = std::max(m_maxMs, ms);
    if (d > m_budget)
        ++m_slow;
}

void TickStats::reset() noexcept
{
    m_hist.fill(0);
    m_count = 0;
    m_slow = 0;
    m_sumMs = 0.0;
    m_maxMs = 0.0;
}

double TickStats::quantileMs(double q) const noexcept
{
    if (m_count == 0)
        return 0.0;
    const auto target = static_cast<std::uint64_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(m_count - 1)) + 1;
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBuckets; ++i)
    {
        seen += m_hist[i];
        if (seen >= target)
            return std::min(static_cast<double>(i + 1) * kBucketMs, m_maxMs);
    }
    return m_maxMs;
}

TickStats::Summary TickStats::summary() const noexcept
{
    Summary s;
    s.ticks = m_count;
    s.slowTicks = m_slow;
    s.avgMs = m_count ? m_sumMs / static_cast<double>(m_count) : 0.0;
    s.p50Ms = quantileMs(0.50);
    s.p99Ms = quantileMs(0.99);
    s.maxMs = m_maxMs;
    return s;
}

} // namespace vo::server::world
