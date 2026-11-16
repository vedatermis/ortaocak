#include "server/world/Zone.h"

#include "shared/core/Assert.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Movement.h"
#include "shared/game/TerrainWalk.h"

#include <cmath>
#include <format>
#include <numbers>

namespace vo::server::world {

namespace {

auth::CharacterPosition toPosition(std::uint16_t zone, const assets::SpawnPoint& s, const assets::TerrainData& t)
{
    return {zone, s.position.x, t.heightAt(s.position.x, s.position.z), s.position.z, game::normalizeYaw(s.yaw)};
}

} // namespace

std::unique_ptr<Zone> Zone::create(std::uint16_t id, assets::TerrainData terrain, ZoneSettings settings,
                                   std::string& error)
{
    if (!(terrain.worldSize > 0.0f) || terrain.resolution < 2 || terrain.maskResolution == 0 ||
        terrain.walkMask.size() != static_cast<std::size_t>(terrain.maskResolution) * terrain.maskResolution)
    {
        error = "terrain has no usable heightmap or walk mask";
        return nullptr;
    }
    for (const std::string_view name : {kSpawnAydor, kSpawnKargan})
    {
        const assets::SpawnPoint* s = terrain.findSpawn(name);
        if (!s)
        {
            error = std::format("terrain has no spawn point '{}'", name);
            return nullptr;
        }
        if (!terrain.isWalkable(s->position.x, s->position.z))
        {
            error = std::format("spawn point '{}' ({:.1f}, {:.1f}) is not walkable", name, s->position.x,
                                s->position.z);
            return nullptr;
        }
    }
    if (settings.rules.bankSeconds <= 0.0f || settings.rules.walkSpeed <= 0.0f || settings.rules.runSpeed <= 0.0f)
    {
        error = "invalid movement rules";
        return nullptr;
    }
    return std::make_unique<Zone>(PrivateTag{}, id, std::move(terrain), std::move(settings));
}

std::unique_ptr<Zone> Zone::load(std::uint16_t id, const std::filesystem::path& file, ZoneSettings settings,
                                 std::string& error)
{
    assets::TerrainData terrain;
    if (!assets::loadTerrain(file, terrain, error))
        return nullptr;
    return create(id, std::move(terrain), std::move(settings), error);
}

Zone::Zone(PrivateTag, std::uint16_t id, assets::TerrainData terrain, ZoneSettings settings)
    : m_id(id), m_terrain(std::move(terrain)), m_settings(std::move(settings)),
      m_grid(m_terrain.worldSize, game::kAoiCellSize, game::kAoiViewRadiusCells),
      m_rng(m_settings.seed != 0 ? m_settings.seed : std::random_device{}())
{
    m_spawnAydor = toPosition(m_id, *m_terrain.findSpawn(kSpawnAydor), m_terrain);
    m_spawnKargan = toPosition(m_id, *m_terrain.findSpawn(kSpawnKargan), m_terrain);
}

auth::CharacterPosition Zone::raceSpawn(std::uint8_t race) const noexcept
{
    return race == static_cast<std::uint8_t>(game::Race::Kargan) ? m_spawnKargan : m_spawnAydor;
}

Zone::Player* Zone::find(SessionKey session)
{
    const auto it = m_bySession.find(session);
    return it == m_bySession.end() ? nullptr : findById(it->second);
}

Zone::Player* Zone::findById(EntityId id)
{
    const auto it = m_players.find(id);
    return it == m_players.end() ? nullptr : it->second.get();
}

const Zone::Player* Zone::findPlayer(SessionKey session) const
{
    return const_cast<Zone*>(this)->find(session);
}

const Zone::Player* Zone::findEntity(EntityId id) const
{
    return const_cast<Zone*>(this)->findById(id);
}

std::optional<auth::CharacterPosition> Zone::positionOf(SessionKey session) const
{
    const Player* p = findPlayer(session);
    if (!p)
        return std::nullopt;
    return auth::CharacterPosition{m_id, p->move.x, p->move.y, p->move.z, p->move.yaw};
}

bool Zone::placeable(float x, float z) const
{
    return std::isfinite(x) && std::isfinite(z) && m_terrain.isWalkable(x, z);
}

void Zone::spreadAround(float& x, float& z)
{
    if (!(m_settings.spawnSpread > 0.5f))
        return;
    std::uniform_real_distribution<float> angle(0.0f, 2.0f * std::numbers::pi_v<float>);
    std::uniform_real_distribution<float> radius(0.5f, m_settings.spawnSpread);
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const float a = angle(m_rng);
        const float r = radius(m_rng);
        const float cx = x + r * std::sin(a);
        const float cz = z + r * std::cos(a);
        if (game::isSegmentWalkable(m_terrain, x, z, cx, cz))
        {
            x = cx;
            z = cz;
            return;
        }
    }
}

