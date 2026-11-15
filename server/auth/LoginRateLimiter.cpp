#include "server/auth/LoginRateLimiter.h"

#include <utility>

namespace vo::server::auth {

LoginRateLimiter::LoginRateLimiter(std::uint32_t maxFailures, std::chrono::milliseconds window, TimeSource now)
    : m_maxFailures(maxFailures), m_window(window), m_now(std::move(now))
{
}

void LoginRateLimiter::pruneLocked(std::deque<core::MonoTime>& times, core::MonoTime now) const
{
    while (!times.empty() && now - times.front() >= m_window)
        times.pop_front();
}

void LoginRateLimiter::purgeLocked(core::MonoTime now)
{
    for (auto it = m_failures.begin(); it != m_failures.end();)
    {
        pruneLocked(it->second, now);
        if (it->second.empty())
            it = m_failures.erase(it);
        else
            ++it;
    }
}

bool LoginRateLimiter::isBlocked(const std::string& address)
{
    const std::scoped_lock lock(m_mutex);
    const auto it = m_failures.find(address);
    if (it == m_failures.end())
        return false;
    pruneLocked(it->second, m_now());
    if (it->second.empty())
    {
        m_failures.erase(it);
        return false;
    }
    return it->second.size() >= m_maxFailures;
}

void LoginRateLimiter::recordFailure(const std::string& address)
{
    const std::scoped_lock lock(m_mutex);
    const auto now = m_now();
    if (++m_opsSincePurge >= 1024)
    {
        m_opsSincePurge = 0;
        purgeLocked(now);
    }
    auto& times = m_failures[address];
    pruneLocked(times, now);
    times.push_back(now);
}

std::size_t LoginRateLimiter::trackedAddresses() const
{
    const std::scoped_lock lock(m_mutex);
    return m_failures.size();
}

} // namespace vo::server::auth
