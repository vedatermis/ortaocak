// Authoritative movement validation (docs/PROTOCOL.md §5.4, docs/SERVER_WORLD.md §4).
//
//   isPlausibleMove()  stateless sanity check run on the network thread; failure is a protocol
//                      violation (non-finite or absurd numbers, unknown flag bits).
//   validateMove()     the §5.4 rules against the terrain and the player's MoveState: distance
//                      budget, zone bounds, walk mask incl. the straight path, height tolerance.
//
// Distance budget ("credit"): every processed MOVE_REQ first accrues speed x elapsed x 1.25 metres,
// elapsed being the server-side time since the previous processed MOVE_REQ (receive timestamps),
// then the move is accepted if its horizontal distance <= credit + 0.5 m and the distance is
// subtracted. Unused credit carries over, so a burst of delayed packets is accepted exactly like the
// same packets arriving on time, but it is capped at speed x bankSeconds x 1.25, so standing still
// cannot be saved up to teleport. The 0.5 m slack is a one-off tolerance (credit may go down to
// -0.5 m), not a per-packet bonus, so spamming small moves cannot exceed 1.25x speed either. For a
// single move from rest this is exactly the §5.4 formula (up to the bank cap).
#pragma once

#include "shared/assets/Terrain.h"
#include "shared/core/Clock.h"
#include "shared/game/Movement.h"
#include "shared/net/Messages.h"

#include <cstdint>
#include <string_view>

namespace vo::server::world {

struct MoveRules
{
    float walkSpeed = game::kWalkSpeed;
    float runSpeed = game::kRunSpeed;
    float speedTolerance = game::kMoveSpeedTolerance;
    float distanceSlack = game::kMoveDistanceSlack;
    float maxHeightError = game::kMoveMaxHeightError;
    float bankSeconds = game::kMoveDefaultBankSeconds;
};

struct MoveState
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    std::uint8_t flags = 0;
    core::MonoTime lastTime{}; // last processed MOVE_REQ (or entering the world)
    float credit = 0.0f;       // metres of movement still allowed (see above)
};

enum class MoveVerdict : std::uint8_t
{
    Accepted,
    OutOfBounds,    // outside the zone
    TooFar,         // distance budget exceeded (speed hack, teleport)
    Blocked,        // target cell is not walkable
    PathBlocked,    // straight path crosses a blocked cell
    HeightMismatch, // |y - terrain| > 1 m
    Dead,           // the player is dead (PROTOCOL_COMBAT.md §2): not validated at all
    Ignored,        // the player is loading a zone it changed to (PROTOCOL_ZONES.md §3.4): dropped, no correction
};

[[nodiscard]] std::string_view moveVerdictName(MoveVerdict v) noexcept;

// False (with a reason) for values no honest client can produce: non-finite floats, coordinates
// beyond +-100 km, |yaw| > 10000 rad, unknown flag bits.
[[nodiscard]] bool isPlausibleMove(const net::MoveReq& req, std::string_view& why) noexcept;

// Applies the §5.4 rules. On Accepted the state holds the new position (y snapped to the terrain,
// yaw normalised to (-pi, pi], the request's flags). On rejection the position is unchanged, the
// moving flag is cleared (the player stands at the corrected position) and only the time/credit
// bookkeeping advances. `at` must not go backwards between calls (earlier values count as `lastTime`).
[[nodiscard]] MoveVerdict validateMove(const assets::TerrainData& terrain, const MoveRules& rules, MoveState& state,
                                       const net::MoveReq& req, core::MonoTime at);

} // namespace vo::server::world