// ---- Enter / leave ----

void Zone::enter(EnterParams params, core::MonoTime now, std::uint32_t nowMs)
{
    VO_ASSERT(!m_bySession.contains(params.session) && !m_players.contains(params.entityId));
    auto owned = std::make_unique<Player>();
    Player& p = *owned;
    p.id = params.entityId;
    p.session = params.session;
    p.generation = params.generation;
    p.link = std::move(params.link);
    p.account = params.account;
    p.character = std::move(params.character);

    const auth::CharacterPosition saved = p.character.position;
    const auth::CharacterPosition spawn = raceSpawn(p.character.race);
    float x = saved.x;
    float z = saved.z;
    float yaw = saved.yaw;
    bool atSpawn = false;
    if (saved.zoneId != m_id || !placeable(x, z))
    {
        x = spawn.x;
        z = spawn.z;
        yaw = spawn.yaw;
        atSpawn = true;
    }
    else if (x == spawn.x && z == spawn.z)
    {
        // Exactly the spawn point: a new character (created there), never moved. Spread like a
        // fresh spawn. A player who merely walked back near the spawn keeps its position.
        atSpawn = true;
    }
    if (atSpawn)
        spreadAround(x, z);

    p.move.x = x;
    p.move.y = m_terrain.heightAt(x, z);
    p.move.z = z;
    p.move.yaw = game::normalizeYaw(yaw);
    p.move.flags = 0;
    p.move.lastTime = now;
    p.move.credit = 0.0f;
    p.moveTimeMs = nowMs;
    p.cell = m_grid.cellOf(x, z);
    if (m_settings.savePeriodTicks > 0)
        p.saveSlot = static_cast<std::uint32_t>((static_cast<std::uint64_t>(p.character.id) * 2654435761ull) %
                                                m_settings.savePeriodTicks);
    p.character.position = {m_id, p.move.x, p.move.y, p.move.z, p.move.yaw};

    p.out.generation = p.generation;
    EnteredInfo info;
    info.entityId = p.id;
    info.zoneId = m_id;
    info.x = p.move.x;
    info.y = p.move.y;
    info.z = p.move.z;
    info.yaw = p.move.yaw;
    info.walkSpeed = m_settings.rules.walkSpeed;
    info.runSpeed = m_settings.rules.runSpeed;
    info.serverTimeMs = nowMs;
    p.out.entered = info;

    m_players.emplace(p.id, std::move(owned));
    m_bySession.emplace(p.session, p.id);
    m_grid.forEachInView(p.cell, [&](EntityId other) {
        if (Player* o = findById(other); o && o != &p)
            spawnPair(p, *o);
    });
    m_grid.add(p.id, p.cell);
}

std::optional<Zone::LeftPlayer> Zone::leave(SessionKey session)
{
    Player* p = find(session);
    if (!p)
        return std::nullopt;
    for (const EntityId other : p->known)
    {
        Player* o = findById(other);
        VO_ASSERT(o != nullptr);
        if (o && o->known.erase(p->id) > 0)
            sendDespawn(*o, p->id);
    }
    m_grid.remove(p->id, p->cell);
    LeftPlayer left{p->account, p->character.id, {m_id, p->move.x, p->move.y, p->move.z, p->move.yaw}};
    const EntityId id = p->id;
    m_bySession.erase(session);
    m_players.erase(id); // drops its pending packets
    return left;
}

