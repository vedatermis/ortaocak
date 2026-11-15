#include "client/world/LocalMovement.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vo::client::world {

// ---- LocalMover ----

void LocalMover::reset(Vec3 position, float yaw) noexcept
{
    m_position = position;
    m_yaw = wrapAngle(yaw);
    m_destination.reset();
    m_stuckTime = 0.0f;
    m_speed = 0.0f;
    m_moving = false;
    m_blocked = false;
}

void LocalMover::setPosition(Vec3 position, float yaw, const assets::TerrainData* terrain) noexcept
{
    m_position = position;
    if (terrain != nullptr && terrain->resolution >= 2)
        m_position.y = terrain->heightAt(position.x, position.z);
    m_yaw = wrapAngle(yaw);
    m_speed = 0.0f;
}

void LocalMover::setDestination(float x, float z) noexcept
{
    m_destination = Vec2{x, z};
    m_stuckTime = 0.0f;
}

bool LocalMover::resolveClearance(const assets::TerrainData& terrain, float& x, float& z, float radius) noexcept
{
    if (terrain.maskResolution == 0 || !(terrain.worldSize > 0.0f))
        return false;
    const float cell = terrain.worldSize / static_cast<float>(terrain.maskResolution);
    constexpr float kEps = 1e-4f;
    bool pushedAny = false;
    // A few passes: a push out of one cell's box can land in a neighbour's (inside corners).
    for (int pass = 0; pass < 4; ++pass)
    {
        bool pushed = false;
        const int x0 = static_cast<int>(std::floor((x - radius) / cell));
        const int x1 = static_cast<int>(std::floor((x + radius) / cell));
        const int z0 = static_cast<int>(std::floor((z - radius) / cell));
        const int z1 = static_cast<int>(std::floor((z + radius) / cell));
        for (int cz = z0; cz <= z1; ++cz)
        {
            for (int cx = x0; cx <= x1; ++cx)
            {
                const float minX = static_cast<float>(cx) * cell;
                const float minZ = static_cast<float>(cz) * cell;
                if (terrain.isWalkable(minX + 0.5f * cell, minZ + 0.5f * cell))
                    continue; // outside the zone counts as blocked too (isWalkable is false there)
                // The blocked cell grown by the radius; leave it across the nearest side.
                const float l = x - (minX - radius);
                const float r = (minX + cell + radius) - x;
                const float d = z - (minZ - radius);
                const float u = (minZ + cell + radius) - z;
                if (l <= 0.0f || r <= 0.0f || d <= 0.0f || u <= 0.0f)
                    continue;
                const float m = std::min(std::min(l, r), std::min(d, u));
                if (m == l)
                    x = minX - radius - kEps;
                else if (m == r)
                    x = minX + cell + radius + kEps;
                else if (m == d)
                    z = minZ - radius - kEps;
                else
                    z = minZ + cell + radius + kEps;
                pushed = true;
            }
        }
        pushedAny = pushedAny || pushed;
        if (!pushed)
            break;
    }
    return pushedAny;
}

Vec3 LocalMover::slideMove(const assets::TerrainData& terrain, Vec3 from, float dx, float dz, float maxSubStep,
                           float radius, bool* blocked) noexcept
{
    if (blocked != nullptr)
        *blocked = false;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len <= 0.0f)
        return from;
    const int steps = std::max(1, static_cast<int>(std::ceil(len / std::max(maxSubStep, 0.01f))));
    const float sx = dx / static_cast<float>(steps);
    const float sz = dz / static_cast<float>(steps);
    // Standing on a blocked cell itself (spawned inside a footprint, or the mask changed): let the
    // character walk out freely instead of freezing it.
    const bool escaping = !terrain.isWalkable(from.x, from.z);
    Vec3 p = from;
    for (int i = 0; i < steps; ++i)
    {
        float x = p.x + sx;
        float z = p.z + sz;
        if (!escaping)
        {
            // The position against the blocked cells grown by the radius: the wish is pushed out
            // across the nearest face, which keeps the tangential part of the move: sliding along
            // walls, rounding corners hit diagonally, stopping when walking straight into a wall.
            if (resolveClearance(terrain, x, z, radius) && blocked != nullptr)
                *blocked = true;
            if (!terrain.isWalkable(x, z))
            {
                if (blocked != nullptr)
                    *blocked = true;
                break; // could not be resolved (a pocket narrower than the body): stay
            }
        }
        p.x = x;
        p.z = z;
    }    p.y = terrain.heightAt(p.x, p.z);
    return p;
}

