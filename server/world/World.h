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
//     callbacks go straight to the sessions (never into the world).
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
    [[nodiscard]] bool createZone(std::uint16_t id, assets::TerrainData terrain, std::string& error);
    [[nodiscard]] bool loadZone(std::uint16_t id, const std::filesystem::path& file, std::string& error);
    [[nodiscard]] Zone* zone(std::uint16_t id) noexcept;
    [[nodiscard]] Zone* startZone() noexcept { return m_startZone; }
    // Race spawn points of the start zone, for the account service's new characters.
    [[nodiscard]] auth::NewCharacterSpawns newCharacterSpawns() const;

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
    [[nodiscard]] std::size_t playerCount() const noexcept;
    [[nodiscard]] std::uint64_t tickIndex() const noexcept { return m_tickIndex; }

private:
    bool addZone(std::unique_ptr<Zone> zone, std::string& error);
    void threadMain();
    void runCommands(core::MonoTime now);
    void onEnter(EnterCommand& cmd, core::MonoTime now);
    void onMove(MoveCommand& cmd);
    void onEquip(EquipCommand& cmd);
    void onLeave(LeaveCommand& cmd, core::MonoTime now);
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
        core::MonoTime at;
    };
    std::unordered_map<auth::CharacterId, Recent> m_recent; // last positions of characters that left
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
