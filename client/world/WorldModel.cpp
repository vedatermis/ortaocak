#include "client/world/WorldModel.h"

#include "shared/core/Log.h"
#include "shared/net/Opcodes.h"

#include <cmath>

namespace vo::client::world {

namespace vn = vo::net;

WorldModel::WorldModel()
{
    m_entities.resize(kMaxEntities);
}

void WorldModel::enter(const vn::EnterWorldRes& res, const assets::TerrainData* terrain, double localNow,
                       bool running)
{
    m_terrain = terrain;
    m_localId = res.entityId;
    m_zoneId = res.zoneId;
    if (res.walkSpeed > 0.0f && std::isfinite(res.walkSpeed))
        m_params.walkSpeed = res.walkSpeed;
    if (res.runSpeed > 0.0f && std::isfinite(res.runSpeed))
        m_params.runSpeed = res.runSpeed;
    Vec3 p{res.x, res.y, res.z};
    if (terrain != nullptr && terrain->resolution >= 2)
        p.y = terrain->heightAt(p.x, p.z); // the y the server sends is not trusted for rendering
    m_local.reset(p, res.yaw, running);
    m_clock.reset();
    m_clock.observe(res.serverTimeMs, localNow);
    dropRemotes(); // PROTOCOL §5.4: nothing survives from a previous stay
    m_stats = {};
    m_leaving = false;
    m_localEquip.reset();
}

std::size_t WorldModel::countOf(std::uint8_t entityType) const noexcept
{
    std::size_t n = 0;
    for (const RemoteEntity& e : m_entities)
        n += e.active && e.type == entityType ? 1 : 0;
    return n;
}

RemoteEntity* WorldModel::find(std::uint32_t id) noexcept
{
    for (RemoteEntity& e : m_entities)
    {
        if (e.active && e.id == id)
            return &e;
    }
    return nullptr;
}

bool WorldModel::handleMessage(std::uint16_t opcode, std::span<const std::uint8_t> payload, double localNow)
{
    switch (static_cast<vn::Opcode>(opcode))
    {
    case vn::Opcode::EntitySpawn:
    {
        vn::EntitySpawn m;
        if (!vn::decodeMessage(payload, m))
            break;
        handleSpawn(m, localNow);
        return true;
    }
    case vn::Opcode::EntityMove:
    {
        vn::EntityMove m;
        if (!vn::decodeMessage(payload, m))
            break;
        handleMove(m, localNow);
        return true;
    }
    case vn::Opcode::EntityDespawn:
    {
        vn::EntityDespawn m;
        if (!vn::decodeMessage(payload, m))
            break;
        handleDespawn(m);
        return true;
    }
    case vn::Opcode::PositionCorrect:
    {
        vn::PositionCorrect m;
        if (!vn::decodeMessage(payload, m))
            break;
        handleCorrection(m);
        return true;
    }
    case vn::Opcode::EntityEquip:
    {
        vn::EntityEquip m;
        if (!vn::decodeMessage(payload, m))
            break;
        handleEquip(m);
        return true;
    }
    default:
        return false;
    }
    ++m_stats.decodeErrors;
    return false;
}

void WorldModel::handleSpawn(const vn::EntitySpawn& m, double localNow)
{
    if (m.entityId == m_localId)
        return;
    if (m.entityType != vn::kEntityTypePlayer && m.entityType != vn::kEntityTypeMonster &&
        m.entityType != vn::kEntityTypeLoot)
    {
        // A future entity type: not drawn; its ENTITY_MOVE / ENTITY_DESPAWN count as unknown.
        ++m_stats.ignoredSpawns;
        return;
    }
    RemoteEntity* e = find(m.entityId);
    if (e == nullptr)
    {
        for (RemoteEntity& slot : m_entities)
        {
            if (!slot.active)
            {
                e = &slot;
                break;
            }
        }
        if (e == nullptr)
        {
            VO_LOG_WARN("world: entity table full ({}), ignoring spawn of {}", kMaxEntities, m.entityId);
            return;
        }
        ++m_active;
    }
    ++m_stats.spawns;
    e->active = true;
    e->id = m.entityId;
    e->type = m.entityType;
    e->name = m.name;
    e->race = m.race;
    e->charClass = m.charClass;
    e->face = m.face;
    e->hair = m.hair;
    e->level = m.level;
    e->mainHand = m.mainHandTemplate;
    e->offHand = m.offHandTemplate;
    e->armor = m.armorTemplates; // M17
    e->equipChanges = 0;
    e->hp = m.hp;
    e->maxHp = m.maxHp;
    e->templateId = m.templateId;
    e->state = m.state;
    e->scale = 1.0f;
    e->hpShownUntil = 0.0f;
    e->swings = 0;
    e->facing = 0;
    e->facingUntil = 0.0f;
    e->casting = false;
    e->animatorMesh = nullptr; // rebind: a reused slot may have held another kind of model
    e->animatorClips = nullptr;
    if (m.entityType == vn::kEntityTypeMonster)
        ++m_stats.monsterSpawns;
    if (m.entityType == vn::kEntityTypeLoot)
        ++m_stats.lootSpawns;
    e->motion.clear();
    Vec3 p{m.x, m.y, m.z};
    if (m_terrain != nullptr && m_terrain->resolution >= 2)
        p.y = m_terrain->heightAt(p.x, p.z);
    e->motion.push(m_clock.now(localNow) - kSpawnBackdate, p, m.yaw, m.flags);
    e->position = p;
    e->yaw = wrapAngle(m.yaw);
    e->speed = 0.0f;
    e->moving = (m.flags & vn::kMoveFlagMoving) != 0;
    e->running = (m.flags & vn::kMoveFlagRunning) != 0;
    e->extrapolating = false;
    e->holding = true;
    e->age = 0.0f;
}

void WorldModel::handleMove(const vn::EntityMove& m, double localNow)
{
    m_clock.observe(m.serverTimeMs, localNow);
    RemoteEntity* e = find(m.entityId);
    if (e == nullptr)
    {
        ++m_stats.unknownEntity;
        return;
    }
    ++m_stats.moves;
    e->motion.push(m_clock.toSeconds(m.serverTimeMs), Vec3{m.x, m.y, m.z}, m.yaw, m.flags);
}

void WorldModel::handleDespawn(const vn::EntityDespawn& m)
{
    RemoteEntity* e = find(m.entityId);
    if (e == nullptr)
    {
        ++m_stats.unknownEntity;
        return;
    }
    ++m_stats.despawns;
    if (e->isLoot())
        ++m_stats.lootDespawns;
    e->active = false;
    e->motion.clear();
    --m_active;
}

void WorldModel::handleCorrection(const vn::PositionCorrect& m)
{
    if (m_local.applyCorrection(m.seq, Vec3{m.x, m.y, m.z}, m.yaw, m_terrain))
    {
        ++m_stats.corrections;
        VO_LOG_INFO("world: POSITION_CORRECT seq {} -> ({:.2f}, {:.2f}, {:.2f}) applied", m.seq, m.x, m.y, m.z);
    }
    else
    {
        ++m_stats.ignoredCorrections;
        VO_LOG_DEBUG("world: POSITION_CORRECT seq {} ignored (stale)", m.seq);
    }
}

void WorldModel::handleEquip(const vn::EntityEquip& m)
{
    ++m_stats.equips;
    if (m.entityId == m_localId)
    {
        m_localEquip = m;
        return;
    }
    RemoteEntity* e = find(m.entityId);
    if (e == nullptr)
    {
        ++m_stats.unknownEntity;
        return;
    }
    e->mainHand = m.mainHandTemplate;
    e->offHand = m.offHandTemplate;
    e->armor = m.armorTemplates; // M17
    ++e->equipChanges;
    VO_LOG_DEBUG("world: entity {} '{}' now holds {} / {}", e->id, e->name, e->mainHand, e->offHand);
}

void WorldModel::beginLeaving()
{
    m_leaving = true;
    m_local.mover().clearDestination();
}

void WorldModel::dropRemotes()
{
    for (RemoteEntity& e : m_entities)
    {
        e.active = false;
        e.motion.clear();
    }
    m_active = 0;
}

std::optional<vn::MoveReq> WorldModel::updateLocal(double localNow, const MoveIntent& intent, float dt)
{
    if (m_leaving)
    {
        // Let the character come to a stop on screen, but never tell the server.
        MoveIntent idle;
        idle.cameraYaw = intent.cameraYaw;
        idle.running = intent.running;
        m_local.mover().clearDestination();
        (void)m_local.update(localNow, idle, m_params, m_terrain, dt);
        return std::nullopt;
    }
    const std::optional<MoveReqData> d = m_local.update(localNow, intent, m_params, m_terrain, dt);
    if (!d)
        return std::nullopt;
    vn::MoveReq req;
    req.seq = d->seq;
    req.flags = d->flags;
    req.x = d->position.x;
    req.y = d->position.y;
    req.z = d->position.z;
    req.yaw = d->yaw;
    return req;
}

void WorldModel::updateRemotes(double localNow, float dt)
{
    const double renderTime = m_clock.now(localNow) - SnapshotBuffer::kInterpolationDelay;
    for (RemoteEntity& e : m_entities)
    {
        if (!e.active)
            continue;
        const MotionSample s = e.motion.sample(renderTime);
        Vec3 p = s.position;
        if (m_terrain != nullptr && m_terrain->resolution >= 2)
            p.y = m_terrain->heightAt(p.x, p.z); // feet on the ground between snapshots
        e.speed = std::sqrt(s.velocity.x * s.velocity.x + s.velocity.z * s.velocity.z);
        e.position = p;
        // Face the travel direction while moving (the yaw in the packets is the sender's facing,
        // which turns smoothly anyway); otherwise the interpolated yaw.
        e.yaw = s.yaw;
        e.moving = (s.flags & vn::kMoveFlagMoving) != 0 && !s.holding;
        e.running = (s.flags & vn::kMoveFlagRunning) != 0;
        e.extrapolating = s.extrapolating;
        e.holding = s.holding;
        e.age += dt;
    }
}

} // namespace vo::client::world