void LocalMover::step(const MoveIntent& intent, const MovementParams& params, const assets::TerrainData* terrain,
                      float dt) noexcept
{
    m_running = intent.running;
    m_blocked = false;
    if (!(dt > 0.0f))
        return;

    const float stick = std::sqrt(intent.forward * intent.forward + intent.right * intent.right);
    Vec3 dir{};
    bool wantMove = false;
    float maxTravel = std::numeric_limits<float>::max();
    if (stick > 1e-3f)
    {
        m_destination.reset(); // WASD cancels click-to-move
        const Vec3 f = forwardFromYaw(intent.cameraYaw);
        const Vec3 r = rightFromYaw(intent.cameraYaw);
        dir = assets::normalize(f * intent.forward + r * intent.right);
        wantMove = true;
    }
    else if (m_destination)
    {
        const float dx = m_destination->x - m_position.x;
        const float dz = m_destination->y - m_position.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist <= params.arriveDistance)
        {
            m_destination.reset();
        }
        else
        {
            dir = {dx / dist, 0.0f, dz / dist};
            maxTravel = dist;
            wantMove = true;
        }
    }

    if (!wantMove)
    {
        m_moving = false;
        m_speed = 0.0f;
        m_stuckTime = 0.0f;
        return;
    }

    const float speed = m_running ? params.runSpeed : params.walkSpeed;
    const float travel = std::min(speed * dt, maxTravel);
    const Vec3 before = m_position;
    if (terrain != nullptr && terrain->resolution >= 2)
    {
        bool blocked = false;
        m_position = slideMove(*terrain, m_position, dir.x * travel, dir.z * travel, params.maxSubStep,
                               params.bodyRadius, &blocked);
        m_blocked = blocked;
    }
    else
    {
        m_position = m_position + dir * travel;
    }
    const float actual = horizontalDistance(m_position, before);
    m_speed = actual / dt;
    m_moving = actual > 1e-5f;

    // Face the way the character actually goes (along a wall while sliding), else the wish.
    float targetYaw = yawFromDirection(dir.x, dir.z);
    if (actual > 0.2f * travel)
        targetYaw = yawFromDirection(m_position.x - before.x, m_position.z - before.z);
    m_yaw = approachAngle(m_yaw, targetYaw, params.turnRate * dt);

    if (m_destination)
    {
        if (actual < 0.2f * travel)
        {
            m_stuckTime += dt;
            if (m_stuckTime > 0.5f)
                m_destination.reset(); // blocked: give up rather than push forever
        }
        else
        {
            m_stuckTime = 0.0f;
        }
    }
}

// ---- MoveSender ----

void MoveSender::reset() noexcept
{
    *this = MoveSender{};
}

std::optional<MoveReqData> MoveSender::update(double now, bool moving, bool running, Vec3 position,
                                              float yaw) noexcept
{
    if (!m_hasSent)
    {
        // Nothing sent yet: the server knows the spawn position. The first packet is the start.
        m_lastRunning = running;
        m_hasSent = true; // "state as last sent" = standing at the spawn
        m_lastMoving = false;
        m_nextDue = now;
    }
    if (moving != m_lastMoving || running != m_lastRunning)
        m_immediatePending = true; // start, stop or run/walk toggle
    const bool periodicDue = moving && now >= m_nextDue - 1e-6;
    if (!m_immediatePending && !periodicDue)
        return std::nullopt;
    if (now - m_lastSendTime < kMinSendInterval - 1e-9)
        return std::nullopt; // rate guard: goes out on a later frame

    MoveReqData out;
    out.seq = ++m_seq;
    out.flags = static_cast<std::uint8_t>((moving ? 0x01 : 0x00) | (running ? 0x02 : 0x00));
    out.position = position;
    out.yaw = yaw;
    if (m_immediatePending || now >= m_nextDue + kPeriod)
        m_nextDue = now + kPeriod; // restart the 10 Hz cadence (also after falling far behind)
    else
        m_nextDue = std::max(m_nextDue + kPeriod, now + kMinSendInterval);
    m_lastSendTime = now;
    m_lastMoving = moving;
    m_lastRunning = running;
    m_immediatePending = false;
    ++m_sent;
    return out;
}

// ---- Prediction ----

void Prediction::reset(Vec3 position, float yaw, bool running) noexcept
{
    m_mover.reset(position, yaw);
    m_sender.reset();
    (void)running;
    m_visualOffset = {};
    m_ignoreUpToSeq = 0;
    m_snapped = false;
    m_corrections = 0;
    m_ignored = 0;
}

std::optional<MoveReqData> Prediction::update(double now, const MoveIntent& intent, const MovementParams& params,
                                              const assets::TerrainData* terrain, float dt) noexcept
{
    m_mover.step(intent, params, terrain, dt);
    const float k = 1.0f - smoothingFactor(5.0f / kCorrectionBlendTime, dt);
    m_visualOffset = m_visualOffset * k;
    if (assets::dot(m_visualOffset, m_visualOffset) < 1e-6f)
        m_visualOffset = {};
    return m_sender.update(now, m_mover.moving(), m_mover.running(), m_mover.position(), m_mover.yaw());
}

bool Prediction::applyCorrection(std::uint32_t seq, Vec3 position, float yaw, const assets::TerrainData* terrain) noexcept
{
    if (m_snapped && seq <= m_ignoreUpToSeq)
    {
        ++m_ignored; // answers a MOVE_REQ sent before our last snap: already superseded
        return false;
    }
    const Vec3 oldVisual = visualPosition();
    m_mover.setPosition(position, yaw, terrain);
    m_mover.clearDestination();
    m_visualOffset = oldVisual - m_mover.position();
    if (assets::length(m_visualOffset) > 8.0f)
        m_visualOffset = {}; // far jump (teleport-like): no blend
    m_ignoreUpToSeq = m_sender.lastSeq();
    m_snapped = true;
    ++m_corrections;
    return true;
}

} // namespace vo::client::world
