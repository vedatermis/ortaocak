// LoginRateLimiter: failed-login limit per IP address (docs/PROTOCOL.md §5.2: 5 failures per IP per
// minute -> TooManyAttempts). Sliding window; successful logins do not reset the count.
// Thread-safe. Time is injectable for tests.
#pragma once

#include "shared/core/Clock.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vo::server::auth {

class LoginRateLimiter
{
public:
    using TimeSource = std::function<core::MonoTime()>;

    explicit LoginRateLimiter(std::uint32_t maxFailures = 5,
                              std::chrono::milliseconds window = std::chrono::minutes(1),
                              TimeSource now = &core::monoNow);

    // True if `address` already has maxFailures failures inside the window.
    [[nodiscard]] bool isBlocked(const std::string& address);
    void recordFailure(const std::string& address);

    [[nodiscard]] std::size_t trackedAddresses() const;

private:
    void pruneLocked(std::deque<core::MonoTime>& times, core::MonoTime now) const;
    void purgeLocked(core::MonoTime now);

    std::uint32_t m_maxFailures;
    std::chrono::milliseconds m_window;
    TimeSource m_now;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::deque<core::MonoTime>> m_failures;
    std::uint32_t m_opsSincePurge = 0;
};

} // namespace vo::server::auth
