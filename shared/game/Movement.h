// Movement rules shared by client and server (docs/PROTOCOL.md §5.4, docs/SERVER_WORLD.md): speeds,
// the validation tolerances of the authoritative server, send/relay rates, the area-of-interest grid
// and small pure helpers (speed for MOVE_REQ flags, yaw normalisation, the per-move distance bound).
// Header-only and platform-neutral.
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace vo::game {

// ---- Speeds (ENTER_WORLD_RES walk_speed / run_speed) ----
inline constexpr float kWalkSpeed = 2.0f; // m/s
inline constexpr float kRunSpeed = 5.5f;  // m/s

// ---- MOVE_REQ flags (same bits as vo::net::kMoveFlag*) ----
inline constexpr std::uint8_t kMoveMoving = 0x01;
inline constexpr std::uint8_t kMoveRunning = 0x02;
inline constexpr std::uint8_t kMoveKnownFlags = kMoveMoving | kMoveRunning;

// ---- Server-side validation (§5.4) ----
// Accept if horizontal distance <= speed * elapsed * kMoveSpeedTolerance + kMoveDistanceSlack.
inline constexpr float kMoveSpeedTolerance = 1.25f;
inline constexpr float kMoveDistanceSlack = 0.5f; // metres
inline constexpr float kMoveMaxHeightError = 1.0f; // |y - terrain(x, z)| limit before the server snaps y
// Unused movement allowance is carried over between moves (so a lagging client catching up in a
// burst is not rejected) but never more than this many seconds' worth (so it cannot be banked to
// teleport). Server default; configurable as world.move_bank_seconds.
inline constexpr float kMoveDefaultBankSeconds = 1.0f;
// Anything beyond this is not a position at all: ERROR(ProtocolViolation).
inline constexpr float kMoveAbsurdCoordinate = 100000.0f; // metres
inline constexpr float kMoveAbsurdYaw = 10000.0f;         // radians

// ---- Rates ----
inline constexpr int kMoveSendRateHz = 10;     // client MOVE_REQ while moving (+ start/stop/toggle)
inline constexpr int kEntityMoveRelayHz = 10;  // server ENTITY_MOVE per entity, at most
inline constexpr int kMaxMoveReqPerSecond = 30; // more -> ERROR(Flood) and close
inline constexpr std::chrono::milliseconds kRemoteInterpolationDelay{100};

// ---- Area of interest ----
inline constexpr float kAoiCellSize = 32.0f; // metres
inline constexpr int kAoiViewRadiusCells = 2; // 5x5 block centred on the observer's cell

[[nodiscard]] constexpr bool isMoving(std::uint8_t flags) noexcept
{
    return (flags & kMoveMoving) != 0;
}
[[nodiscard]] constexpr bool isRunning(std::uint8_t flags) noexcept
{
    return (flags & kMoveRunning) != 0;
}
// Speed the validator applies to a MOVE_REQ with these flags (bit1 running -> run, else walk).
[[nodiscard]] constexpr float moveSpeed(std::uint8_t flags, float walkSpeed = kWalkSpeed,
                                        float runSpeed = kRunSpeed) noexcept
{
    return isRunning(flags) ? runSpeed : walkSpeed;
}

// The §5.4 bound for a single move from rest: speed * elapsed * 1.25 + 0.5 m.
[[nodiscard]] constexpr float maxMoveDistance(float speed, float elapsedSeconds) noexcept
{
    return speed * elapsedSeconds * kMoveSpeedTolerance + kMoveDistanceSlack;
}

// Yaw in (-pi, pi]. Non-finite input yields 0.
[[nodiscard]] inline float normalizeYaw(float yaw) noexcept
{
    if (!std::isfinite(yaw))
        return 0.0f;
    constexpr float kPi = std::numbers::pi_v<float>;
    float r = std::remainder(yaw, 2.0f * kPi); // [-pi, pi]
    if (r <= -kPi)
        r += 2.0f * kPi;
    return r;
}

// Yaw (0 = +Z, clockwise seen from above, i.e. towards +X) of the direction (dx, dz).
[[nodiscard]] inline float yawFromDirection(float dx, float dz) noexcept
{
    return std::atan2(dx, dz);
}

} // namespace vo::game
