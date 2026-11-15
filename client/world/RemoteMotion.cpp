#include "client/world/RemoteMotion.h"

#include <algorithm>
#include <cmath>

namespace vo::client::world {

namespace {
constexpr double kOffsetDecayPerSecond = 0.01; // how fast a larger delay is accepted (s per s)
constexpr double kStillGap = 0.25;             // a gap longer than this after a stop inserts a hold
} // namespace

// ---- ServerClock ----

void ServerClock::observe(std::uint32_t serverMs, double localSeconds) noexcept
{
    if (!m_valid)
    {
        m_baseMs = serverMs;
        m_valid = true;
        m_offset = toSeconds(serverMs) - localSeconds;
        m_lastLocal = localSeconds;
        return;
    }
    const double sample = toSeconds(serverMs) - localSeconds;
    if (sample > m_offset)
    {
        m_offset = sample;
    }
    else
    {
        const double elapsed = std::max(0.0, localSeconds - m_lastLocal);
        m_offset -= std::min(m_offset - sample, kOffsetDecayPerSecond * elapsed);
    }
    m_lastLocal = localSeconds;
}

double ServerClock::toSeconds(std::uint32_t serverMs) const noexcept
{
    // Signed 32-bit difference: correct across the u32 wrap for stamps within +-24 days of the base.
    const auto diff = static_cast<std::int32_t>(serverMs - m_baseMs);
    return static_cast<double>(diff) / 1000.0;
}

double ServerClock::now(double localSeconds) const noexcept
{
    return localSeconds + m_offset;
}

std::uint32_t ServerClock::toServerMs(double seconds) const noexcept
{
    const auto ms = static_cast<std::int64_t>(std::llround(seconds * 1000.0));
    return m_baseMs + static_cast<std::uint32_t>(ms);
}

// ---- SnapshotBuffer ----

void SnapshotBuffer::append(const Snapshot& s) noexcept
{
    if (m_count == kCapacity)
    {
        m_head = (m_head + 1) % kCapacity;
        --m_count;
    }
    m_ring[(m_head + m_count) % kCapacity] = s;
    ++m_count;
}

void SnapshotBuffer::push(double t, Vec3 position, float yaw, std::uint8_t flags) noexcept
{
    if (m_count > 0)
    {
        const Snapshot& newest = at(m_count - 1);
        if (t <= newest.t)
            return; // duplicate or out of order
        if (assets::length(position - newest.position) > kTeleportDistance)
        {
            clear(); // teleport / correction on the server: no sliding across the map
        }
        else if ((newest.flags & 0x01) == 0 && t - newest.t > kStillGap)
        {
            Snapshot still = newest; // it stood still until (about) one update interval ago
            still.t = std::max(newest.t, t - 0.1);
            if (still.t > newest.t)
                append(still);
        }
    }
    append(Snapshot{t, position, wrapAngle(yaw), flags});
}

MotionSample SnapshotBuffer::sample(double renderTime) const noexcept
{
    MotionSample out;
    if (m_count == 0)
        return out;
    const Snapshot& oldest = at(0);
    const Snapshot& newest = at(m_count - 1);
    if (m_count == 1 || renderTime <= oldest.t)
    {
        const Snapshot& s = m_count == 1 ? newest : oldest;
        out.position = s.position;
        out.yaw = s.yaw;
        out.flags = s.flags;
        out.holding = renderTime >= s.t;
        return out;
    }
    if (renderTime < newest.t)
    {
        // Interpolate within the bracketing pair.
        std::size_t i = m_count - 2;
        while (i > 0 && at(i).t > renderTime)
            --i;
        const Snapshot& a = at(i);
        const Snapshot& b = at(i + 1);
        const double span = b.t - a.t;
        const float f = span > 1e-6 ? static_cast<float>((renderTime - a.t) / span) : 1.0f;
        out.position = assets::lerp(a.position, b.position, std::clamp(f, 0.0f, 1.0f));
        out.yaw = lerpAngle(a.yaw, b.yaw, std::clamp(f, 0.0f, 1.0f));
        out.velocity = span > 1e-6 ? (b.position - a.position) * static_cast<float>(1.0 / span) : Vec3{};
        out.flags = b.flags;
        return out;
    }

    // Past the newest snapshot.
    const Snapshot& prev = at(m_count - 2);
    const double span = newest.t - prev.t;
    const Vec3 velocity =
        span > 1e-6 ? (newest.position - prev.position) * static_cast<float>(1.0 / span) : Vec3{};
    out.yaw = newest.yaw;
    out.flags = newest.flags;
    const bool moving = (newest.flags & 0x01) != 0;
    if (!moving)
    {
        out.position = newest.position;
        out.holding = true;
        return out;
    }
    const double ahead = renderTime - newest.t;
    if (ahead <= kMaxExtrapolation)
    {
        out.position = newest.position + velocity * static_cast<float>(ahead);
        out.velocity = velocity;
        out.extrapolating = true;
        return out;
    }
    // Updates stopped: hold where the brief extrapolation ended.
    out.position = newest.position + velocity * static_cast<float>(kMaxExtrapolation);
    out.holding = true;
    return out;
}

} // namespace vo::client::world
