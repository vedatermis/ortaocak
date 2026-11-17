// World: the authoritative world simulation (docs/SERVER_WORLD.md). Owns the zones and runs them in
// a fixed-timestep loop (world.tick_rate, 20 Hz) on ONE dedicated thread.
//
// THREADING CONTRACT
//   - All simulation state (zones, entities, AOI, sessions-in-world maps) is owned by the world
//     thread and touched only there. Nothing in it is locked.
//   - Other threads talk to the world only through post(), which appends a WorldCommand to a
//     mutex-protected queue. The queue is swapped out once per tick and the commands run in posting
//     order at the start of the tick. Network handlers therefore never touch world state, and
//     neither do DB completions: the account service calls back on its worker threads, and those
//     callbacks go straight to the sessions. The one exception is the end of a loot take, which
//     the DB completion *posts* as a LootSettleCommand (docs/SERVER_WORLD.md §8c).
//   - Results flow out through IPlayerLink::deliver(), called on the world thread once per tick per
//     player with that player's whole batch; the network implementation posts it to the session's
//     io_context. Position saves are submitted from the world thread to the (thread-safe) account
//     service; their completions run on DB worker threads and only log or forward to sessions.
//   - start()/stop() from the owning thread (main). Before start() and after stop() the owner may
//     drive the world synchronously with tick() (tests do).
//
// Tick pipeline: commands (enter / move / leave) -> per zone: POSITION_CORRECT, ENTITY_MOVE relay,
// periodic saves -> flush batches to links. Absolute scheduling (next += interval) gives drift-free
// 20 Hz; a late tick runs immediately (catch-up), more than world.max_catch_up_ticks behind skips
// ahead with a warning. Tick durations go into a histogram (TickStats); slow ticks (> 1 interval)
// are logged (rate-limited) and a summary is logged every world.stats_interval_s.
#pragma once

#include "server/world/TickStats.h"
#include "server/world/WorldTypes.h"
#include "server/world/Zone.h"
#include "shared/core/Clock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vo::server::world {

struct WorldSettings
{
    int tickRate = 20;                     // world.tick_rate
    ZoneSettings zone;                     // movement rules, relay interval, spawn spread
    std::chrono::seconds saveInterval{60}; // world.save_interval_s (0 = only on leave)
    std::chrono::seconds statsInterval{60}; // world.stats_interval_s (0 = off)
    std::chrono::seconds recentLeaveMemory{60};
    int maxCatchUpTicks = 5; // world.max_catch_up_ticks
};

