// Socket-free request/response bookkeeping used by GameClient (vo_client_core, unit-tested).
//
// RequestTracker: every request that expects an answer registers the response opcode with a
// deadline; an arriving message completes the oldest pending request waiting for that opcode.
// expired() reports the first request whose deadline has passed (GameClient then drops the
// connection with a "timeout" reason).
//
// PingTracker: decides when the next PING is due (immediately after connecting, then every
// interval), matches PONG echoes to the pings sent and measures the round-trip time; timedOut()
// is true when the oldest unanswered ping is older than the ping timeout.
//
// Time is passed in as monotonic milliseconds (any epoch) so tests are deterministic.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace vo::client::net {

using Millis = std::chrono::milliseconds;

class RequestTracker
{
public:
    void add(std::uint16_t responseOpcode, Millis now, Millis timeout);
    // True if `opcode` answered a pending request (which is then removed).
    bool complete(std::uint16_t opcode);
    // Response opcode of the first request past its deadline, if any.
    [[nodiscard]] std::optional<std::uint16_t> expired(Millis now) const;
    [[nodiscard]] bool isPending(std::uint16_t responseOpcode) const;
    [[nodiscard]] bool empty() const noexcept { return m_pending.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_pending.size(); }
    void clear() noexcept { m_pending.clear(); }

private:
    struct Pending
    {
        std::uint16_t opcode;
        Millis deadline;
    };
    std::deque<Pending> m_pending;
};

class PingTracker
{
public:
    explicit PingTracker(Millis interval = Millis(5000), Millis timeout = Millis(20000));

    // Starts a new connection: the first ping is due at `now`.
    void reset(Millis now);
    // If a ping is due, records it and returns the client_time_ms stamp to send.
    [[nodiscard]] std::optional<std::uint32_t> pingDue(Millis now);
    // PONG with the echoed stamp. Returns the RTT in ms if it matched an outstanding ping.
    std::optional<double> onPong(std::uint32_t echoedStamp, Millis now);
    [[nodiscard]] bool timedOut(Millis now) const;

    [[nodiscard]] std::optional<double> lastRttMs() const noexcept { return m_lastRtt; }
    // Exponential moving average (1/4 weight for each new sample).
    [[nodiscard]] std::optional<double> smoothedRttMs() const noexcept { return m_smoothedRtt; }
    [[nodiscard]] std::size_t outstanding() const noexcept { return m_outstanding.size(); }

private:
    struct Sent
    {
        std::uint32_t stamp;
        Millis at;
    };

    Millis m_interval;
    Millis m_timeout;
    Millis m_nextDue{0};
    std::deque<Sent> m_outstanding;
    std::optional<double> m_lastRtt;
    std::optional<double> m_smoothedRtt;
    std::uint32_t m_lastStamp = 0;
    bool m_active = false;
};

} // namespace vo::client::net
