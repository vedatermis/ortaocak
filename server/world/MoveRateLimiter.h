// MoveRateLimiter: the §5.4 hard cap "more than 30 MOVE_REQ per second -> ERROR(Flood)". Sliding
// window over receive timestamps: the (limit+1)-th message within any window is refused.
// Not thread-safe; one per session, used on the network thread.
#pragma once

#include "shared/core/Clock.h"
#include "shared/game/Movement.h"

#include <array>
#include <chrono>
#include <cstddef>

namespace vo::server::world {

class MoveRateLimiter
{
public:
    static constexpr std::size_t kLimit = static_cast<std::size_t>(game::kMaxMoveReqPerSecond);
    static constexpr core::MonoClock::duration kWindow = std::chrono::seconds(1);

    // Records a message received at `at` (non-decreasing). False if it exceeds the cap.
    [[nodiscard]] bool allow(core::MonoTime at) noexcept
    {
        if (m_count == kLimit)
        {
            const core::MonoTime oldest = m_times[m_head];
            if (at - oldest < kWindow)
                return false;
            m_times[m_head] = at; // overwrite the oldest
            m_head = (m_head + 1) % kLimit;
            return true;
        }
        m_times[(m_head + m_count) % kLimit] = at;
        ++m_count;
        return true;
    }

    void reset() noexcept
    {
        m_head = 0;
        m_count = 0;
    }

private:
    std::array<core::MonoTime, kLimit> m_times{};
    std::size_t m_head = 0;  // index of the oldest timestamp
    std::size_t m_count = 0; // timestamps stored (<= kLimit)
};

} // namespace vo::server::world