class World
{
public:
    // `save` persists positions (normally IAccountService::saveCharacterPosition); may be empty.
    World(WorldSettings settings, SavePositionFn save, core::MonoTime epoch = core::monoNow());
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Before start(): adds a zone built from terrain data / a .vterrain file with the world's zone
    // settings. The first zone is the start zone (new characters, unknown zone ids). False + error
    // if the terrain is unusable (missing/corrupt file, no or unwalkable race spawns).
    [[nodiscard]] bool createZone(std::uint16_t id, assets::TerrainData terrain, std::string& error,
                                  ZoneLayout layout = {});
    [[nodiscard]] bool loadZone(std::uint16_t id, const std::filesystem::path& file, std::string& error,
                                ZoneLayout layout = {});
    // Before start(), after every zone: checks every gate against its target (the target zone is
    // loaded, the arrival marker exists, is walkable and lies more than radius + 1 m outside every
    // gate of the target zone; PROTOCOL_ZONES.md §1.2) and sets the home zone of each race (0 = the
    // start zone; the zone must allow the race). False + error otherwise.
    [[nodiscard]] bool finalizeZones(std::uint16_t homeAydor, std::uint16_t homeKargan, std::string& error);
    // The home zone of a race (new characters, invalid saved positions; PROTOCOL_ZONES.md §4).
    [[nodiscard]] Zone* homeZone(std::uint8_t race) noexcept;
    [[nodiscard]] std::size_t zoneCount() const noexcept { return m_zones.size(); }
    [[nodiscard]] Zone* zone(std::uint16_t id) noexcept;
    [[nodiscard]] Zone* startZone() noexcept { return m_startZone; }
    // Race spawn points of each race's home zone, for the account service's new characters.
    [[nodiscard]] auth::NewCharacterSpawns newCharacterSpawns() const;
    // Before start(), after the zones: monsters, spawn points and safe zones (gamedata/monsters.toml,
    // docs/PROTOCOL_COMBAT.md §1.3) for every loaded zone. False + error if a spawn point is not
    // usable on its zone's terrain.
    [[nodiscard]] bool installMonsters(std::shared_ptr<const game::MonsterData> data, std::string& error);
    [[nodiscard]] std::size_t monsterCount() const noexcept;
    // Before start(): the loot tables (gamedata/loot.toml, docs/PROTOCOL_LOOT.md §2) for every zone.
    void installLoot(std::shared_ptr<const game::LootTables> tables);
    // Before start(), after the zones: the NPCs (gamedata/npcs.toml, docs/PROTOCOL_SHOP.md §1) of every
    // loaded zone, prices from `items`. False + error if a spot is not usable (§1.2).
    [[nodiscard]] bool installNpcs(std::shared_ptr<const game::NpcCatalog> catalog, const game::ItemCatalog& items,
                                   std::string& error);
    [[nodiscard]] std::size_t npcCount() const noexcept;

    [[nodiscard]] const WorldSettings& settings() const noexcept { return m_settings; }
    [[nodiscard]] core::MonoTime epoch() const noexcept { return m_epoch; }
    [[nodiscard]] core::MonoClock::duration tickInterval() const noexcept { return m_interval; }
    // Milliseconds since `epoch` (the server_time_ms of the protocol; wraps after ~49 days).
    [[nodiscard]] std::uint32_t serverTimeMs(core::MonoTime t) const noexcept;

    // ---- Any thread ----

    // Queues a command for the next tick. After stop() commands are not run; a LeaveCommand's
    // `done` is then invoked immediately with false.
    void post(WorldCommand command);
    // Starts the simulation thread.
    void start();
    // Stops the thread (bounded: at most one tick), runs the commands still queued, removes every
    // remaining player with a final position save, and refuses later commands. Idempotent.
    void stop();
    [[nodiscard]] bool running() const noexcept { return m_thread.joinable(); }
    [[nodiscard]] TickStats::Summary totalStats() const;
    // Ticks skipped by the overload rule since start() (thread-safe).
    [[nodiscard]] std::uint64_t skippedTicks() const;
    // Ticks run by the simulation thread so far (thread-safe).
    [[nodiscard]] std::uint64_t ticksRun() const noexcept { return m_ticksRun.load(std::memory_order_relaxed); }

    // Testing: called on the world thread at the start of every tick with the tick index (e.g. to
    // inject a slow tick). Set before start().
    void setTickHookForTesting(std::function<void(std::uint64_t tickIndex)> hook) { m_tickHook = std::move(hook); }

    // ---- World thread (or the owner while not running) ----

