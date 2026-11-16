// Zone: one map of the world simulation (docs/SERVER_WORLD.md): its terrain (.vterrain: heights,
// walk mask, spawn points), the player and monster entities in it, the 32 m area-of-interest grid
// with the 5x5-cell visibility rule, authoritative movement (MoveValidator), ENTITY_SPAWN/DESPAWN
// pairs on visibility changes, ENTITY_MOVE relay (at most every relayInterval per entity, the latest
// state always delivered), POSITION_CORRECT, the periodic saves, and combat (docs/PROTOCOL_COMBAT.md,
// docs/SERVER_WORLD.md §8b: monsters and their AI, auto-attack, skills, death and respawn, regen,
// experience, stance; implemented in ZoneCombat.cpp), and loot (docs/PROTOCOL_LOOT.md,
// docs/SERVER_WORLD.md §8c: containers dropped by dying monsters, owner window, viewers, take
// reservations handed to the sessions; implemented in ZoneLoot.cpp).
//
// Every player keeps the set of entities its client currently has spawned ("known"); every monster
// the set of players that have it spawned ("observers"). Invariants (checked by tests/server/world):
// B in known(A) <=> A in known(B) <=> cells of A and B within 2 for players; M in known(P) <=> P in
// observers(M) <=> M is in the grid and its cell is within 2 of P's. A spawn is only sent for an
// entity not known yet and a despawn only for a known one, so pairs are never duplicated.
//
// Threading: not thread-safe; owned by World and used only on the world thread.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/world/AoiGrid.h"
#include "server/world/MoveValidator.h"
#include "server/world/WorldTypes.h"
#include "shared/assets/Terrain.h"
#include "shared/core/Clock.h"
#include "shared/game/Combat.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/game/PathFind.h"
#include "shared/net/PacketWriter.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vo::server::world {

inline constexpr std::string_view kSpawnAydor = "spawn_aydor";
inline constexpr std::string_view kSpawnKargan = "spawn_kargan";

struct ZoneSettings
{
    MoveRules rules;
    core::MonoClock::duration relayInterval = std::chrono::milliseconds(100); // 10 Hz per entity
    std::uint32_t savePeriodTicks = 1200; // periodic position save every N ticks (60 s at 20 Hz); 0 = off
    float spawnSpread = 2.0f;             // players placed at a spawn point land within this radius
    std::uint32_t seed = 0;               // spawn spread / combat / AI RNG; 0 = random
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
    std::uint64_t equips = 0;        // ENTITY_EQUIP packets
    std::uint64_t combatEvents = 0;  // COMBAT_EVENTs resolved (not packets)
    std::uint64_t monsterKills = 0;
    std::uint64_t playerDeaths = 0;
    std::uint64_t monsterRespawns = 0;
    std::uint64_t pathSearches = 0; // grid searches by monsters (straight lines are not counted)
    std::uint64_t lootDrops = 0;    // containers created
    std::uint64_t lootTakes = 0;    // reservations handed to sessions
    std::uint64_t lootReturns = 0;  // reservations that gave something back (did not fit, failed, cancelled)
    std::uint64_t lootDespawns = 0; // containers removed (emptied or expired)
};

class Zone
{
public:
    // Combat state of a player (PROTOCOL_COMBAT.md §1).
    struct PlayerCombat
    {
        game::EquipBonus equip;
        game::Stats stats;
        std::uint32_t hp = 0;
        std::uint32_t mp = 0;
        std::uint32_t xp = 0;
        bool dead = false;
        core::MonoTime diedAt{};
        bool sitting = false;
        EntityId selected = 0; // TARGET_REQ (informational)

        EntityId attackTarget = 0; // auto-attack
        bool rangeFailSent = false;
        bool safeFailSent = false;
        core::MonoTime nextSwingAt{};
        core::MonoTime gcdUntil{};
        std::array<core::MonoTime, game::kMaxSkillId + 1> skillReadyAt{};

        std::uint16_t castSkill = 0; // 0 = not casting
        EntityId castTarget = 0;
        core::MonoTime castEndsAt{};

        bool everFought = false;
        core::MonoTime lastCombatAt{}; // damage dealt or taken
        core::MonoTime nextRegenAt{};
        float hpRegen = 0.0f, mpRegen = 0.0f; // fractional regeneration carried over

