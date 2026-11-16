// Zone: one map of the world simulation (docs/SERVER_WORLD.md): its terrain (.vterrain: heights,
// walk mask, spawn points), the player entities in it, the 32 m area-of-interest grid with the
// 5x5-cell visibility rule, authoritative movement (MoveValidator), ENTITY_SPAWN/DESPAWN pairs on
// visibility changes, ENTITY_MOVE relay (at most every relayInterval per entity, the latest state
// always delivered), POSITION_CORRECT, and the periodic position saves.
//
// Every player keeps the set of entities its client currently has spawned ("known"). Invariants
// (checked by tests/server/world): B in known(A) <=> A in known(B) <=> cells of A and B within 2;
// a spawn is only sent for an entity not known yet and a despawn only for a known one, so pairs
// are never duplicated.
//
// Threading: not thread-safe; owned by World and used only on the world thread.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/world/AoiGrid.h"
#include "server/world/MoveValidator.h"
#include "server/world/WorldTypes.h"
#include "shared/assets/Terrain.h"
#include "shared/core/Clock.h"
#include "shared/net/PacketWriter.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace vo::server::world {

inline constexpr std::string_view kSpawnAydor = "spawn_aydor";
inline constexpr std::string_view kSpawnKargan = "spawn_kargan";

struct ZoneSettings
{
    MoveRules rules;
    core::MonoClock::duration relayInterval = std::chrono::milliseconds(100); // 10 Hz per entity
    std::uint32_t savePeriodTicks = 1200; // periodic position save every N ticks (60 s at 20 Hz); 0 = off
    float spawnSpread = 2.0f;             // players placed at a spawn point land within this radius
    std::uint32_t seed = 0;               // spawn spread RNG; 0 = random
};

struct ZoneCounters
{
    std::uint64_t movesAccepted = 0;
    std::uint64_t movesRejected = 0;
    std::uint64_t corrections = 0; // POSITION_CORRECT packets
    std::uint64_t relays = 0;      // ENTITY_MOVE packets (one per observer)
    std::uint64_t spawns = 0;      // ENTITY_SPAWN packets
    std::uint64_t despawns = 0;    // ENTITY_DESPAWN packets
    std::uint64_t periodicSaves = 0;
    std::uint64_t equips = 0; // ENTITY_EQUIP packets
};

class Zone
{
public:
    struct Player
    {
        EntityId id = 0;
        SessionKey session = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<IPlayerLink> link;
        auth::AccountId account = 0;
        auth::CharacterInfo character;

        MoveState move;                // authoritative position + validation state
        std::uint32_t moveTimeMs = 0;  // server time of the current state (ENTITY_MOVE server_time_ms)
        AoiCell cell;
        std::uint32_t lastSeq = 0;     // last processed MOVE_REQ seq (POSITION_CORRECT)
        bool correctionPending = false;

        bool relayDirty = false; // state changed since the last ENTITY_MOVE relay
        bool everRelayed = false;
        core::MonoTime lastRelay{};
        float relayedX = 0.0f, relayedY = 0.0f, relayedZ = 0.0f, relayedYaw = 0.0f;
        std::uint8_t relayedFlags = 0;

        bool saveDirty = true;
        std::uint32_t saveSlot = 0;

        std::unordered_set<EntityId> known; // entities this player's client has spawned
        WorldBatch out;                     // packets for this tick
    };

    struct EnterParams
    {
        EntityId entityId = 0;
        SessionKey session = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<IPlayerLink> link;
        auth::AccountId account = 0;
        auth::CharacterInfo character;
    };

    struct LeftPlayer
    {
        auth::AccountId account = 0;
        auth::CharacterId character = 0;
        auth::CharacterPosition position;
    };

    // Validates the terrain (both race spawns present and walkable). nullptr + error otherwise.
    [[nodiscard]] static std::unique_ptr<Zone> create(std::uint16_t id, assets::TerrainData terrain,
                                                      ZoneSettings settings, std::string& error);
    // Loads a .vterrain file and creates the zone.
    [[nodiscard]] static std::unique_ptr<Zone> load(std::uint16_t id, const std::filesystem::path& file,
                                                    ZoneSettings settings, std::string& error);