// ---- Movement ----

std::optional<MoveVerdict> Zone::move(SessionKey session, const net::MoveReq& req, core::MonoTime at,
                                      std::uint32_t atMs)
{
    Player* p = find(session);
    if (!p)
        return std::nullopt;
    p->lastSeq = req.seq;
    const std::uint8_t oldFlags = p->move.flags;
    const MoveVerdict v = validateMove(m_terrain, m_settings.rules, p->move, req, at);
    if (v == MoveVerdict::Accepted)
    {
        ++m_counters.movesAccepted;
        p->relayDirty = true;
        p->saveDirty = true;
        p->moveTimeMs = atMs;
        const AoiCell cell = m_grid.cellOf(p->move.x, p->move.z);
        if (cell != p->cell)
        {
            const AoiCell from = p->cell;
            m_grid.move(p->id, from, cell);
            p->cell = cell;
            applyCellChange(*p, from, cell);
        }
    }
    else
    {
        ++m_counters.movesRejected;
        VO_LOG_TRACE("zone {}: entity {} move seq {} rejected ({})", m_id, p->id, req.seq, moveVerdictName(v));
        p->correctionPending = true;
        if (p->move.flags != oldFlags)
        {
            // The player now stands at the corrected position: observers must see it stop there.
            p->relayDirty = true;
            p->moveTimeMs = atMs;
        }
    }
    return v;
}

void Zone::setEquipment(SessionKey session, std::uint16_t mainHandTemplate, std::uint16_t offHandTemplate)
{
    Player* p = find(session);
    if (!p)
        return;
    if (p->character.mainHandTemplate == mainHandTemplate && p->character.offHandTemplate == offHandTemplate)
        return;
    p->character.mainHandTemplate = mainHandTemplate;
    p->character.offHandTemplate = offHandTemplate;
    const net::EntityEquip msg{p->id, mainHandTemplate, offHandTemplate};
    m_scratch.clear();
    msg.write(m_scratch);
    const auto payload = m_scratch.data();
    const auto op = static_cast<std::uint16_t>(net::Opcode::EntityEquip);
    p->out.append(op, payload);
    ++m_counters.equips;
    for (const EntityId other : p->known)
    {
        if (Player* o = findById(other))
        {
            o->out.append(op, payload);
            ++m_counters.equips;
        }
    }
}

void Zone::placeEntity(SessionKey session, float x, float z)
{
    Player* p = find(session);
    if (!p)
        return;
    p->move.x = x;
    p->move.z = z;
    p->move.y = m_terrain.heightAt(x, z);
    p->relayDirty = true;
    p->saveDirty = true;
    const AoiCell cell = m_grid.cellOf(x, z);
    if (cell != p->cell)
    {
        const AoiCell from = p->cell;
        m_grid.move(p->id, from, cell);
        p->cell = cell;
        applyCellChange(*p, from, cell);
    }
}

// ---- Area of interest ----

void Zone::applyCellChange(Player& p, AoiCell from, AoiCell to)
{
    m_grid.forEachViewChange(
        from, to,
        [&](EntityId other) {
            if (Player* o = findById(other); o && o != &p)
                despawnPair(p, *o);
        },
        [&](EntityId other) {
            if (Player* o = findById(other); o && o != &p)
                spawnPair(p, *o);
        });
}

void Zone::spawnPair(Player& a, Player& b)
{
    if (a.known.insert(b.id).second)
        sendSpawn(a, b);
    if (b.known.insert(a.id).second)
        sendSpawn(b, a);
}

void Zone::despawnPair(Player& a, Player& b)
{
    if (a.known.erase(b.id) > 0)
        sendDespawn(a, b.id);
    if (b.known.erase(a.id) > 0)
        sendDespawn(b, a.id);
}

