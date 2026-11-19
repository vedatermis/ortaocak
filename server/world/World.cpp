#include "server/world/World.h"

#include "server/db/Db.h"
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
#include <cmath>
#include <format>
#include <string>
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

void applyProgress(auth::CharacterInfo& c, const auth::CharacterProgress& p)
{
    c.level = p.level;
    c.xp = p.xp;
    c.hp = p.hp;
    c.mp = p.mp;
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

bool World::createZone(std::uint16_t id, assets::TerrainData terrain, std::string& error, ZoneLayout layout)
{
    return addZone(Zone::create(id, std::move(terrain), m_settings.zone, error, std::move(layout)), error);
}

bool World::loadZone(std::uint16_t id, const std::filesystem::path& file, std::string& error, ZoneLayout layout)
{
    return addZone(Zone::load(id, file, m_settings.zone, error, std::move(layout)), error);
}

bool World::finalizeZones(std::uint16_t homeAydor, std::uint16_t homeKargan, std::string& error)
{
    if (running())
    {
        error = "the world is running";
        return false;
    }
    for (const auto& [id, z] : m_zones)
    {
        for (const ZoneLayout::Gate& gate : z->layout().gates)
        {
            Zone* target = zone(gate.def.toZone);
            if (!target)
            {
                error = std::format("zone {}: gate '{}' leads to zone {}, which is not loaded", id, gate.def.marker,
                                    gate.def.toZone);
                return false;
            }
            const auto arrive = target->marker(gate.def.arrive);
            if (!arrive)
            {
                error = std::format("zone {}: the arrival marker '{}' of gate '{}' (zone {}) is missing", target->id(),
                                    gate.def.arrive, gate.def.marker, id);
                return false;
            }
            if (!target->placeable(arrive->x, arrive->z))
            {
                error = std::format("zone {}: the arrival marker '{}' ({:.1f}, {:.1f}) is not walkable", target->id(),
                                    gate.def.arrive, arrive->x, arrive->z);
                return false;
            }
            for (const ZoneLayout::Gate& other : target->layout().gates)
            {
                const auto at = target->marker(other.def.marker);
                const float d = at ? std::hypot(arrive->x - at->x, arrive->z - at->z) : 1e9f;
                if (d <= other.def.radius + game::kArrivalGateMargin)
                {
                    error = std::format("zone {}: the arrival marker '{}' is {:.1f} m from gate '{}' (it must be more "
                                        "than radius {:.1f} + {:.1f} m away)",
                                        target->id(), gate.def.arrive, d, other.def.marker, other.def.radius,
                                        game::kArrivalGateMargin);
                    return false;
                }
            }
        }
    }
    const std::uint16_t homes[3] = {0, homeAydor, homeKargan};
    for (std::uint8_t race = 1; race <= 2; ++race)
    {
        const std::uint16_t zid = homes[race] != 0 ? homes[race] : (m_startZone ? m_startZone->id() : std::uint16_t{0});
        const Zone* h = zone(zid);
        if (!h)
        {
            error = std::format("the home zone {} of race {} is not loaded", zid, race);
            return false;
        }
        if (!h->allowsRace(race))
        {
            error = std::format("the home zone {} does not allow race {}", zid, race);
            return false;
        }
        m_homeZone[race] = zid;
    }
    return true;
}

Zone* World::homeZone(std::uint8_t race) noexcept
{
    const std::uint16_t zid = race >= 1 && race <= 2 ? m_homeZone[race] : std::uint16_t{0};
    if (Zone* z = zid != 0 ? zone(zid) : nullptr)
        return z;
    return m_startZone;
}

std::uint16_t World::zoneIdOf(SessionKey session) const noexcept
{
    const auto it = m_sessionZone.find(session);
    return it == m_sessionZone.end() ? std::uint16_t{0} : it->second;
}

Zone* World::zone(std::uint16_t id) noexcept
{
    const auto it = m_zones.find(id);
    return it == m_zones.end() ? nullptr : it->second.get();
}

auth::NewCharacterSpawns World::newCharacterSpawns() const
{
    auth::NewCharacterSpawns s;
    auto* self = const_cast<World*>(this);
    const auto aydor = static_cast<std::uint8_t>(game::Race::Aydor);
    const auto kargan = static_cast<std::uint8_t>(game::Race::Kargan);
    if (Zone* z = self->homeZone(aydor))
        s.aydor = z->raceSpawn(aydor);
    if (Zone* z = self->homeZone(kargan))
        s.kargan = z->raceSpawn(kargan);
    return s;
}

bool World::installMonsters(std::shared_ptr<const game::MonsterData> data, std::string& error)
{
    if (running())
    {
        error = "the world is running";
        return false;
    }
    const auto now = core::monoNow();
    for (auto& [id, z] : m_zones)
    {
        z->beginTick(now, serverTimeMs(now));
        if (!z->installMonsters(data, [this] { return allocateEntityId(); }, now, error))
            return false;
    }
    return true;
}

void World::installLoot(std::shared_ptr<const game::LootTables> tables)
{
    for (auto& [id, z] : m_zones)
        z->installLoot(tables, [this] { return allocateEntityId(); });
}

bool World::installNpcs(std::shared_ptr<const game::NpcCatalog> catalog, const game::ItemCatalog& items,
                        std::string& error)
{
    if (running())
    {
        error = "the world is running";
        return false;
    }
    for (auto& [id, z] : m_zones)
        if (!z->installNpcs(catalog, items, [this] { return allocateEntityId(); }, error))
            return false;
    return true;
}

std::size_t World::npcCount() const noexcept
{
    std::size_t n = 0;
    for (const auto& [id, z] : m_zones)
        n += z->npcCount();
    return n;
}

std::size_t World::monsterCount() const noexcept
{
    std::size_t n = 0;
    for (const auto& [id, z] : m_zones)
        n += z->monsterCount();
    return n;
}

std::uint32_t World::serverTimeMs(core::MonoTime t) const noexcept
{
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t - m_epoch).count());
}

