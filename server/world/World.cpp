#include "server/world/World.h"

#include "shared/core/Assert.h"
#include "shared/core/Log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

namespace vo::server::world {

namespace {

// Sleeps until an absolute monotonic time. On Windows a high-resolution waitable timer gives ~0.5 ms
// accuracy without raising the process-wide timer resolution (plain sleeps round up to 15.6 ms).
class TickTimer
{
public:
    TickTimer()
    {
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_timer)
            VO_LOG_WARN("world: no high-resolution timer (error {}), tick timing falls back to sleep",
                        GetLastError());
#endif
    }
    ~TickTimer()
    {
#if defined(_WIN32)
        if (m_timer)
            CloseHandle(m_timer);
#endif
    }
    TickTimer(const TickTimer&) = delete;
    TickTimer& operator=(const TickTimer&) = delete;

    void waitUntil(core::MonoTime t)
    {
        const auto now = core::monoNow();
        if (t <= now)
            return;
#if defined(_WIN32)
        if (m_timer)
        {
            const auto hundredNs = std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10'000'000>>>(
                                       t - now)
                                       .count();
            LARGE_INTEGER due;
            due.QuadPart = -std::max<long long>(hundredNs, 1); // negative = relative
            if (SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0))
            {
                WaitForSingleObject(m_timer, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_until(t);
    }

private:
#if defined(_WIN32)
    HANDLE m_timer = nullptr;
#endif
};

double toMs(core::MonoClock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

} // namespace

World::World(WorldSettings settings, SavePositionFn save, core::MonoTime epoch)
    : m_settings(std::move(settings)), m_save(std::move(save)), m_epoch(epoch),
      m_interval(std::chrono::duration_cast<core::MonoClock::duration>(std::chrono::nanoseconds(
          1'000'000'000 / std::clamp(m_settings.tickRate, 1, 1000)))),
      m_total(m_interval), m_window(m_interval)
{
    const auto period = static_cast<std::int64_t>(m_settings.saveInterval.count()) * std::clamp(m_settings.tickRate, 1, 1000);
    m_settings.zone.savePeriodTicks = static_cast<std::uint32_t>(std::max<std::int64_t>(period, 0));
}

World::~World()
{
    stop();
}

bool World::addZone(std::unique_ptr<Zone> zone, std::string& error)
{
    if (!zone)
        return false;
    if (running() || m_zones.contains(zone->id()))
    {
        error = "zone already exists or the world is running";
        return false;
    }
    Zone* raw = zone.get();
    m_zones[raw->id()] = std::move(zone);
    if (!m_startZone)
        m_startZone = raw;
    return true;
}

bool World::createZone(std::uint16_t id, assets::TerrainData terrain, std::string& error)
{
    return addZone(Zone::create(id, std::move(terrain), m_settings.zone, error), error);
}

bool World::loadZone(std::uint16_t id, const std::filesystem::path& file, std::string& error)
{
    return addZone(Zone::load(id, file, m_settings.zone, error), error);
}

Zone* World::zone(std::uint16_t id) noexcept
{
    const auto it = m_zones.find(id);
    return it == m_zones.end() ? nullptr : it->second.get();
}

auth::NewCharacterSpawns World::newCharacterSpawns() const
{
    auth::NewCharacterSpawns s;
    if (m_startZone)
    {
        s.aydor = m_startZone->raceSpawn(static_cast<std::uint8_t>(game::Race::Aydor));
        s.kargan = m_startZone->raceSpawn(static_cast<std::uint8_t>(game::Race::Kargan));
    }
    return s;
}

std::uint32_t World::serverTimeMs(core::MonoTime t) const noexcept
{
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t - m_epoch).count());
}

std::size_t World::playerCount() const noexcept
{
    std::size_t n = 0;
    for (const auto& [id, z] : m_zones)
        n += z->playerCount();
    return n;
}

// ---- Commands ----

void World::post(WorldCommand command)
{
    {
        const std::scoped_lock lock(m_queueMutex);
        if (!m_closed)
        {
            m_queue.push_back(std::move(command));
            return;
        }
    }
    if (auto* leave = std::get_if<LeaveCommand>(&command); leave && leave->done)
        leave->done(false);
}

void World::runCommands(core::MonoTime now)
{
    {
        const std::scoped_lock lock(m_queueMutex);
        m_running.swap(m_queue);
    }
    for (auto& command : m_running)
    {
        std::visit(
            [&](auto& cmd) {
                using T = std::decay_t<decltype(cmd)>;
                if constexpr (std::is_same_v<T, EnterCommand>)
                    onEnter(cmd, now);
                else if constexpr (std::is_same_v<T, MoveCommand>)
                    onMove(cmd);
                else
                    onLeave(cmd, now);
            },
            command);
    }
    m_running.clear();
}

Zone* World::zoneOfSession(SessionKey session) noexcept
{
    const auto it = m_sessionZone.find(session);
    return it == m_sessionZone.end() ? nullptr : zone(it->second);
}

EntityId World::allocateEntityId()
{
    for (;;)
    {
        const EntityId id = m_nextEntityId++;
        if (id == 0)
            continue;
        bool used = false;
        for (const auto& [zid, z] : m_zones)
            used = used || z->findEntity(id) != nullptr;
        if (!used)
            return id;
    }
}

void World::onEnter(EnterCommand& cmd, core::MonoTime now)
{
    if (m_sessionZone.contains(cmd.session))
    {
        VO_LOG_ERROR("world: session {} is already in the world, ignoring a second enter", cmd.session);
        return;
    }
    auth::CharacterInfo character = std::move(cmd.character);
    if (const auto it = m_characterSession.find(character.id); it != m_characterSession.end())
    {
        // The character is still in the world under an older session (duplicate login: the old
        // session is being kicked, its leave has not arrived yet). Its live position is the newest.
        const SessionKey old = it->second;
        if (Zone* z = zoneOfSession(old))
        {
            if (const auto left = z->leave(old))
                character.position = left->position;
        }
        m_sessionZone.erase(old);
        m_characterSession.erase(it);
        VO_LOG_INFO("world: character {} entered from session {}, replacing session {}", character.id, cmd.session,
                    old);
    }
    else if (const auto r = m_recent.find(character.id); r != m_recent.end())
    {
        // Left recently: its last position may not be in storage yet (the save is asynchronous).
        character.position = r->second.position;
        m_recent.erase(r);
    }

    Zone* z = zone(character.position.zoneId);
    if (!z)
        z = m_startZone;
    VO_VERIFY(z != nullptr);
    Zone::EnterParams params;
    params.entityId = allocateEntityId();
    params.session = cmd.session;
    params.generation = cmd.generation;
    params.link = std::move(cmd.link);
    params.account = cmd.account;
    params.character = std::move(character);
    const auth::CharacterId characterId = params.character.id;
    const EntityId entity = params.entityId;
    z->enter(std::move(params), now, serverTimeMs(now));
    m_sessionZone[cmd.session] = z->id();
    m_characterSession[characterId] = cmd.session;
    VO_LOG_DEBUG("world: session {} entered zone {} as entity {} (character {})", cmd.session, z->id(), entity,
                 characterId);
}

void World::onMove(MoveCommand& cmd)
{
    if (Zone* z = zoneOfSession(cmd.session))
        (void)z->move(cmd.session, cmd.request, cmd.receivedAt, serverTimeMs(cmd.receivedAt));
}

void World::onLeave(LeaveCommand& cmd, core::MonoTime now)
{
    Zone* z = zoneOfSession(cmd.session);
    std::optional<Zone::LeftPlayer> left;
    if (z)
        left = z->leave(cmd.session);
    m_sessionZone.erase(cmd.session);
    if (!left)
    {
        if (cmd.done)
            cmd.done(true); // nothing to save (never entered, or replaced by a newer session)
        return;
    }
    if (const auto it = m_characterSession.find(left->character); it != m_characterSession.end() &&
                                                                   it->second == cmd.session)
        m_characterSession.erase(it);
    m_recent[left->character] = {left->position, now};
    VO_LOG_DEBUG("world: session {} left (character {} at {:.2f}, {:.2f}, {:.2f})", cmd.session, left->character,
                 left->position.x, left->position.y, left->position.z);
    if (m_save)
    {
        auto done = std::move(cmd.done);
        const auth::CharacterId character = left->character;
        m_save(left->account, left->character, left->position, [character, done = std::move(done)](bool ok) {
            if (!ok)
                VO_LOG_WARN("world: saving the position of character {} failed", character);
            if (done)
                done(ok);
        });
    }
    else if (cmd.done)
    {
        cmd.done(true);
    }
}

void World::removeAllPlayers(core::MonoTime now)
{
    std::vector<SessionKey> sessions;
    sessions.reserve(m_sessionZone.size());
    for (const auto& [s, zid] : m_sessionZone)
        sessions.push_back(s);
    if (!sessions.empty())
        VO_LOG_INFO("world: saving {} player(s) still in the world", sessions.size());
    for (const SessionKey s : sessions)
    {
        LeaveCommand leave{s, nullptr};
        onLeave(leave, now);
    }
}

// ---- Tick ----

void World::tick(core::MonoTime now, core::MonoTime logical)
{
    runCommands(now);
    for (auto& [id, z] : m_zones)
        z->update(logical, m_tickIndex, m_save);
    for (auto& [id, z] : m_zones)
        z->flush();
    if (m_tickIndex % 20 == 0)
    {
        for (auto it = m_recent.begin(); it != m_recent.end();)
            it = now - it->second.at > m_settings.recentLeaveMemory ? m_recent.erase(it) : std::next(it);
    }
    ++m_tickIndex;
}

void World::start()
{
    if (running())
        return;
    m_stopRequested = false;
    m_thread = std::thread([this] { threadMain(); });
}

void World::stop()
{
    {
        const std::scoped_lock lock(m_queueMutex);
        if (m_closed)
            return;
    }
    m_stopRequested = true;
    if (m_thread.joinable())
        m_thread.join();
    // The owner is the only thread touching the world now. Run what is still queued, then close the
    // queue and take everyone out with a final save.
    const auto now = core::monoNow();
    runCommands(now);
    {
        const std::scoped_lock lock(m_queueMutex);
        m_closed = true;
    }
    runCommands(now); // commands posted between the two runs
    removeAllPlayers(now);
    for (auto& [id, z] : m_zones)
        z->flush();
    if (m_total.summary().ticks > 0)
        logStats(now, true);
}

TickStats::Summary World::totalStats() const
{
    const std::scoped_lock lock(m_statsMutex);
    return m_total.summary();
}

void World::threadMain()
{
    VO_LOG_INFO("world: simulation thread started ({} Hz, tick {:.1f} ms)", m_settings.tickRate, toMs(m_interval));
    TickTimer timer;
    auto next = core::monoNow();
    {
        const std::scoped_lock lock(m_statsMutex);
        m_windowStart = next;
        m_lastSlowWarn = next;
    }
    const auto maxLag = m_interval * std::max(m_settings.maxCatchUpTicks, 1);
    while (!m_stopRequested.load(std::memory_order_relaxed))
    {
        timer.waitUntil(next);
        if (m_stopRequested.load(std::memory_order_relaxed))
            break;
        auto now = core::monoNow();
        if (now - next > maxLag)
        {
            const auto behind = now - next;
            const auto skipped = static_cast<std::uint64_t>(behind / m_interval);
            VO_LOG_WARN("world: overloaded, {:.1f} ms behind schedule: skipping {} tick(s)", toMs(behind), skipped);
            {
                const std::scoped_lock lock(m_statsMutex);
                m_skippedTicks += skipped;
                m_windowSkipped += skipped;
            }
            next = now;
        }
        const auto started = core::monoNow();
        tick(now, next);
        const auto took = core::monoNow() - started;
        next += m_interval;

        bool warnSlow = false;
        std::uint64_t slowCount = 0;
        {
            const std::scoped_lock lock(m_statsMutex);
            m_total.record(took);
            m_window.record(took);
            if (took > m_interval)
            {
                ++m_slowSinceWarn;
                if (started - m_lastSlowWarn >= std::chrono::seconds(5))
                {
                    warnSlow = true;
                    slowCount = m_slowSinceWarn;
                    m_slowSinceWarn = 0;
                    m_lastSlowWarn = started;
                }
            }
        }
        if (warnSlow)
            VO_LOG_WARN("world: slow tick {:.1f} ms (budget {:.1f} ms); {} slow tick(s) since the last warning",
                        toMs(took), toMs(m_interval), slowCount);
        if (m_settings.statsInterval.count() > 0 && started - m_windowStart >= m_settings.statsInterval)
            logStats(started, false);
    }
    VO_LOG_INFO("world: simulation thread stopped after {} tick(s)", m_tickIndex);
}

void World::logStats(core::MonoTime now, bool final)
{
    ZoneCounters c;
    for (const auto& [id, z] : m_zones)
    {
        const auto& zc = z->counters();
        c.movesAccepted += zc.movesAccepted;
        c.movesRejected += zc.movesRejected;
        c.corrections += zc.corrections;
        c.relays += zc.relays;
        c.spawns += zc.spawns;
        c.despawns += zc.despawns;
        c.periodicSaves += zc.periodicSaves;
    }
    TickStats::Summary s;
    std::uint64_t skipped = 0;
    double seconds = 0.0;
    {
        const std::scoped_lock lock(m_statsMutex);
        if (final)
        {
            s = m_total.summary();
            skipped = m_skippedTicks;
        }
        else
        {
            s = m_window.summary();
            skipped = m_windowSkipped;
            seconds = core::toSeconds(now - m_windowStart);
            m_window.reset();
            m_windowSkipped = 0;
            m_windowStart = now;
        }
    }
    const ZoneCounters base = final ? ZoneCounters{} : m_windowBase;
    if (!final)
        m_windowBase = c;
    const auto rate = [&](std::uint64_t total, std::uint64_t before) {
        return seconds > 0.0 ? static_cast<double>(total - before) / seconds : 0.0;
    };
    if (final)
        VO_LOG_INFO("world: totals: {} ticks, tick avg {:.3f} ms, p50 {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms, "
                    "{} slow, {} skipped; moves {} accepted / {} rejected, {} corrections, {} relays, {} spawns, "
                    "{} despawns, {} periodic saves",
                    s.ticks, s.avgMs, s.p50Ms, s.p99Ms, s.maxMs, s.slowTicks, skipped, c.movesAccepted,
                    c.movesRejected, c.corrections, c.relays, c.spawns, c.despawns, c.periodicSaves);
    else
        VO_LOG_INFO("world: {:.0f} s: {} players, {} ticks, tick avg {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms, "
                    "{} slow, {} skipped; moves/s {:.0f} ok {:.1f} rejected, relays/s {:.0f}, spawns {}, "
                    "despawns {}, saves {}",
                    seconds, playerCount(), s.ticks, s.avgMs, s.p99Ms, s.maxMs, s.slowTicks, skipped,
                    rate(c.movesAccepted, base.movesAccepted), rate(c.movesRejected, base.movesRejected),
                    rate(c.relays, base.relays), c.spawns - base.spawns, c.despawns - base.despawns,
                    c.periodicSaves - base.periodicSaves);
}

} // namespace vo::server::world