    struct PrivateTag
    {
        explicit PrivateTag() = default;
    };
    Zone(PrivateTag, std::uint16_t id, assets::TerrainData terrain, ZoneSettings settings); // use create()
    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;

    [[nodiscard]] std::uint16_t id() const noexcept { return m_id; }
    [[nodiscard]] const assets::TerrainData& terrain() const noexcept { return m_terrain; }
    [[nodiscard]] const AoiGrid& grid() const noexcept { return m_grid; }
    [[nodiscard]] const ZoneSettings& settings() const noexcept { return m_settings; }
    [[nodiscard]] const ZoneCounters& counters() const noexcept { return m_counters; }

    // Exact spawn point of a race (y on the terrain). Unknown races use the Aydor spawn.
    [[nodiscard]] auth::CharacterPosition raceSpawn(std::uint8_t race) const noexcept;

    // ---- Commands ----

    // Places the character (saved position if inside the zone and walkable, y snapped; otherwise
    // its race spawn; spread slightly when at a spawn point), queues ENTER_WORLD info + ENTITY_SPAWN
    // of everyone in range for the newcomer and ENTITY_SPAWN of the newcomer for them.
    void enter(EnterParams params, core::MonoTime now, std::uint32_t nowMs);
    // Despawns the player to its observers and removes it (its pending packets are dropped).
    std::optional<LeftPlayer> leave(SessionKey session);
    // Validates and applies one MOVE_REQ; nullopt if the session has no entity here.
    std::optional<MoveVerdict> move(SessionKey session, const net::MoveReq& req, core::MonoTime at,
                                    std::uint32_t atMs);
    // New visible equipment: ENTITY_EQUIP to the player itself and to every player that has it in
    // view (its `known` set); nothing if unchanged or the session has no entity here.
    void setEquipment(SessionKey session, std::uint16_t mainHandTemplate, std::uint16_t offHandTemplate);

    // ---- Per tick ----

    // POSITION_CORRECT for players with rejected moves, ENTITY_MOVE relay, periodic saves.
    // `logicalNow` is the scheduled time of the tick (exactly one tick interval apart).
    void update(core::MonoTime logicalNow, std::uint64_t tickIndex, const SavePositionFn& save);
    // Hands every non-empty batch to its player's link.
    void flush();

    // ---- Queries ----

    [[nodiscard]] std::size_t playerCount() const noexcept { return m_players.size(); }
    [[nodiscard]] const Player* findPlayer(SessionKey session) const;
    [[nodiscard]] const Player* findEntity(EntityId id) const;
    [[nodiscard]] std::optional<auth::CharacterPosition> positionOf(SessionKey session) const;
    template <class F>
    void forEachPlayer(F&& f) const
    {
        for (const auto& [id, p] : m_players)
            f(*p);
    }

    // Moves an entity to (x, z) without validation (y snapped), with full AOI bookkeeping. Used by
    // the move path after validation and by tests.
    void placeEntity(SessionKey session, float x, float z);

private:
    Player* find(SessionKey session);
    Player* findById(EntityId id);
    void applyCellChange(Player& p, AoiCell from, AoiCell to);
    void spawnPair(Player& a, Player& b);
    void despawnPair(Player& a, Player& b);
    void sendSpawn(Player& to, const Player& about);
    void sendDespawn(Player& to, EntityId about);
    void relay(Player& p, core::MonoTime logicalNow);
    [[nodiscard]] bool placeable(float x, float z) const;
    void spreadAround(float& x, float& z);

    std::uint16_t m_id;
    assets::TerrainData m_terrain;
    ZoneSettings m_settings;
    AoiGrid m_grid;
    auth::CharacterPosition m_spawnAydor;
    auth::CharacterPosition m_spawnKargan;
    std::unordered_map<EntityId, std::unique_ptr<Player>> m_players;
    std::unordered_map<SessionKey, EntityId> m_bySession;
    std::mt19937 m_rng;
    net::PacketWriter m_scratch;
    ZoneCounters m_counters;
};

} // namespace vo::server::world
