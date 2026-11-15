#include "client/net/RequestTracker.h"

#include <algorithm>

namespace vo::client::net {

// ---- RequestTracker ----

void RequestTracker::add(std::uint16_t responseOpcode, Millis now, Millis timeout)
{
    m_pending.push_back({responseOpcode, now + timeout});
}

bool RequestTracker::complete(std::uint16_t opcode)
{
    const auto it = std::find_if(m_pending.begin(), m_pending.end(),
                                 [opcode](const Pending& p) { return p.opcode == opcode; });
    if (it == m_pending.end())
        return false;
    m_pending.erase(it);
    return true;
}

std::optional<std::uint16_t> RequestTracker::expired(Millis now) const
{
    for (const Pending& p : m_pending)
    {
        if (now >= p.deadline)
            return p.opcode;
    }
    return std::nullopt;
}

bool RequestTracker::isPending(std::uint16_t responseOpcode) const
{
    return std::any_of(m_pending.begin(), m_pending.end(),
                       [responseOpcode](const Pending& p) { return p.opcode == responseOpcode; });
}

// ---- PingTracker ----

PingTracker::PingTracker(Millis interval, Millis timeout) : m_interval(interval), m_timeout(timeout) {}

void PingTracker::reset(Millis now)
{
    m_outstanding.clear();
    m_lastRtt.reset();
    m_smoothedRtt.reset();
    m_nextDue = now;
    m_active = true;
}

std::optional<std::uint32_t> PingTracker::pingDue(Millis now)
{
    if (!m_active || now < m_nextDue)
        return std::nullopt;
    // Wire stamp: low 32 bits of the monotonic clock. Two pings in the same millisecond still get
    // distinct stamps so their PONGs can be told apart.
    auto stamp = static_cast<std::uint32_t>(now.count());
    if (!m_outstanding.empty() && stamp == m_lastStamp)
        ++stamp;
    m_lastStamp = stamp;
    m_outstanding.push_back({stamp, now});
    // Keep the bookkeeping bounded if the server never answers (timedOut() reports it).
    while (m_outstanding.size() > 16)
        m_outstanding.pop_front();
    m_nextDue = now + m_interval;
    return stamp;
}

std::optional<double> PingTracker::onPong(std::uint32_t echoedStamp, Millis now)
{
    const auto it = std::find_if(m_outstanding.begin(), m_outstanding.end(),
                                 [echoedStamp](const Sent& s) { return s.stamp == echoedStamp; });
    if (it == m_outstanding.end())
        return std::nullopt;
    const double rtt = static_cast<double>((now - it->at).count());
    // Older unanswered pings are superseded by this answer.
    m_outstanding.erase(m_outstanding.begin(), it + 1);
    m_lastRtt = rtt;
    m_smoothedRtt = m_smoothedRtt ? (*m_smoothedRtt * 0.75 + rtt * 0.25) : rtt;
    return rtt;
}

bool PingTracker::timedOut(Millis now) const
{
    return m_active && !m_outstanding.empty() && now - m_outstanding.front().at >= m_timeout;
}

} // namespace vo::client::net