        std::uint8_t sentState = 0; // last ENTITY_STATE bits the observers were told
        bool selfStatsDirty = false;
        core::MonoTime lastSelfStats{};
        bool entityStatsDirty = false;
        core::MonoTime lastEntityStats{};
        bool saveNow = false; // level-up: save in this tick
    };

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
        std::uint64_t mark = 0;             // broadcast de-duplication (sendAround)

        PlayerCombat combat;
        EntityId viewingLoot = 0; // the loot container this player has open (PROTOCOL_LOOT.md §3.2)
    };

    // A loot container (PROTOCOL_LOOT.md §1.3).
    struct Loot
    {
        struct Entry
        {
            std::uint16_t templateId = 0;
            std::uint16_t quantity = 0; // available now (reserved units are not counted); 0 = empty
        };
        EntityId id = 0;
        std::uint16_t monsterTemplate = 0;
        std::string name; // the monster's display name
        std::uint16_t level = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        AoiCell cell;
        auth::CharacterId owner = 0;
        core::MonoTime createdAt{}, ownerUntil{}, expiresAt{};
        bool ownerWindowOver = false;
        std::uint32_t coins = 0; // available now
        std::array<Entry, game::kMaxLootEntries> entries{};
        std::uint8_t entryCount = 0; // indices 0..entryCount-1 were filled at creation
        std::uint32_t reservations = 0; // open reservations (the container is not despawned before 0)
        std::unordered_set<EntityId> observers; // players that have it spawned
        std::unordered_set<EntityId> viewers;   // players that have it open
        bool contentsDirty = false;            // LOOT_CONTENTS to the viewers in this tick

        [[nodiscard]] bool empty() const noexcept;
    };

    // Units reserved for one take (PROTOCOL_LOOT.md §3.4).
    struct LootReservation
    {
        std::uint64_t id = 0;
        SessionKey session = 0;
        EntityId loot = 0;
        std::uint32_t coins = 0;
        std::vector<LootGrant::Item> items;
    };

    struct Monster
    {
        enum class State : std::uint8_t
        {
            Idle,   // wandering inside the spawn circle
            Chase,  // fighting: chasing / attacking the highest-threat player
            Return, // walking home at full hp, ignoring damage
            Dead,   // corpse (still spawned, state bit dead)
            Gone,   // despawned, waiting for the respawn timer
        };
        struct Threat
        {
            EntityId player = 0;
            float threat = 0.0f;
            std::uint32_t damage = 0; // experience goes to the player who dealt the most
        };

        EntityId id = 0;
        const game::MonsterTemplate* tmpl = nullptr;
        float homeX = 0.0f, homeZ = 0.0f, radius = 0.0f; // spawn point
        State state = State::Gone;
        float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
        std::uint8_t flags = 0; // ENTITY_MOVE flags
        std::uint32_t moveTimeMs = 0;
        AoiCell cell;
        bool inGrid = false;
        std::unordered_set<EntityId> observers;
        std::uint32_t hp = 0;

        std::vector<game::PathPoint> path; // waypoints still to walk
        std::size_t pathIndex = 0;
        core::MonoTime nextWanderAt{}, nextAttackAt{}, diedAt{}, respawnAt{}, nextRepathAt{}, nextSearchAt{},
            nextAggroScanAt{};
        core::MonoTime lastProgressAt{};
        EntityId target = 0;
        std::vector<Threat> threats;

        bool relayDirty = false;
        bool everRelayed = false;
        core::MonoTime lastRelay{};
        float relayedX = 0.0f, relayedY = 0.0f, relayedZ = 0.0f, relayedYaw = 0.0f;
        std::uint8_t relayedFlags = 0;
        std::uint8_t sentState = 0;
        bool statsDirty = false;
        core::MonoTime lastStats{};
    };

    struct EnterParams
    {
        EntityId entityId = 0;
        SessionKey session = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<IPlayerLink> link;
        auth::AccountId account = 0;
        auth::CharacterInfo character;
        game::EquipBonus equip;
    };

    struct LeftPlayer
    {
        auth::AccountId account = 0;
        auth::CharacterId character = 0;
        auth::CharacterPosition position;
        auth::CharacterProgress progress; // hp 0 = left dead
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

    // Before the first player enters: the safe zones and monster spawns of this zone from `data`
    // (entries of other zones are ignored). Every spawn centre must be walkable, outside the zone's
    // safe zones; `allocate` hands out unique entity ids. The monsters spawn at `now`. False + error
    // otherwise (nothing is installed then).
    [[nodiscard]] bool installMonsters(std::shared_ptr<const game::MonsterData> data,
                                       const std::function<EntityId()>& allocate, core::MonoTime now,
                                       std::string& error);
    [[nodiscard]] bool inSafeZone(float x, float z) const noexcept;
    // Loot tables (gamedata/loot.toml); null = monsters drop nothing. `allocate` hands out the
    // containers' entity ids. Before the first player enters.
    void installLoot(std::shared_ptr<const game::LootTables> tables, std::function<EntityId()> allocate);

    // ---- Commands ----

    // The scheduled time of the tick about to run (combat timing, COMBAT_EVENT server_time_ms).
    // update() sets it too; World calls it before running the tick's commands.
    void beginTick(core::MonoTime logicalNow, std::uint32_t logicalMs);

    // Places the character (saved position if inside the zone and walkable, y snapped; otherwise
    // its race spawn; spread slightly when at a spawn point; a character that left dead respawns at
    // its race spawn with 50 % hp/mp), queues ENTER_WORLD info + ENTITY_SPAWN of everyone in range
    // for the newcomer and ENTITY_SPAWN of the newcomer for them.
    void enter(EnterParams params, core::MonoTime now, std::uint32_t nowMs);
    // Despawns the player to its observers and removes it (its pending packets are dropped).
    std::optional<LeftPlayer> leave(SessionKey session);
    // Validates and applies one MOVE_REQ; nullopt if the session has no entity here. A dead player is
    // corrected (MoveVerdict::Dead); an accepted move interrupts a cast, one with the moving bit also
    // stands the player up.
    std::optional<MoveVerdict> move(SessionKey session, const net::MoveReq& req, core::MonoTime at,
                                    std::uint32_t atMs);
    // New equipment: ENTITY_EQUIP to the player itself and to every player that has it in view (its
    // `known` set) if the visible items changed; new stats + SELF_STATS if the bonuses changed.
    void setEquipment(SessionKey session, std::uint16_t mainHandTemplate, std::uint16_t offHandTemplate,
                      game::EquipBonus equip = {});
    // TARGET_REQ / ATTACK_REQ / ATTACK_STOP_REQ / SKILL_REQ / RESPAWN_REQ / STANCE_REQ.
    void combat(const CombatCommand& cmd);
    // LOOT_OPEN_REQ / LOOT_TAKE_REQ / LOOT_CLOSE_REQ (PROTOCOL_LOOT.md §3; ZoneLoot.cpp).
    void loot(const LootCommand& cmd);
    // Ends a reservation of this zone (units not taken go back). False if it is not one of this zone.
    bool settleLoot(const LootSettleCommand& cmd);
    // The session left the world: every reservation of it not in `keep` (writes in flight) is
    // returned to its container.
    void cancelLootTakes(SessionKey session, const std::vector<std::uint64_t>& keep);
    // Testing: a container with these contents at (x, z), owned by `owner` (0 = nobody), as if a
    // monster of `monsterTemplate` had died there now. Returns its id.
    EntityId dropLootForTesting(float x, float z, auth::CharacterId owner, const game::LootRoll& roll,
                                std::uint16_t monsterTemplate = 1);

    // ---- Per tick ----

    // POSITION_CORRECT for players with rejected moves, combat (casts, swings, regen, monster AI,
    // stats), ENTITY_MOVE relay, periodic saves. `logicalNow` is the scheduled time of the tick
    // (exactly one tick interval apart).
    void update(core::MonoTime logicalNow, std::uint64_t tickIndex, const SavePositionFn& save);
    // Hands every non-empty batch to its player's link.
    void flush();

    // ---- Queries ----

    [[nodiscard]] std::size_t playerCount() const noexcept { return m_players.size(); }
    [[nodiscard]] std::size_t monsterCount() const noexcept { return m_monsters.size(); }
    [[nodiscard]] const Player* findPlayer(SessionKey session) const;
    [[nodiscard]] const Player* findEntity(EntityId id) const; // players only
    [[nodiscard]] const Monster* findMonster(EntityId id) const;
    [[nodiscard]] const Loot* findLoot(EntityId id) const;
    [[nodiscard]] std::size_t lootCount() const noexcept { return m_loot.size(); }
    [[nodiscard]] std::size_t lootReservationCount() const noexcept { return m_reservations.size(); }
    template <class F>
    void forEachLoot(F&& f) const
    {
        for (const auto& [id, l] : m_loot)
            f(*l);
    }
    [[nodiscard]] bool hasEntity(EntityId id) const noexcept; // player, monster or loot container
    [[nodiscard]] std::optional<auth::CharacterPosition> positionOf(SessionKey session) const;
    template <class F>
    void forEachPlayer(F&& f) const
    {
        for (const auto& [id, p] : m_players)
            f(*p);
    }
    template <class F>
    void forEachMonster(F&& f) const
    {
        for (const auto& [id, m] : m_monsters)
            f(*m);
    }
    // Progress as it would be saved now.
    [[nodiscard]] static auth::CharacterProgress progressOf(const Player& p) noexcept;
    // ENTITY_STATE bits of an entity now.
    [[nodiscard]] std::uint8_t stateOf(const Player& p) const noexcept;
    [[nodiscard]] static std::uint8_t stateOf(const Monster& m) noexcept;

    // Moves an entity to (x, z) without validation (y snapped), with full AOI bookkeeping. Used by
    // the move path after validation and by tests.
    void placeEntity(SessionKey session, float x, float z);
    // Testing: puts a monster at (x, z) (y snapped) with AOI bookkeeping; sets its hp if given.
    void placeMonsterForTesting(EntityId id, float x, float z, std::optional<std::uint32_t> hp = std::nullopt);
    // Testing: sets a player's hp/mp (clamped to the maxima), marking SELF_STATS dirty.
    void setVitalsForTesting(SessionKey session, std::uint32_t hp, std::uint32_t mp);

private:
    Player* find(SessionKey session);
    Player* findById(EntityId id);
    Monster* monsterById(EntityId id);
    void applyCellChange(Player& p, AoiCell from, AoiCell to);
    void spawnPair(Player& a, Player& b);
    void despawnPair(Player& a, Player& b);
    void sendSpawn(Player& to, const Player& about);
    void sendDespawn(Player& to, EntityId about);
    void relay(Player& p, core::MonoTime logicalNow);
    [[nodiscard]] bool placeable(float x, float z) const;
    void spreadAround(float& x, float& z);
    void movePlayerTo(Player& p, float x, float z);

    // ---- Monsters and combat (ZoneCombat.cpp) ----
    void initCombat(Player& p, const auth::CharacterInfo& c, game::EquipBonus equip, bool& respawnAtSpawn);
    void monsterSeen(Player& p, Monster& m);   // spawn M to P if not known yet
    void monsterUnseen(Player& p, Monster& m); // despawn M from P if known
    void sendMonsterSpawn(Player& to, const Monster& m);
    void spawnMonster(Monster& m, bool atHome);
    void despawnMonster(Monster& m);
    void setMonsterPosition(Monster& m, float x, float z);
    void relayMonster(Monster& m, core::MonoTime logicalNow);
    void updateMonster(Monster& m);
    void monsterIdle(Monster& m);
    void monsterChase(Monster& m);
    void monsterReturn(Monster& m);
    void startChase(Monster& m, Player& p);
    void startReturn(Monster& m);
    [[nodiscard]] bool validMonsterTarget(const Monster& m, const Player& p) const;
    bool walkMonster(Monster& m, float speed, bool running);
    bool pathMonsterTo(Monster& m, float x, float z, bool allowSearch);
    void stopMonster(Monster& m);
    void monsterAttack(Monster& m, Player& p);
    void killMonster(Monster& m, EntityId killer);
    [[nodiscard]] Monster::Threat* threatOf(Monster& m, EntityId player);

    void updatePlayerCombat(Player& p);
    void trySwing(Player& p);
    void completeCast(Player& p);
    void interruptCast(Player& p, bool notify);
    // Validates the target of `skill` for `p` now; 0 on success, else the ACTION_FAIL reason.
    [[nodiscard]] std::uint8_t checkSkillTarget(Player& p, const game::SkillDef& skill, EntityId target,
                                                EntityId& resolved);
    void executeSkill(Player& p, const game::SkillDef& skill, EntityId target);
    void damageMonster(Player& p, Monster& m, std::uint16_t skillId, game::Hit hit);
    void damagePlayer(Monster& m, Player& p, game::Hit hit);
    void healPlayer(Player& healer, Player& target, const game::SkillDef& skill);
    void killPlayer(Player& p, EntityId killer);
    void respawnPlayer(Player& p);
    void gainExperience(Player& p, std::uint32_t amount, EntityId source);
    void standUp(Player& p);
    void markInCombat(Player& p);
    void recomputeStats(Player& p);
    void flushCombatState();
    void actionFail(Player& p, net::CombatAction action, std::uint16_t skill, net::ActionFailReason reason);
    [[nodiscard]] bool inCombat(const Player& p) const noexcept;
    [[nodiscard]] bool knows(const Player& p, EntityId id) const noexcept { return p.known.contains(id); }
    // Encodes `msg` once and appends it to every player that has `a` or `b` spawned, and to `a` /
    // `b` themselves if they are players (PROTOCOL_COMBAT.md §3.3). 0 = no entity.
    template <class T>
    void sendAround(const T& msg, EntityId a, EntityId b = 0);

    // ---- Loot (ZoneLoot.cpp) ----
    Loot* lootById(EntityId id);
    void dropLoot(const Monster& m, auth::CharacterId owner);
    EntityId createLoot(float x, float z, auth::CharacterId owner, std::uint16_t monsterTemplate, std::string name,
                        std::uint16_t level, const game::LootRoll& roll);
    void lootSeen(Player& p, Loot& l);   // spawn L to P if not known yet
    void lootUnseen(Player& p, Loot& l); // despawn L from P if known (and close its view)
    void sendLootSpawn(Player& to, const Loot& l);
    [[nodiscard]] std::uint8_t lootStateFor(const Loot& l, const Player& p) const noexcept;
    void sendLootContents(Player& to, const Loot& l);
    void closeLootView(Player& p, net::LootClosedReason reason, bool notify);
    void returnReservation(const LootReservation& r, std::uint32_t coins, const std::vector<LootGrant::Item>& items);
    void updateLoot(); // per tick: owner windows, viewers, contents, despawns
    void removeLoot(Loot& l);
    [[nodiscard]] net::LootResult checkLootAccess(const Player& p, const Loot* l) const;

    std::uint16_t m_id;
    assets::TerrainData m_terrain;
    ZoneSettings m_settings;
    AoiGrid m_grid;
    auth::CharacterPosition m_spawnAydor;
    auth::CharacterPosition m_spawnKargan;
    std::unordered_map<EntityId, std::unique_ptr<Player>> m_players;
    std::unordered_map<SessionKey, EntityId> m_bySession;
    std::unordered_map<EntityId, std::unique_ptr<Monster>> m_monsters;
    std::shared_ptr<const game::MonsterData> m_monsterData;
    std::vector<game::SafeZone> m_safeZones;
    std::shared_ptr<const game::LootTables> m_lootTables;
    std::function<EntityId()> m_allocate; // entity ids for loot containers
    std::unordered_map<EntityId, std::unique_ptr<Loot>> m_loot;
    std::unordered_map<std::uint64_t, LootReservation> m_reservations;
    std::uint64_t m_nextReservation = 1;
    std::mt19937 m_rng;
    net::PacketWriter m_scratch;
    ZoneCounters m_counters;
    core::MonoTime m_now{};      // scheduled time of the current tick
    core::MonoTime m_lastTick{}; // previous update (monster movement step)
    std::uint32_t m_nowMs = 0;
    float m_dt = 0.05f; // seconds since the previous update (clamped)
    std::uint64_t m_mark = 0;
};

} // namespace vo::server::world