    // One tick. `now` is the real time, `logical` the scheduled tick time (relay rate limiting).
    void tick(core::MonoTime now, core::MonoTime logical);
    void tick(core::MonoTime now) { tick(now, now); }
    [[nodiscard]] std::size_t playerCount() const noexcept; // in zones and between zones
    // The zone a session's player is in (0 = none, or between two zones).
    [[nodiscard]] std::uint16_t zoneIdOf(SessionKey session) const noexcept;
    [[nodiscard]] bool inTransit(SessionKey session) const noexcept { return m_transit.contains(session); }
    [[nodiscard]] std::uint64_t zoneChanges() const noexcept { return m_zoneChanges; }
    [[nodiscard]] std::uint64_t tickIndex() const noexcept { return m_tickIndex; }

private:
    bool addZone(std::unique_ptr<Zone> zone, std::string& error);
    void threadMain();
    void runCommands(core::MonoTime now);
    void onEnter(EnterCommand& cmd, core::MonoTime now);
    void onMove(MoveCommand& cmd);
    void onEquip(EquipCommand& cmd);
    void onCombat(CombatCommand& cmd);
    void onLoot(LootCommand& cmd);
    void onLootSettle(LootSettleCommand& cmd);
    void onStats(StatsCommand& cmd);
    void onSkills(SkillsCommand& cmd);
    void onLeave(LeaveCommand& cmd, core::MonoTime now);
    void onZoneArrived(ZoneArrivedCommand& cmd, core::MonoTime now);
    void onZoneReady(ZoneReadyCommand& cmd);
    void onShop(ShopCommand& cmd);
    void onItemUse(ItemUseCommand& cmd);
    void runGateTriggers(core::MonoTime now);
    void beginZoneChange(Zone& from, const Zone::GateTrigger& trigger, core::MonoTime now);
    Zone* zoneOfSession(SessionKey session) noexcept;
    EntityId allocateEntityId();
    void removeAllPlayers(core::MonoTime now);
    void logStats(core::MonoTime now, bool final);

    WorldSettings m_settings;
    SavePositionFn m_save;
    core::MonoTime m_epoch;
    core::MonoClock::duration m_interval;

    // Command queue (any thread).
    std::mutex m_queueMutex;
    std::vector<WorldCommand> m_queue;
    bool m_closed = false; // stop() finished: commands are refused

    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<std::uint64_t> m_ticksRun{0};
    std::function<void(std::uint64_t)> m_tickHook;

    // World-thread state.
    std::map<std::uint16_t, std::unique_ptr<Zone>> m_zones;
    Zone* m_startZone = nullptr;
    std::vector<WorldCommand> m_running; // commands of the current tick
    std::unordered_map<SessionKey, std::uint16_t> m_sessionZone;
    std::unordered_map<auth::CharacterId, SessionKey> m_characterSession;
    struct Recent
    {
        auth::CharacterPosition position;
        auth::CharacterProgress progress;
        core::MonoTime at;
        std::vector<Zone::RememberedCooldown> cooldowns; // PROTOCOL_SKILLS.md §1.7
    };
    std::unordered_map<auth::CharacterId, Recent> m_recent; // last state of characters that left
    // Players between two zones (PROTOCOL_ZONES.md §3.3): out of the old zone, the arrival being stored.
    struct Transit
    {
        std::uint64_t token = 0;
        Zone::EnterParams params; // character.position = the arrival
        std::uint16_t fromZone = 0;
        std::uint16_t toZone = 0;
        auth::CharacterPosition origin; // where it stood in the old zone
        auth::CharacterProgress progress;
        bool leaving = false; // a LeaveCommand arrived meanwhile
        std::function<void(bool)> leaveDone;
    };
    std::unordered_map<SessionKey, Transit> m_transit;
    std::uint64_t m_nextTransitToken = 1;
    std::uint16_t m_homeZone[3] = {0, 0, 0}; // by race (index 1 Aydor, 2 Kargan); 0 = start zone
    std::uint64_t m_zoneChanges = 0;
    std::uint64_t m_zoneChangeFailures = 0;
    EntityId m_nextEntityId = 1;
    std::uint64_t m_tickIndex = 0;

    // Statistics (written by the world thread, read by others under the mutex).
    mutable std::mutex m_statsMutex;
    TickStats m_total;
    TickStats m_window;
    std::uint64_t m_skippedTicks = 0;
    std::uint64_t m_windowSkipped = 0;
    core::MonoTime m_windowStart{};
    core::MonoTime m_lastSlowWarn{};
    std::uint64_t m_slowSinceWarn = 0;
    ZoneCounters m_windowBase;
};

} // namespace vo::server::world
