#include "server/world/MoveValidator.h"

#include "shared/game/TerrainWalk.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace vo::server::world {

std::string_view moveVerdictName(MoveVerdict v) noexcept
{
    switch (v)
    {
    case MoveVerdict::Accepted: return "accepted";
    case MoveVerdict::OutOfBounds: return "out of bounds";
    case MoveVerdict::TooFar: return "too far";
    case MoveVerdict::Blocked: return "blocked";
    case MoveVerdict::PathBlocked: return "path blocked";
    case MoveVerdict::HeightMismatch: return "height mismatch";
    case MoveVerdict::Dead: return "dead";
    }
    return "?";
}

bool isPlausibleMove(const net::MoveReq& req, std::string_view& why) noexcept
{
    if (!std::isfinite(req.x) || !std::isfinite(req.y) || !std::isfinite(req.z) || !std::isfinite(req.yaw))
    {
        why = "MOVE_REQ with non-finite values";
        return false;
    }
    constexpr float kMax = game::kMoveAbsurdCoordinate;
    if (std::abs(req.x) > kMax || std::abs(req.y) > kMax || std::abs(req.z) > kMax)
    {
        why = "MOVE_REQ coordinates out of any plausible range";
        return false;
    }
    if (std::abs(req.yaw) > game::kMoveAbsurdYaw)
    {
        why = "MOVE_REQ yaw out of any plausible range";
        return false;
    }
    if ((req.flags & ~game::kMoveKnownFlags) != 0)
    {
        why = "MOVE_REQ with unknown flag bits";
        return false;
    }
    return true;
}

MoveVerdict validateMove(const assets::TerrainData& terrain, const MoveRules& rules, MoveState& state,
                         const net::MoveReq& req, core::MonoTime at)
{
    // Time/credit bookkeeping happens for every processed move, accepted or not.
    const auto elapsed = at > state.lastTime ? at - state.lastTime : core::MonoClock::duration::zero();
    if (at > state.lastTime)
        state.lastTime = at;
    const float dt = std::chrono::duration<float>(elapsed).count();
    const float speed = game::moveSpeed(req.flags, rules.walkSpeed, rules.runSpeed);
    const float cap = speed * rules.bankSeconds * rules.speedTolerance;
    state.credit = std::min(state.credit + speed * dt * rules.speedTolerance, cap);

    const auto reject = [&](MoveVerdict v) {
        state.flags = static_cast<std::uint8_t>(state.flags & ~game::kMoveMoving);
        return v;
    };

    if (!terrain.contains(req.x, req.z))
        return reject(MoveVerdict::OutOfBounds);
    const float dist = std::hypot(req.x - state.x, req.z - state.z);
    if (!(dist <= state.credit + rules.distanceSlack))
        return reject(MoveVerdict::TooFar);
    if (!terrain.isWalkable(req.x, req.z))
        return reject(MoveVerdict::Blocked);
    if (!game::isSegmentWalkable(terrain, state.x, state.z, req.x, req.z))
        return reject(MoveVerdict::PathBlocked);
    const float ground = terrain.heightAt(req.x, req.z);
    if (!(std::abs(req.y - ground) <= rules.maxHeightError))
        return reject(MoveVerdict::HeightMismatch);

    state.credit = std::max(state.credit - dist, -rules.distanceSlack);
    state.x = req.x;
    state.y = ground;
    state.z = req.z;
    state.yaw = game::normalizeYaw(req.yaw);
    state.flags = req.flags;
    return MoveVerdict::Accepted;
}

} // namespace vo::server::world
