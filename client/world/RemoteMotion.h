// Remote entity motion (docs/PROTOCOL.md §5.4): ENTITY_MOVE snapshots stamped with server_time_ms
// are buffered and rendered kInterpolationDelay in the past, interpolating between the two
// snapshots around the render time. Past the newest snapshot the entity is extrapolated along its
// last velocity for at most kMaxExtrapolation (only if the newest snapshot says "moving"), then held.
// No D3D, no sockets; time is passed in.
#pragma once

#include "client/world/WorldMath.h"

#include <array>
#include <cstdint>

namespace vo::client::world {

// Maps server_time_ms (u32, wrapping) onto the client's monotonic clock. The offset estimate is the
// largest (server - local) seen, i.e. the least-delayed packet, decaying slowly so clock drift and
// route changes are followed.
class ServerClock
{
public:
    void reset() noexcept { m_valid = false; }
    // A packet stamped `serverMs` arrived at local time `localSeconds`.
    void observe(std::uint32_t serverMs, double localSeconds) noexcept;
    [[nodiscard]] bool valid() const noexcept { return m_valid; }
    // Server time (seconds, unwrapped, continuous) of a server stamp.
    [[nodiscard]] double toSeconds(std::uint32_t serverMs) const noexcept;
    // Estimated current server time in seconds (same scale as toSeconds()).
    [[nodiscard]] double now(double localSeconds) const noexcept;
    // Inverse of toSeconds() (debug ghosts stamp their synthetic packets with it).
    [[nodiscard]] std::uint32_t toServerMs(double seconds) const noexcept;

private:
    bool m_valid = false;
    std::uint32_t m_baseMs = 0; // first stamp; times are unwrapped relative to it
    double m_offset = 0.0;      // server seconds - local seconds
    double m_lastLocal = 0.0;
};

struct MotionSample
{
    Vec3 position{};
    float yaw = 0.0f;
    Vec3 velocity{}; // m/s of the segment being played (0 when holding)
    std::uint8_t flags = 0;
    bool extrapolating = false;
    bool holding = false; // past the extrapolation cap or stopped: standing at the newest snapshot
};

class SnapshotBuffer
{
public:
    static constexpr double kInterpolationDelay = 0.1; // seconds behind the newest server time
    static constexpr double kMaxExtrapolation = 0.25;  // then hold
    static constexpr float kTeleportDistance = 12.0f;  // larger jumps between snapshots snap
    static constexpr std::size_t kCapacity = 32;

    void clear() noexcept { m_count = 0; }
    // Adds a snapshot at server time `t` (seconds). Older-than-newest snapshots are dropped. A long
    // gap after a stop inserts a "still standing" copy so the start of a new walk does not crawl.
    void push(double t, Vec3 position, float yaw, std::uint8_t flags) noexcept;
    // State at server time `renderTime` (normally ServerClock::now() - kInterpolationDelay).
    [[nodiscard]] MotionSample sample(double renderTime) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return m_count; }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }

private:
    struct Snapshot
    {
        double t = 0.0;
        Vec3 position{};
        float yaw = 0.0f;
        std::uint8_t flags = 0;
    };
    [[nodiscard]] const Snapshot& at(std::size_t i) const noexcept { return m_ring[(m_head + i) % kCapacity]; }
    void append(const Snapshot& s) noexcept;

    std::array<Snapshot, kCapacity> m_ring{};
    std::size_t m_head = 0;  // oldest
    std::size_t m_count = 0;
};

} // namespace vo::client::world
