// Client-predicted movement of the local character (docs/PROTOCOL.md §5.4), no D3D, no sockets.
//
//   LocalMover   integrates the character: camera-relative WASD or click-to-move, walk/run speed
//                from ENTER_WORLD_RES, slide along blocked walk-mask cells, zone bounds, y on the
//                terrain, smooth facing turn towards the direction of travel.
//   MoveSender   decides when a MOVE_REQ goes out: at 10 Hz while moving, plus immediately on start,
//                stop (moving = 0) and run/walk toggle; seq is incremented per packet; never more
//                than one packet per kMinSendInterval (well below the server's 30/s flood limit).
//   Prediction   both of the above plus POSITION_CORRECT handling: snap to the server position with
//                a short visual blend, cancel click-to-move, and ignore stale corrections (those
//                whose seq was sent before the last snap).
#pragma once

#include "client/world/WorldMath.h"
#include "shared/assets/Terrain.h"
#include "shared/game/Movement.h"

#include <cstdint>
#include <optional>

namespace vo::client::world {

struct MovementParams
{
    float walkSpeed = vo::game::kWalkSpeed; // m/s, from ENTER_WORLD_RES (this is the fallback)
    float runSpeed = vo::game::kRunSpeed;
    float turnRate = 12.0f;       // rad/s facing turn towards the travel direction
    float arriveDistance = 0.15f; // click-to-move: target reached
    float maxSubStep = 0.25f;     // metres per collision sub-step (walk mask cells are 1 m)
    // Clearance the client keeps from blocked cells (the blocked cells grown by it). Stricter than
    // the server's point test (the 1 m mask cells under-cover wall footprints by up to ~0.5 m), so
    // predicted positions are always ones the server accepts.
    float bodyRadius = 0.3f;
};

struct MoveIntent
{
    // Camera-relative stick: forward (+1 = W) and right (+1 = D). Zero vector = no key held.
    float forward = 0.0f;
    float right = 0.0f;
    float cameraYaw = 0.0f; // yaw the camera looks along
    bool running = true;
};

class LocalMover
{
public:
    void reset(Vec3 position, float yaw) noexcept;
    // Snaps to `position` (y re-snapped to the terrain if one is given).
    void setPosition(Vec3 position, float yaw, const assets::TerrainData* terrain) noexcept;

    // Click-to-move target (x, z). Cleared by WASD input, arrival, being stuck or a correction.
    void setDestination(float x, float z) noexcept;
    void clearDestination() noexcept { m_destination.reset(); }
    [[nodiscard]] const std::optional<Vec2>& destination() const noexcept { return m_destination; }

    // Advances by dt seconds. With no terrain the character moves freely (tests only).
    void step(const MoveIntent& intent, const MovementParams& params, const assets::TerrainData* terrain,
              float dt) noexcept;

    [[nodiscard]] Vec3 position() const noexcept { return m_position; }
    [[nodiscard]] float yaw() const noexcept { return m_yaw; }
    [[nodiscard]] bool moving() const noexcept { return m_moving; }
    [[nodiscard]] bool running() const noexcept { return m_running; }
    // Horizontal speed actually achieved during the last step (m/s, after sliding/blocking).
    [[nodiscard]] float speed() const noexcept { return m_speed; }
    // True if the last step wanted to move but a blocked cell stopped part of it.
    [[nodiscard]] bool blocked() const noexcept { return m_blocked; }

    // Moves from `from` by (dx, dz) in sub-steps, keeping the position out of the blocked walk-mask
    // cells grown by `radius` (and the zone border): sliding along walls and around corners. Returns the new position
    // (y on the terrain).
    [[nodiscard]] static Vec3 slideMove(const assets::TerrainData& terrain, Vec3 from, float dx, float dz,
                                        float maxSubStep, float radius, bool* blocked = nullptr) noexcept;
    // Pushes (x, z) out of every blocked cell grown by `radius` (across its nearest face). Returns
    // true if it had to push.
    static bool resolveClearance(const assets::TerrainData& terrain, float& x, float& z, float radius) noexcept;

private:
    Vec3 m_position{};
    float m_yaw = 0.0f;
    std::optional<Vec2> m_destination;
    float m_stuckTime = 0.0f;
    float m_speed = 0.0f;
    bool m_moving = false;
    bool m_running = true;
    bool m_blocked = false;
};

struct MoveReqData
{
    std::uint32_t seq = 0;
    std::uint8_t flags = 0; // bit0 moving, bit1 running
    Vec3 position{};
    float yaw = 0.0f;
};

class MoveSender
{
public:
    static constexpr double kPeriod = 0.1;           // 10 Hz while moving
    static constexpr double kMinSendInterval = 0.04; // never more than 25 packets per second

    void reset() noexcept;
    // Call once per frame after the movement step (`now` in seconds, monotonic). Returns the packet
    // to send this frame, if any.
    [[nodiscard]] std::optional<MoveReqData> update(double now, bool moving, bool running, Vec3 position,
                                                    float yaw) noexcept;

    [[nodiscard]] std::uint32_t lastSeq() const noexcept { return m_seq; }
    [[nodiscard]] std::uint32_t sentCount() const noexcept { return m_sent; }

private:
    std::uint32_t m_seq = 0;
    std::uint32_t m_sent = 0;
    double m_lastSendTime = -1.0e9;
    double m_nextDue = 0.0; // next periodic (10 Hz) packet while moving
    bool m_lastMoving = false;
    bool m_lastRunning = true;
    bool m_hasSent = false;
    bool m_immediatePending = false;
};

// Local player state: mover + sender + correction handling.
class Prediction
{
public:
    static constexpr float kCorrectionBlendTime = 0.2f; // seconds the visual offset takes to fade

    void reset(Vec3 position, float yaw, bool running) noexcept;

    LocalMover& mover() noexcept { return m_mover; }
    [[nodiscard]] const LocalMover& mover() const noexcept { return m_mover; }

    // One frame: movement step, visual-offset decay and MOVE_REQ scheduling.
    [[nodiscard]] std::optional<MoveReqData> update(double now, const MoveIntent& intent,
                                                    const MovementParams& params,
                                                    const assets::TerrainData* terrain, float dt) noexcept;

    // POSITION_CORRECT. Returns false if ignored as stale (seq <= last seq sent before the previous
    // snap). On apply: snap, drop the click-to-move target, keep the old visual position as an
    // offset that fades out over kCorrectionBlendTime.
    bool applyCorrection(std::uint32_t seq, Vec3 position, float yaw, const assets::TerrainData* terrain) noexcept;
    // Server-placed jump (M9 RESPAWN_RES): snap without a blend, drop the click target, and treat
    // corrections answering earlier MOVE_REQs as stale.
    void teleport(Vec3 position, float yaw, const assets::TerrainData* terrain) noexcept;

    // Where to draw the character (simulation position + fading correction offset).
    [[nodiscard]] Vec3 visualPosition() const noexcept { return m_mover.position() + m_visualOffset; }
    [[nodiscard]] std::uint32_t correctionsApplied() const noexcept { return m_corrections; }
    [[nodiscard]] std::uint32_t correctionsIgnored() const noexcept { return m_ignored; }
    [[nodiscard]] const MoveSender& sender() const noexcept { return m_sender; }

private:
    LocalMover m_mover;
    MoveSender m_sender;
    Vec3 m_visualOffset{};
    std::uint32_t m_ignoreUpToSeq = 0;
    bool m_snapped = false;
    std::uint32_t m_corrections = 0;
    std::uint32_t m_ignored = 0;
};

} // namespace vo::client::world