void Zone::sendSpawn(Player& to, const Player& about)
{
    net::EntitySpawn s;
    s.entityId = about.id;
    s.entityType = net::kEntityTypePlayer;
    s.name = about.character.name;
    s.race = about.character.race;
    s.charClass = about.character.charClass;
    s.face = about.character.face;
    s.hair = about.character.hair;
    s.level = about.character.level;
    s.x = about.move.x;
    s.y = about.move.y;
    s.z = about.move.z;
    s.yaw = about.move.yaw;
    s.flags = about.move.flags;
    s.mainHandTemplate = about.character.mainHandTemplate;
    s.offHandTemplate = about.character.offHandTemplate;
    to.out.appendMessage(s, m_scratch);
    ++m_counters.spawns;
}

void Zone::sendDespawn(Player& to, EntityId about)
{
    to.out.appendMessage(net::EntityDespawn{about}, m_scratch);
    ++m_counters.despawns;
}

// ---- Per tick ----

void Zone::relay(Player& p, core::MonoTime logicalNow)
{
    if (p.everRelayed && logicalNow - p.lastRelay < m_settings.relayInterval)
        return; // stays dirty: the latest state goes out at the next allowed tick
    p.relayDirty = false;
    const auto& m = p.move;
    if (p.everRelayed && m.x == p.relayedX && m.y == p.relayedY && m.z == p.relayedZ && m.yaw == p.relayedYaw &&
        m.flags == p.relayedFlags)
        return; // nothing new (e.g. a repeated stop packet)
    p.everRelayed = true;
    p.lastRelay = logicalNow;
    p.relayedX = m.x;
    p.relayedY = m.y;
    p.relayedZ = m.z;
    p.relayedYaw = m.yaw;
    p.relayedFlags = m.flags;
    if (p.known.empty())
        return;
    net::EntityMove msg;
    msg.entityId = p.id;
    msg.flags = m.flags;
    msg.x = m.x;
    msg.y = m.y;
    msg.z = m.z;
    msg.yaw = m.yaw;
    msg.serverTimeMs = p.moveTimeMs;
    m_scratch.clear();
    msg.write(m_scratch);
    const auto payload = m_scratch.data();
    for (const EntityId other : p.known)
    {
        if (Player* o = findById(other))
        {
            o->out.append(static_cast<std::uint16_t>(net::Opcode::EntityMove), payload);
            ++m_counters.relays;
        }
    }
}

void Zone::update(core::MonoTime logicalNow, std::uint64_t tickIndex, const SavePositionFn& save)
{
    for (auto& [id, owned] : m_players)
    {
        Player& p = *owned;
        if (p.correctionPending)
        {
            p.correctionPending = false;
            net::PositionCorrect pc;
            pc.seq = p.lastSeq;
            pc.x = p.move.x;
            pc.y = p.move.y;
            pc.z = p.move.z;
            pc.yaw = p.move.yaw;
            p.out.appendMessage(pc, m_scratch);
            ++m_counters.corrections;
        }
    }
    for (auto& [id, owned] : m_players)
    {
        if (owned->relayDirty)
            relay(*owned, logicalNow);
    }
    if (m_settings.savePeriodTicks > 0 && save)
    {
        const auto phase = static_cast<std::uint32_t>(tickIndex % m_settings.savePeriodTicks);
        for (auto& [id, owned] : m_players)
        {
            Player& p = *owned;
            if (p.saveSlot != phase || !p.saveDirty)
                continue;
            p.saveDirty = false;
            ++m_counters.periodicSaves;
            const auth::CharacterId character = p.character.id;
            save(p.account, character, {m_id, p.move.x, p.move.y, p.move.z, p.move.yaw}, [character](bool ok) {
                if (!ok)
                    VO_LOG_WARN("world: periodic position save of character {} failed", character);
            });
        }
    }
}

void Zone::flush()
{
    for (auto& [id, owned] : m_players)
    {
        Player& p = *owned;
        if (p.out.empty())
            continue;
        if (p.link)
            p.link->deliver(std::move(p.out));
        p.out = WorldBatch{};
        p.out.generation = p.generation;
    }
}

} // namespace vo::server::world