std::size_t World::playerCount() const noexcept
{
    std::size_t n = m_transit.size();
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
                else if constexpr (std::is_same_v<T, EquipCommand>)
                    onEquip(cmd);
                else if constexpr (std::is_same_v<T, CombatCommand>)
                    onCombat(cmd);
                else if constexpr (std::is_same_v<T, LootCommand>)
                    onLoot(cmd);
                else if constexpr (std::is_same_v<T, LootSettleCommand>)
                    onLootSettle(cmd);
                else if constexpr (std::is_same_v<T, StatsCommand>)
                    onStats(cmd);
                else if constexpr (std::is_same_v<T, SkillsCommand>)
                    onSkills(cmd);
                else if constexpr (std::is_same_v<T, ZoneArrivedCommand>)
                    onZoneArrived(cmd, now);
                else if constexpr (std::is_same_v<T, ZoneReadyCommand>)
                    onZoneReady(cmd);
                else if constexpr (std::is_same_v<T, ShopCommand>)
                    onShop(cmd);
                else if constexpr (std::is_same_v<T, ItemUseCommand>)
                    onItemUse(cmd);
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
            used = used || z->hasEntity(id);
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
    std::vector<Zone::RememberedCooldown> cooldowns;
    if (const auto it = m_characterSession.find(character.id); it != m_characterSession.end())
    {
        // The character is still in the world under an older session (duplicate login: the old
        // session is being kicked, its leave has not arrived yet). Its live position is the newest.
        const SessionKey old = it->second;
        if (Zone* z = zoneOfSession(old))
        {
            if (const auto left = z->leave(old))
            {
                character.position = left->position;
                applyProgress(character, left->progress);
                cooldowns = std::move(left->cooldowns);
            }
        }
        else if (const auto t = m_transit.find(old); t != m_transit.end())
        {
            // Between two zones: its arrival (being stored right now) is the newest place.
            character.position = t->second.params.character.position;
            applyProgress(character, t->second.progress);
            cooldowns = std::move(t->second.params.cooldowns);
            if (t->second.leaving && t->second.leaveDone)
                t->second.leaveDone(true);
            m_transit.erase(t); // its ZoneArrivedCommand finds nothing and is ignored
        }
        m_sessionZone.erase(old);
        m_characterSession.erase(it);
        VO_LOG_INFO("world: character {} entered from session {}, replacing session {}", character.id, cmd.session,
                    old);
    }
    else if (const auto r = m_recent.find(character.id); r != m_recent.end())
    {
        // Left recently: its last state may not be in storage yet (the save is asynchronous). Levels
        // only go up in play, so a stored level above the cached one is newer (vo_admin set-level).
        character.position = r->second.position;
        if (character.level <= r->second.progress.level)
            applyProgress(character, r->second.progress);
        cooldowns = std::move(r->second.cooldowns);
        m_recent.erase(r);
    }

    // PROTOCOL_ZONES.md §4: the saved zone must be loaded and allow the race, the position must be
    // usable; otherwise the race spawn of the home zone (Zone::enter spreads it).
    Zone* z = zone(character.position.zoneId);
    if (!z || !z->allowsRace(character.race) || !z->placeable(character.position.x, character.position.z))
    {
        Zone* home = homeZone(character.race);
        VO_VERIFY(home != nullptr);
        if (z != nullptr || character.position.zoneId != 0)
            VO_LOG_INFO("world: character {} saved in zone {} at ({:.1f}, {:.1f}) cannot be placed there; "
                        "using the race spawn of its home zone {}",
                        character.id, character.position.zoneId, character.position.x, character.position.z, home->id());
        z = home;
        character.position = z->raceSpawn(character.race);
    }
    VO_VERIFY(z != nullptr);
    Zone::EnterParams params;
    params.entityId = allocateEntityId();
    params.session = cmd.session;
    params.generation = cmd.generation;
    params.link = std::move(cmd.link);
    params.account = cmd.account;
    params.character = std::move(character);
    params.equip = cmd.equip;
    params.cooldowns = std::move(cooldowns);
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

void World::onEquip(EquipCommand& cmd)
{
    if (Zone* z = zoneOfSession(cmd.session))
        z->setEquipment(cmd.session, cmd.mainHandTemplate, cmd.offHandTemplate, cmd.equip);
    else if (const auto t = m_transit.find(cmd.session); t != m_transit.end())
    {
        // Between two zones (PROTOCOL_ZONES.md §2.3): the new zone gets the committed equipment.
        t->second.params.equip = cmd.equip;
        t->second.params.character.mainHandTemplate = cmd.mainHandTemplate;
        t->second.params.character.offHandTemplate = cmd.offHandTemplate;
    }
}

void World::onCombat(CombatCommand& cmd)
{
    if (Zone* z = zoneOfSession(cmd.session))
        z->combat(cmd);
}

void World::onLoot(LootCommand& cmd)
{
    if (Zone* z = zoneOfSession(cmd.session))
        z->loot(cmd);
}

void World::onStats(StatsCommand& cmd)
{
    if (Zone* z = zoneOfSession(cmd.session))
        z->setAllocation(cmd.session, cmd.allocated);
    else if (const auto t = m_transit.find(cmd.session); t != m_transit.end())
        t->second.params.character.statPoints = cmd.allocated;
}

void World::onSkills(SkillsCommand& cmd)
{
    if (Zone* z = zoneOfSession(cmd.session))
        z->setSkillRanks(cmd.session, cmd.ranks);
    else if (const auto t = m_transit.find(cmd.session); t != m_transit.end())
        t->second.params.character.skillRanks = cmd.ranks;
}

void World::onZoneReady(ZoneReadyCommand& cmd)
{
    if (Zone* z = zoneOfSession(cmd.session))
        z->setReady(cmd.session);
}

void World::onShop(ShopCommand& cmd)
{
    // Between two zones the request is dropped (PROTOCOL_SHOP.md §2.2, like loot requests).
    if (Zone* z = zoneOfSession(cmd.session))
        z->shop(cmd);
}

void World::onItemUse(ItemUseCommand& cmd)
{
    if (Zone* z = zoneOfSession(cmd.session))
    {
        z->useItem(cmd);
        return;
    }
    if (cmd.kind != ItemUseCommand::Kind::Apply)
        return; // a use between two zones is dropped like other requests
    if (const auto t = m_transit.find(cmd.session); t != m_transit.end())
    {
        // PROTOCOL_SHOP.md §2.3: consumed while changing zone; the arrival carries the restore
        // (Zone::enter clamps to the maxima). No ITEM_USED.
        auth::CharacterInfo& c = t->second.params.character;
        if (c.hp && *c.hp > 0)
            c.hp = *c.hp + cmd.restoreHp;
        if (c.mp)
            c.mp = *c.mp + cmd.restoreMp;
    }
}

void World::runGateTriggers(core::MonoTime now)
{
    for (auto& [id, z] : m_zones)
    {
        for (const Zone::GateTrigger& trigger : z->takeGateTriggers())
            beginZoneChange(*z, trigger, now);
    }
}

void World::beginZoneChange(Zone& from, const Zone::GateTrigger& trigger, core::MonoTime now)
{
    if (trigger.gate >= from.layout().gates.size())
        return;
    const ZoneLayout::Gate& gate = from.layout().gates[trigger.gate];
    Zone* to = zone(gate.def.toZone);
    if (!to || !from.findPlayer(trigger.session))
        return; // finalizeZones() made sure the target exists
    const auto arrival = to->arrivalPosition(gate.def.arrive);
    if (!arrival)
        return;
    auto d = from.depart(trigger.session);
    if (!d)
        return;
    // PROTOCOL_ZONES.md §3.3 step 1-2: what the old zone produced for the player in this tick, then
    // ZONE_CHANGE_BEGIN, in one batch.
    d->pending.generation = d->generation;
    d->pending.zoneChangeBegin = net::ZoneChangeBegin{from.id(), to->id()};
    if (d->link)
        d->link->deliver(std::move(d->pending));

    Transit t;
    t.token = m_nextTransitToken++;
    t.fromZone = from.id();
    t.toZone = to->id();
    t.origin = d->left.position;
    t.progress = d->left.progress;
    t.params.session = trigger.session;
    t.params.generation = d->generation;
    t.params.link = std::move(d->link);
    t.params.account = d->left.account;
    t.params.character = std::move(d->character);
    t.params.character.position = *arrival;
    t.params.equip = d->equip;
    t.params.cooldowns = std::move(d->left.cooldowns);
    t.params.zoneChange = true;
    t.params.fromZone = from.id();
    t.params.previousEntityId = d->entityId;
    t.params.effects = std::move(d->effects);
    t.begunAt = now;
    const SessionKey session = trigger.session;
    const std::uint64_t token = t.token;
    const auth::AccountId account = t.params.account;
    const auth::CharacterId character = t.params.character.id;
    const auth::CharacterProgress progress = t.progress;
    m_sessionZone.erase(session);
    m_transit[session] = std::move(t);
    ++m_zoneChanges;
    VO_LOG_DEBUG("world: session {} (character {}) takes gate '{}' from zone {} to zone {} at ({:.1f}, {:.1f})", session,
                 character, gate.def.marker, from.id(), to->id(), arrival->x, arrival->z);
    // §3.5: the arrival is stored before the player appears in the new zone.
    if (m_save)
        m_save(account, character, *arrival, progress, [this, session, token](bool ok) {
            post(ZoneArrivedCommand{session, token, ok});
        });
    else
        post(ZoneArrivedCommand{session, token, true});
}

void World::onZoneArrived(ZoneArrivedCommand& cmd, core::MonoTime now)
{
    const auto it = m_transit.find(cmd.session);
    if (it == m_transit.end() || it->second.token != cmd.token)
        return; // replaced by a duplicate login meanwhile
    if (m_settings.testingZoneArrivalDelay.count() > 0 && now < it->second.begunAt + m_settings.testingZoneArrivalDelay)
    {
        m_deferredArrivals.push_back(cmd); // testing: held back, tried again every tick
        return;
    }
    Transit t = std::move(it->second);
    m_transit.erase(it);
    const auth::CharacterId character = t.params.character.id;
    if (!cmd.stored)
    {
        ++m_zoneChangeFailures;
        VO_LOG_WARN("world: storing the arrival of character {} in zone {} failed; back to zone {}", character, t.toZone,
                    t.fromZone);
    }
    const auth::CharacterPosition where = cmd.stored ? t.params.character.position : t.origin;
    if (t.leaving)
    {
        // §3.5: the leave waited for the write. Stored: done; not stored: save where it stood.
        if (const auto c = m_characterSession.find(character); c != m_characterSession.end() && c->second == cmd.session)
            m_characterSession.erase(c);
        m_recent[character] = {where, t.progress, now, t.params.cooldowns};
        if (cmd.stored || !m_save)
        {
            if (t.leaveDone)
                t.leaveDone(cmd.stored);
            return;
        }
        auto done = std::move(t.leaveDone);
        m_save(t.params.account, character, where, t.progress, [character, done = std::move(done)](bool ok) {
            if (!ok)
                VO_LOG_WARN("world: saving the position of character {} failed", character);
            if (done)
                done(ok);
        });
        return;
    }
    Zone* z = zone(where.zoneId);
    VO_VERIFY(z != nullptr);
    t.params.character.position = where;
    t.params.entityId = allocateEntityId();
    t.params.zoneChangeResult = cmd.stored ? net::ZoneChangeResult::Ok : net::ZoneChangeResult::Failed;
    const EntityId entity = t.params.entityId;
    z->enter(std::move(t.params), now, serverTimeMs(now));
    m_sessionZone[cmd.session] = z->id();
    VO_LOG_DEBUG("world: session {} arrived in zone {} as entity {} (character {})", cmd.session, z->id(), entity,
                 character);
}

void World::onLootSettle(LootSettleCommand& cmd)
{
    // The reservation lives in the zone of the container, whatever happened to the player since.
    for (auto& [id, z] : m_zones)
        if (z->settleLoot(cmd))
            return;
    VO_LOG_ERROR("world: settle of loot reservation {} matches no zone", cmd.reservation);
}

void World::onLeave(LeaveCommand& cmd, core::MonoTime now)
{
    if (const auto t = m_transit.find(cmd.session); t != m_transit.end())
    {
        // PROTOCOL_ZONES.md §3.5: between two zones the leave waits for the arrival's write.
        for (auto& [id, zone] : m_zones)
            zone->cancelLootTakes(cmd.session, cmd.lootInFlight);
        if (t->second.leaving && t->second.leaveDone)
            t->second.leaveDone(true); // a second leave of the same session (cannot happen in practice)
        t->second.leaving = true;
        t->second.leaveDone = std::move(cmd.done);
        return;
    }
    Zone* z = zoneOfSession(cmd.session);
    std::optional<Zone::LeftPlayer> left;
    if (z)
        left = z->leave(cmd.session);
    m_sessionZone.erase(cmd.session);
    // PROTOCOL_LOOT.md §3.4: takes the session never started writing go back to their containers
    // (also for a session already replaced by a duplicate login, which has no entity any more).
    for (auto& [id, zone] : m_zones)
        zone->cancelLootTakes(cmd.session, cmd.lootInFlight);
    if (!left)
    {
        if (cmd.done)
            cmd.done(true); // nothing to save (never entered, or replaced by a newer session)
        return;
    }
    if (const auto it = m_characterSession.find(left->character); it != m_characterSession.end() &&
                                                                   it->second == cmd.session)
        m_characterSession.erase(it);
    m_recent[left->character] = {left->position, left->progress, now, left->cooldowns};
    VO_LOG_DEBUG("world: session {} left (character {} at {:.2f}, {:.2f}, {:.2f}, level {}, hp {})", cmd.session,
                 left->character, left->position.x, left->position.y, left->position.z, left->progress.level,
                 left->progress.hp);
    if (m_save)
    {
        auto done = std::move(cmd.done);
        const auth::CharacterId character = left->character;
        m_save(left->account, left->character, left->position, left->progress,
               [character, done = std::move(done)](bool ok) {
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
    // Between two zones: their arrival's write may never be answered here any more (the queue is
    // closed). Save the arrival again (the same values; one account's writes run in order).
    for (auto& [session, t] : m_transit)
    {
        const auth::CharacterId character = t.params.character.id;
        auto done = std::move(t.leaveDone);
        if (m_save)
            m_save(t.params.account, character, t.params.character.position, t.progress,
                   [character, done = std::move(done)](bool ok) {
                       if (!ok)
                           VO_LOG_WARN("world: saving the arrival of character {} failed", character);
                       if (done)
                           done(ok);
                   });
        else if (done)
            done(true);
    }
    m_transit.clear();
    m_deferredArrivals.clear();
}

// ---- Tick ----

void World::tick(core::MonoTime now, core::MonoTime logical)
{
    if (m_tickHook)
        m_tickHook(m_tickIndex);
    for (auto& [id, z] : m_zones)
        z->beginTick(logical, serverTimeMs(logical));
    runCommands(now);
    if (!m_deferredArrivals.empty())
    {
        std::vector<ZoneArrivedCommand> due;
        due.swap(m_deferredArrivals);
        for (auto& cmd : due)
            onZoneArrived(cmd, now); // defers again until the testing delay has passed
    }
    for (auto& [id, z] : m_zones)
        z->update(logical, m_tickIndex, m_save);
    runGateTriggers(now); // PROTOCOL_ZONES.md §2: the gates that fired in this tick
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

std::uint64_t World::skippedTicks() const
{
    const std::scoped_lock lock(m_statsMutex);
    return m_skippedTicks;
}

void World::threadMain()
{
    db::markThreadNoDb(); // the world never waits for the database
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
        m_ticksRun.fetch_add(1, std::memory_order_relaxed);

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
        c.combatEvents += zc.combatEvents;
        c.monsterKills += zc.monsterKills;
        c.playerDeaths += zc.playerDeaths;
        c.pathSearches += zc.pathSearches;
        c.lootDrops += zc.lootDrops;
        c.lootTakes += zc.lootTakes;
        c.lootReturns += zc.lootReturns;
        c.lootDespawns += zc.lootDespawns;
        c.dodges += zc.dodges;
        c.gateRefusals += zc.gateRefusals;
        c.shopOpens += zc.shopOpens;
        c.shopGrants += zc.shopGrants;
        c.itemUses += zc.itemUses;
        c.itemRestores += zc.itemRestores;
        c.upgradeGrants += zc.upgradeGrants;
    }
    std::string perZone;
    for (const auto& [id, z] : m_zones)
        perZone += std::format("{}{}: {}", perZone.empty() ? "" : ", ", id, z->playerCount());
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
                    "{} despawns, {} periodic saves; combat: {} events, {} monster kills, {} player deaths, {} dodges, {} "
                    "path searches; loot: {} drops, {} takes, {} returns, {} despawns; zones: {} changes, {} failed, "
                    "{} refusals; shops: {} opened, {} buys/sells, {} upgrade attempts; consumables: {} used, {} restored",
                    s.ticks, s.avgMs, s.p50Ms, s.p99Ms, s.maxMs, s.slowTicks, skipped, c.movesAccepted,
                    c.movesRejected, c.corrections, c.relays, c.spawns, c.despawns, c.periodicSaves, c.combatEvents,
                    c.monsterKills, c.playerDeaths, c.dodges, c.pathSearches, c.lootDrops, c.lootTakes, c.lootReturns,
                    c.lootDespawns, m_zoneChanges, m_zoneChangeFailures, c.gateRefusals, c.shopOpens, c.shopGrants,
                    c.upgradeGrants, c.itemUses, c.itemRestores);
    else
        VO_LOG_INFO("world: {:.0f} s: {} players (zone {}; {} between zones), {} ticks, tick avg {:.3f} ms, p99 "
                    "{:.3f} ms, max {:.3f} ms, {} slow, {} skipped; moves/s {:.0f} ok {:.1f} rejected, relays/s {:.0f}, "
                    "spawns {}, despawns {}, saves {}; combat events/s {:.1f}, kills {}, deaths {}; zone changes {}",
                    seconds, playerCount(), perZone, m_transit.size(), s.ticks, s.avgMs, s.p99Ms, s.maxMs, s.slowTicks,
                    skipped, rate(c.movesAccepted, base.movesAccepted), rate(c.movesRejected, base.movesRejected),
                    rate(c.relays, base.relays), c.spawns - base.spawns, c.despawns - base.despawns,
                    c.periodicSaves - base.periodicSaves, rate(c.combatEvents, base.combatEvents),
                    c.monsterKills - base.monsterKills, c.playerDeaths - base.playerDeaths, m_zoneChanges);
}

} // namespace vo::server::world
