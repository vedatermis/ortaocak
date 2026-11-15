// The client's view of the world (no D3D, no sockets): the predicted local player and the remote
// entities, fed by the world messages of docs/PROTOCOL.md §5.4 exactly as they arrive from the
// network (ENTITY_SPAWN / ENTITY_MOVE / ENTITY_DESPAWN / POSITION_CORRECT). The scene passes every
// world message to handleMessage() and every frame calls updateLocal() (which may produce a
// MOVE_REQ to send) and updateRemotes().
//
// Server-behaviour assumptions (checked against the server half):
//   * ENTER_WORLD_RES y is ignored: the character is snapped to heightAt(x, z) of the zone terrain.
//   * ENTITY_MOVE.server_time_ms is the server's monotonic clock in ms (wrapping u32); the client
//     renders remotes kInterpolationDelay (100 ms) behind its estimate of that clock.
//   * ENTITY_MOVE / DESPAWN for an unknown id are ignored; a SPAWN for a known id replaces it; a SPAWN
//     of our own entity id is ignored.
//   * POSITION_CORRECT.seq is the last MOVE_REQ seq the server processed; a correction whose seq
//     was sent before the client's previous snap is stale and ignored (see Prediction).
#pragma once

#include "client/world/Animator.h"
#include "client/world/LocalMovement.h"
#include "client/world/RemoteMotion.h"
#include "shared/assets/Terrain.h"
#include "shared/net/Messages.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vo::client::world {

struct RemoteEntity
{
    bool active = false;
    std::uint32_t id = 0;
    std::uint8_t type = 0;
    std::string name;
    std::uint8_t race = 0;
    std::uint8_t charClass = 0;
    std::uint8_t face = 0;
    std::uint8_t hair = 0;
    std::uint16_t level = 0;
    SnapshotBuffer motion;
    // Render state, updated by WorldModel::updateRemotes().
    Vec3 position{};
    float yaw = 0.0f;
    float speed = 0.0f; // horizontal m/s of the interpolated motion
    bool running = false;
    bool moving = false;
    bool extrapolating = false;
    bool holding = false;
    float age = 0.0f; // seconds since spawn (fade-in)
    // Owned by the scene's renderer side (bound once the race's assets are loaded).
    Animator animator;
    std::uint8_t animatorRace = 0; // race the animator is bound to (0 = unbound)
};

struct WorldStats
{
    std::uint32_t spawns = 0;
    std::uint32_t despawns = 0;
    std::uint32_t moves = 0;
    std::uint32_t corrections = 0;
    std::uint32_t ignoredCorrections = 0;
    std::uint32_t unknownEntity = 0;
    std::uint32_t decodeErrors = 0;
};

class WorldModel
{
public:
    static constexpr std::size_t kMaxEntities = 512;
    static constexpr double kSpawnBackdate = 0.25; // first snapshot of a spawn, seconds before "now"

    WorldModel();

    // ENTER_WORLD_RES (result ok). `terrain` may be null (tests): then y is taken as sent.
    void enter(const vo::net::EnterWorldRes& res, const assets::TerrainData* terrain, double localNow, bool running);

    // Any world message (opcode + complete payload). Returns false on a malformed payload or a
    // non-world opcode; the caller decides what that means (the scene drops the connection).
    bool handleMessage(std::uint16_t opcode, std::span<const std::uint8_t> payload, double localNow);
    void handleSpawn(const vo::net::EntitySpawn& m, double localNow);
    void handleMove(const vo::net::EntityMove& m, double localNow);
    void handleDespawn(const vo::net::EntityDespawn& m);
    void handleCorrection(const vo::net::PositionCorrect& m);

    // Local player step; returns the MOVE_REQ to send this frame, if any.
    [[nodiscard]] std::optional<vo::net::MoveReq> updateLocal(double localNow, const MoveIntent& intent, float dt);
    // Samples every remote entity at the interpolation time.
    void updateRemotes(double localNow, float dt);

    void setTerrain(const assets::TerrainData* terrain) noexcept { m_terrain = terrain; }
    [[nodiscard]] const assets::TerrainData* terrain() const noexcept { return m_terrain; }

    [[nodiscard]] Prediction& local() noexcept { return m_local; }
    [[nodiscard]] const Prediction& local() const noexcept { return m_local; }
    [[nodiscard]] const MovementParams& params() const noexcept { return m_params; }
    [[nodiscard]] MovementParams& params() noexcept { return m_params; }
    [[nodiscard]] std::uint32_t localEntityId() const noexcept { return m_localId; }
    [[nodiscard]] std::uint16_t zoneId() const noexcept { return m_zoneId; }
    [[nodiscard]] const ServerClock& clock() const noexcept { return m_clock; }

    // Slots (inactive ones included); iterate and skip !active.
    [[nodiscard]] std::span<RemoteEntity> entities() noexcept { return m_entities; }
    [[nodiscard]] std::span<const RemoteEntity> entities() const noexcept { return m_entities; }
    [[nodiscard]] std::size_t activeCount() const noexcept { return m_active; }
    [[nodiscard]] RemoteEntity* find(std::uint32_t id) noexcept;
    [[nodiscard]] const WorldStats& stats() const noexcept { return m_stats; }

private:
    const assets::TerrainData* m_terrain = nullptr;
    MovementParams m_params;
    Prediction m_local;
    ServerClock m_clock;
    std::uint32_t m_localId = 0;
    std::uint16_t m_zoneId = 0;
    std::vector<RemoteEntity> m_entities; // capacity kMaxEntities, slots reused
    std::size_t m_active = 0;
    WorldStats m_stats;
};

} // namespace vo::client::world
