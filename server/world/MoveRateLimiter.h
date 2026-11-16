// Sliding-window request caps of the protocol: "more than N <request> per second -> ERROR(Flood)".
// MoveRateLimiter: §5.4, 30 MOVE_REQ per second; ItemMoveRateLimiter: PROTOCOL_ITEMS.md §4, 10
// ITEM_MOVE_REQ per second. Sliding window over receive timestamps: the (limit+1)-th message within
// any window is refused.
// Not thread-safe; one per session, used on the network thread.
#pragma once

#include "shared/core/Clock.h"
#include "shared/game/Movement.h"
#include "shared/net/Opcodes.h"

#include <array>
#include <chrono>
#include <cstddef>

namespace vo::server::world {

template <std::size_t Limit>
class SlidingWindowLimiter
{
public:
    static constexpr std::size_t kLimit = Limit;
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

using MoveRateLimiter = SlidingWindowLimiter<static_cast<std::size_t>(game::kMaxMoveReqPerSecond)>;
using ItemMoveRateLimiter = SlidingWindowLimiter<static_cast<std::size_t>(::vo::net::kMaxItemMoveReqPerSecond)>;
// PROTOCOL_COMBAT.md §4: ATTACK_REQ / SKILL_REQ / TARGET_REQ / STANCE_REQ, 10 per second combined.
using CombatRateLimiter = SlidingWindowLimiter<static_cast<std::size_t>(::vo::net::kMaxCombatReqPerSecond)>;

} // namespace vo::server::world
