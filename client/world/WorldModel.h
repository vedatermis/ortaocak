// The client's view of the world (no D3D, no sockets): the predicted local player and the remote
// entities, fed by the world messages of docs/PROTOCOL.md §5.4 exactly as they arrive from the
// network (ENTITY_SPAWN / ENTITY_MOVE / ENTITY_DESPAWN / POSITION_CORRECT, and ENTITY_EQUIP for the
// visible weapons, docs/PROTOCOL_ITEMS.md). The scene passes every
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

#include <array>
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
    std::uint8_t face = 0; // look index (client/world/CharacterAssets.h)
    std::uint8_t hair = 0; // unused (always 0 from current clients)
    std::uint16_t level = 0;
    // Visible equipment (ENTITY_SPAWN, then ENTITY_EQUIP; docs/PROTOCOL_ITEMS.md §1.4). 0 = empty hand.
    std::uint16_t mainHand = 0;
    std::uint16_t offHand = 0;
    std::array<std::uint16_t, 5> armor{}; // M17: worn armour Head .. Feet (ENTITY_SPAWN / ENTITY_EQUIP), 0 = none
    std::uint32_t equipChanges = 0; // ENTITY_EQUIP messages applied since the spawn (automation)
    // Combat (v3, docs/PROTOCOL_COMBAT.md): ENTITY_SPAWN, then COMBAT_EVENT / ENTITY_STATS /
    // ENTITY_STATE / ENTITY_DEATH / LEVEL_UP (applied by the scene).
    std::uint32_t hp = 0;
    std::uint32_t maxHp = 0;
    std::uint16_t templateId = 0; // monster template (entity_type 2), 0 for players
    std::uint8_t state = 0;       // kEntityState* bits
    [[nodiscard]] bool isMonster() const noexcept { return type == vo::net::kEntityTypeMonster; }
    [[nodiscard]] bool isPlayer() const noexcept { return type == vo::net::kEntityTypePlayer; }
    // M10 (docs/PROTOCOL_LOOT.md §1.4): a loot container. `name` / `level` / `templateId` are the
    // monster it came from, `state` carries the LootMine / LootLocked bits; it never moves.
    [[nodiscard]] bool isLoot() const noexcept { return type == vo::net::kEntityTypeLoot; }
    [[nodiscard]] bool dead() const noexcept { return (state & vo::net::kEntityStateDead) != 0; }
    [[nodiscard]] bool sitting() const noexcept { return (state & vo::net::kEntityStateSitting) != 0; }
    float scale = 1.0f;          // drawn size (monster template), set by the scene
    float hpShownUntil = 0.0f;   // age until which a damaged player's hp bar is shown
    std::uint32_t swings = 0;    // attack clips played (punches alternate)
    std::uint32_t facing = 0;    // entity it is fighting: drawn facing it for a moment
    float facingUntil = 0.0f;    // age until which `facing` applies
    bool casting = false;        // CAST_START seen, no CAST_END yet
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
    // Owned by the scene's renderer side (bound once the race + look assets are loaded).
    Animator animator;
    const void* animatorMesh = nullptr; // mesh / clips the animator is bound to (null = unbound)
    const CharacterClips* animatorClips = nullptr;
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
    std::uint32_t equips = 0;
    std::uint32_t ignoredSpawns = 0; // unknown entity types (not player, monster or loot)
    std::uint32_t monsterSpawns = 0;
    std::uint32_t lootSpawns = 0;
    std::uint32_t lootDespawns = 0;
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
    // ENTITY_EQUIP: a remote's weapons changed, or (own entity id) the server's view of ours.
    void handleEquip(const vo::net::EntityEquip& m);

    // Local player step; returns the MOVE_REQ to send this frame, if any. Never returns one while
    // leaving (PROTOCOL §5.4 "Leaving"): the local player then stands still and ignores input.
    [[nodiscard]] std::optional<vo::net::MoveReq> updateLocal(double localNow, const MoveIntent& intent, float dt);

    // LEAVE_WORLD_REQ or LOGOUT was sent: from now on no MOVE_REQ, no local movement (the click
    // target is dropped). Cleared by the next enter().
    void beginLeaving();
    [[nodiscard]] bool leaving() const noexcept { return m_leaving; }
    // Forgets every remote entity (LEAVE_WORLD_RES, disconnect; enter() does it as well): the
    // server sends no ENTITY_DESPAWN to a player who leaves.
    void dropRemotes();
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
    // Active remote players / monsters / loot containers (activeCount() = all three).
    [[nodiscard]] std::size_t playerCount() const noexcept { return countOf(vo::net::kEntityTypePlayer); }
    [[nodiscard]] std::size_t monsterCount() const noexcept { return countOf(vo::net::kEntityTypeMonster); }
    [[nodiscard]] std::size_t lootCount() const noexcept { return countOf(vo::net::kEntityTypeLoot); }
    [[nodiscard]] std::size_t countOf(std::uint8_t entityType) const noexcept;
    [[nodiscard]] RemoteEntity* find(std::uint32_t id) noexcept;
    [[nodiscard]] const RemoteEntity* find(std::uint32_t id) const noexcept
    {
        return const_cast<WorldModel*>(this)->find(id);
    }
    [[nodiscard]] const WorldStats& stats() const noexcept { return m_stats; }
    // Last ENTITY_EQUIP the server sent for our own entity (nullopt: none since enter()).
    [[nodiscard]] const std::optional<vo::net::EntityEquip>& localEquip() const noexcept { return m_localEquip; }

private:
    std::optional<vo::net::EntityEquip> m_localEquip;
    const assets::TerrainData* m_terrain = nullptr;
    MovementParams m_params;
    Prediction m_local;
    ServerClock m_clock;
    std::uint32_t m_localId = 0;
    std::uint16_t m_zoneId = 0;
    std::vector<RemoteEntity> m_entities; // capacity kMaxEntities, slots reused
    std::size_t m_active = 0;
    WorldStats m_stats;
    bool m_leaving = false;
};

} // namespace vo::client::world
