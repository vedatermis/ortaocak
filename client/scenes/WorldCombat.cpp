// WorldScene, combat half (M9 "İlk Kan", docs/PROTOCOL_COMBAT.md): monsters, targeting, auto-attack,
// the class skill, damage numbers, death / respawn, level-up, the server-driven stance and the
// combat HUD. The pure rules live in client/world/CombatView.h; this file wires them to the network,
// the input, the animators and the screen.
#include "client/scenes/WorldScene.h"

#include "client/net/GameClient.h"
#include "client/platform/Input.h"
#include "client/render/AssetCache.h"
#include "client/render/TextRenderer.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/CharacterPreview.h"
#include "client/scenes/InventoryWindow.h"
#include "client/scenes/SceneResources.h"
#include "client/ui/Ui.h"
#include "client/world/ItemData.h"
#include "client/world/TerrainQuery.h"
#include "shared/core/Log.h"
#include "shared/game/PathFind.h"
#include "shared/game/TerrainWalk.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;
using world::Vec3;

namespace {

constexpr float kChaseReplan = 0.25f;     // seconds between chase destination updates
constexpr float kCombatMessageTime = 2.6f; // seconds a combat message stays
constexpr float kLevelUpTime = 3.2f;
constexpr double kCombatWindow = 1.0;     // the server's 10/s window ...
constexpr std::size_t kCombatPerWindow = 8; // ... we stay below it

// Chase stop distance for a range: a little inside it (the target keeps moving).
float stopDistance(float range)
{
    return range > 5.0f ? range - 1.5f : std::max(1.2f, range - 0.6f);
}

// A horizontal bar with an inset background, a vertical-gradient fill and centred text.
void drawBar(render::SpriteBatch& batch, const render::Font& font, const Rect& r, float fraction, Color top,
             Color bottom, std::string_view text, float scale)
{
    const float radius = std::min(r.h * 0.5f, 4.0f * scale);
    batch.fillRoundedRect(r, radius, Color::hex(0x0A0D10, 0.88f));
    Rect fill = r;
    fill.w = std::round(r.w * std::clamp(fraction, 0.0f, 1.0f));
    if (fill.w >= 1.0f)
        batch.fillRoundedRectGradient(fill, std::min(radius, fill.w * 0.5f), top, bottom);
    batch.strokeRoundedRect(r, radius, std::max(1.0f, scale), Color::hex(0x6E5A33, 0.9f));
    if (!text.empty())
    {
        render::TextStyle st;
        st.color = Color::hex(0xF6F0DE);
        st.outline = true;
        st.outlineColor = Color::hex(0x000000, 0.8f);
        render::drawTextAligned(batch, font, text, r, render::HAlign::Center, render::VAlign::Middle, st);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// HUD widgets
// ---------------------------------------------------------------------------------------------

// Full-screen, mouse-transparent layer that draws the custom HUD (target frame, xp bar, cast bar,
// messages, level-up text) with the scene's state.
class CombatHudWidget final : public ui::Widget
{
public:
    explicit CombatHudWidget(WorldScene& scene) : m_scene(scene)
    {
        setMouseTransparent(true);
        setName("world.combathud");
    }
    void drawSelf(ui::DrawContext&) override { m_scene.drawCombatHud(); }

private:
    WorldScene& m_scene;
};

namespace {

// Full-screen dim layer of the death screen: consumes clicks (the world is locked), draws a dark
// red vignette; its children are the "Öldün" panel and the respawn button.
class DeathLayer final : public ui::Widget
{
public:
    DeathLayer() { setName("world.death"); }
    void drawSelf(ui::DrawContext& ctx) override
    {
        const Rect r = rect();
        ctx.batch.fillRect(r, Color::hex(0x080305, 0.5f));
        const float edge = std::round(r.h * 0.22f);
        ctx.batch.fillRectGradient({r.x, r.y, r.w, edge}, Color::hex(0x3A0606, 0.55f), Color::hex(0x3A0606, 0.0f));
        ctx.batch.fillRectGradient({r.x, r.bottom() - edge, r.w, edge}, Color::hex(0x3A0606, 0.0f),
                                   Color::hex(0x3A0606, 0.55f));
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void WorldScene::initCombat(const vn::EnterWorldRes& enter, std::uint8_t charClass)
{
    m_charClass = game::isValidClass(charClass) ? charClass : static_cast<std::uint8_t>(1);
    m_skill = game::classSkill(m_charClass);
    m_self = {};
    m_self.level = std::max<std::uint16_t>(enter.level, 1);
    m_self.xp = enter.xp;
    m_self.xpToNext = enter.xpToNext;
    m_self.hp = enter.hp;
    m_self.maxHp = std::max<std::uint32_t>(enter.maxHp, 1);
    m_self.mp = enter.mp;
    m_self.maxMp = enter.maxMp;
    m_self.attack = enter.attack;
    m_self.defense = enter.defense;
    m_self.dead = false; // applied in becomeReady() (the animator is bound there)
    m_target = 0;
    m_targetSent = true;
    m_autoAttack = false;
    m_chasing = false;
    m_queuedSkill = 0;
    m_attackOnArrive = false;
    m_skillTimers.reset();
    m_numbers.clear();
    m_bolts.clear();
    m_combatMessage.clear();
    m_combatMessageAge = 99.0f;
    m_levelUpAge = 99.0f;
    m_levelUpWatch = false;
    m_levelUpSawFull = false;
    m_seen = {};
    m_combatSends.fill(-1e9);
    VO_LOG_INFO("world: combat: level {} xp {}/{} hp {}/{} mp {}/{} attack {} defense {}{}; skill {}", m_self.level,
                m_self.xp, m_self.xpToNext, m_self.hp, m_self.maxHp, m_self.mp, m_self.maxMp, m_self.attack,
                m_self.defense, enter.isDead ? " (dead)" : "", m_skill != nullptr ? m_skill->name : "none");
}

void WorldScene::requestCombatAssets()
{
    render::AssetCache* assets = ctx().assets;
    const world::ItemData* data = ctx().resources.itemData();
    if (assets == nullptr)
        return;
    if (data != nullptr && data->monstersLoaded)
    {
        std::vector<std::string_view> models;
        for (const game::MonsterTemplate& t : data->monsters.templates())
            if (std::find(models.begin(), models.end(), t.model) == models.end())
                models.push_back(t.model);
        for (const std::string_view m : models)
            assets->requestMonster(m);
    }
    assets->request(render::AssetCache::Kind::Mesh, "effects/ember_bolt.vmesh");
    requestSkillAssets(); // M12: skill / status / bar icons, effect meshes
}

void WorldScene::buildCombatHud()
{
    ui::Widget& root = ui().root();
    m_combatHud = &root.add<CombatHudWidget>(*this);
    m_combatHud->setAnchors(anchor::kFill);

    // (M12: the skill bar replaced the single skill button; buildSkills() adds it.)

    // Safe zone indicator right under the zone name (top right).
    m_safeLabel = &root.add<ui::Label>("● Güvenli Bölge", ui::TextRole::Small);
    m_safeLabel->setColor(Color::hex(0x9FE08A)).setShadow(true).setAlign(render::HAlign::Right, render::VAlign::Middle);
    m_safeLabel->setAnchors(anchor::kTopRight).setMargins({0.0f, 42.0f, 19.0f, 0.0f}).setSize(220.0f, 20.0f);
    m_safeLabel->setName("world.safezone");
    m_safeLabel->setVisible(false);

    // M15: the hunting slot the player stands in (its area + 6 m), in the same place: "◆ Kül Çukuru 2 · Sv. 1-2".
    // Slots never reach a safe zone, so the two lines never show together.
    m_slotLabel = &root.add<ui::Label>("", ui::TextRole::Small);
    m_slotLabel->setColor(Color::hex(0xE8C27A)).setShadow(true).setAlign(render::HAlign::Right, render::VAlign::Middle);
    m_slotLabel->setAnchors(anchor::kTopRight).setMargins({0.0f, 42.0f, 19.0f, 0.0f}).setSize(260.0f, 20.0f);
    m_slotLabel->setName("world.slotname");
    m_slotLabel->setVisible(false);
}

const game::MonsterSlot* WorldScene::currentSlot() const
{
    const world::ItemData* data = ctx().resources.itemData();
    if (data == nullptr || !data->monstersLoaded)
        return nullptr;
    const Vec3 p = m_model.local().mover().position();
    return data->monsters.slotAt(m_model.zoneId() != 0 ? m_model.zoneId() : game::kStartZoneId, p.x, p.z,
                                 game::kSlotShowMargin);
}

static std::string slotLineText(const game::MonsterSlot& slot)
{
    return slot.minLevel == slot.maxLevel ? std::format("◆ {} · Sv. {}", slot.name, slot.minLevel)
                                          : std::format("◆ {} · Sv. {}-{}", slot.name, slot.minLevel, slot.maxLevel);
}

// ---------------------------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------------------------

bool WorldScene::combatBudget() const
{
    std::size_t recent = 0;
    for (const double t : m_combatSends)
        recent += (m_time - t) < kCombatWindow ? 1 : 0;
    return recent < kCombatPerWindow;
}

void WorldScene::noteCombatSend()
{
    m_combatSends[m_combatSendNext] = m_time;
    m_combatSendNext = (m_combatSendNext + 1) % m_combatSends.size();
}

float WorldScene::attackRange() const
{
    const game::ClassTable* t = game::classTable(m_charClass);
    return t != nullptr ? t->attackRange : 2.5f;
}

const game::MonsterTemplate* WorldScene::monsterTemplate(const world::RemoteEntity& e) const
{
    const world::ItemData* data = ctx().resources.itemData();
    if (!e.isMonster() || data == nullptr || !data->monstersLoaded)
        return nullptr;
    return data->monsters.find(e.templateId);
}

bool WorldScene::inSafeZone() const
{
    const world::ItemData* data = ctx().resources.itemData();
    if (data == nullptr || !data->monstersLoaded)
        return false;
    const Vec3 p = m_model.local().mover().position();
    return data->monsters.inSafeZone(m_model.zoneId() != 0 ? m_model.zoneId() : game::kStartZoneId, p.x, p.z);
}

void WorldScene::showCombatMessage(std::string_view text)
{
    m_combatMessage = std::string(text);
    m_combatMessageAge = 0.0f;
    VO_LOG_INFO("world: combat message: {}", text);
}

void WorldScene::setTarget(std::uint32_t id)
{
    if (id == m_target)
        return;
    if (const world::RemoteEntity* bag = id != 0 ? m_model.find(id) : nullptr; bag != nullptr && bag->isLoot())
        return; // loot bags are opened, never targeted (M10)
    if (m_autoAttack)
        stopAttack(true);
    m_chasing = false;
    m_queuedSkill = 0;
    m_attackOnArrive = false;
    m_target = id;
    m_targetSent = false;
    if (sendCombat(vn::TargetReq{id}))
        m_targetSent = true;
    const world::RemoteEntity* e = id != 0 ? m_model.find(id) : nullptr;
    VO_LOG_INFO("world: target {} {}", id, e != nullptr ? e->name : (id == m_model.localEntityId() ? m_name : ""));
}

void WorldScene::clearTarget()
{
    setTarget(0);
}

void WorldScene::cycleTarget()
{
    if (m_self.dead)
        return;
    const Vec3 me = m_model.local().mover().position();
    std::vector<world::TabCandidate> c;
    for (const world::RemoteEntity& e : m_model.entities())
        if (e.active && e.isMonster() && !e.dead())
            c.push_back({e.id, world::horizontalDistance(e.position, me)});
    const std::uint32_t next = world::nextTabTarget(c, m_target);
    if (next == 0)
    {
        showCombatMessage("Yakında hedef yok");
        return;
    }
    setTarget(next);
}

void WorldScene::stopAttack(bool sendStop)
{
    if (m_autoAttack && sendStop)
        (void)ctx().net.send(vn::AttackStopReq{}); // not counted by the server's combat window
    m_autoAttack = false;
    m_attackOnArrive = false;
    m_chasing = false;
}

void WorldScene::startAttack()
{
    if (m_self.dead)
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Attack, vn::ActionFailReason::YouAreDead));
        return;
    }
    world::RemoteEntity* t = m_target != 0 ? m_model.find(m_target) : nullptr;
    if (t == nullptr)
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Attack, vn::ActionFailReason::NoTarget));
        return;
    }
    if (!t->isMonster())
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Attack, vn::ActionFailReason::NotAttackable));
        return;
    }
    if (t->dead())
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Attack, vn::ActionFailReason::TargetDead));
        return;
    }
    if (m_localAnim.seated())
        m_localAnim.standUp(); // the server stands an attacker up
    const float d = world::horizontalDistance(t->position, m_model.local().mover().position());
    if (!m_autoAttack)
    {
        if (!sendCombat(vn::AttackReq{m_target}))
        {
            m_attackOnArrive = true; // over the request budget: send it from updateCombat()
        }
        else
        {
            m_autoAttack = true;
            VO_LOG_INFO("world: ATTACK_REQ {} '{}' at {:.1f} m", t->id, t->name, d);
        }
    }
    m_chasing = !game::inRange(d, stopDistance(attackRange()));
    m_chaseReplan = 0.0f;
}

void WorldScene::sendSkill(std::uint16_t skillId)
{
    const game::SkillDef* s = game::findSkill(skillId);
    if (s == nullptr)
        return;
    // M12 §7.1: enemy -> the target; friendly -> a selected living player, else 0 (ourselves); self -> 0.
    const world::SkillUse use =
        world::decideSkillUse(*s, std::max<std::uint8_t>(m_skillBook.rank(skillId), 1), skillSelection());
    const std::uint32_t target = s->target == game::SkillTarget::Enemy ? m_target : use.targetId;
    if (sendCombat(vn::SkillReq{skillId, target}))
        VO_LOG_INFO("world: SKILL_REQ {} ({}) on {}", skillId, s->name, target);
    else
        showCombatMessage("Çok hızlı: biraz bekle");
}

void WorldScene::useSkill()
{
    // M12: the bar's rules (WorldSkills.cpp useSkillById) for the class's free skill.
    if (m_skill != nullptr)
        useSkillById(m_skill->id);
}

void WorldScene::requestRespawn()
{
    if (!m_self.dead || m_self.respawnPending)
        return;
    if (ctx().net.send(vn::RespawnReq{}))
    {
        m_self.respawnPending = true;
        VO_LOG_INFO("world: RESPAWN_REQ {:.1f} s after death", m_time - m_self.diedAt);
    }
}

void WorldScene::requestStance(bool sit)
{
    if (m_self.dead)
        return;
    if (sendCombat(vn::StanceReq{sit ? vn::kStanceSit : vn::kStanceStand}))
        VO_LOG_INFO("world: STANCE_REQ {}", sit ? "sit" : "stand");
}

// ---------------------------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------------------------

world::Vec3 WorldScene::headPoint(const world::RemoteEntity& e) const
{
    render::AssetCache& cache = *ctx().assets;
    float height = 1.8f;
    if (e.isMonster())
    {
        const game::MonsterTemplate* t = monsterTemplate(e);
        const render::GpuMesh* mesh = t != nullptr ? cache.mesh(render::AssetCache::monsterMeshPath(t->model)) : nullptr;
        height = (mesh != nullptr && mesh->cpu ? mesh->cpu->boundsMax.y : 1.6f) * e.scale;
    }
    else
    {
        const render::GpuMesh* mesh = cache.mesh(cache.characterMeshPath(game::isValidRace(e.race) ? e.race : 1, e.face));
        height = mesh != nullptr && mesh->cpu ? mesh->cpu->boundsMax.y : 1.8f;
    }
    return e.position + Vec3{0.0f, height, 0.0f};
}

std::uint32_t WorldScene::pickAt(int x, int y) const
{
    world::Mat4 inv;
    if (!assets::invert(currentViewProj(), inv))
        return 0;
    const world::Ray ray = world::screenRay(inv, static_cast<float>(x), static_cast<float>(y), ctx().viewportWidth,
                                            ctx().viewportHeight);
    std::vector<world::PickCandidate> c;
    for (const world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active || (e.isMonster() && e.dead()))
            continue;
        if (e.isLoot())
        {
            c.push_back({e.id, e.position, 0.5f, 0.75f}); // M10: the bag on the ground (drawn 1.3x)
            continue;
        }
        const float height = headPoint(e).y - e.position.y;
        // A dead player lies on the ground: a low, wide capsule.
        if (e.dead())
            c.push_back({e.id, e.position, 0.7f, 0.6f});
        else
            c.push_back({e.id, e.position, 0.42f * std::max(e.scale, 1.0f), height});
    }
    const std::uint32_t id = world::pickEntity(ray, c, 200.0f);
    if (id == 0)
        return 0;
    // Terrain in front of it (a hill between camera and monster) wins.
    Vec3 hit{};
    if (m_terrain != nullptr && world::raycastTerrain(*m_terrain, ray, 200.0f, hit))
    {
        const world::RemoteEntity* e = const_cast<world::WorldModel&>(m_model).find(id);
        if (e != nullptr &&
            assets::length(hit - ray.origin) + 1.0f < assets::length(e->position - ray.origin))
            return 0;
    }
    return id;
}

// ---------------------------------------------------------------------------------------------
// Animators
// ---------------------------------------------------------------------------------------------

void WorldScene::bindMonsterAnimator(world::RemoteEntity& e)
{
    const game::MonsterTemplate* t = monsterTemplate(e);
    if (t == nullptr)
        return;
    render::AssetCache& cache = *ctx().assets;
    const render::GpuMesh* mesh = cache.mesh(render::AssetCache::monsterMeshPath(t->model));
    const world::CharacterClips* clips = cache.monsterClips(t->model);
    if (mesh == nullptr || !mesh->cpu || clips == nullptr || (e.animatorMesh == mesh && e.animatorClips == clips))
        return;
    e.animator.bind(mesh->cpu.get(), clips);
    e.animator.setMoveScale(t->scale);
    e.scale = t->scale;
    e.animatorMesh = mesh;
    e.animatorClips = clips;
    if (e.dead())
        e.animator.die(true); // a corpse that spawned dead
}

void WorldScene::onLocalDeath()
{
    if (m_self.dead)
        return;
    m_self.dead = true;
    m_self.hp = 0;
    m_self.diedAt = m_time;
    m_self.respawnPending = false;
    m_self.respawnNotBefore = 0.0;
    stopAttack(false); // the server ends it
    m_queuedSkill = 0;
    m_skillTimers.cancelCast();
    m_route.clear();
    m_routeNext = 0;
    m_pendingDestination.reset();
    m_model.local().mover().clearDestination();
    m_localAnim.die();
    stopDrag();
    m_lootWalk = 0;
    closeLootWindow(false, {}); // the server ends the view (LOOT_CLOSED YouDied) by itself
    if (m_invWindow != nullptr)
        m_invWindow->setLocked(true);
    if (m_deathLayer != nullptr)
        m_deathLayer->setVisible(true);
    VO_LOG_INFO("world: the local character died");
}

void WorldScene::onLocalRevive()
{
    if (!m_self.dead)
        return;
    m_self.dead = false;
    m_self.respawnPending = false;
    m_localAnim.revive();
    if (m_invWindow != nullptr)
        m_invWindow->setLocked(false);
    if (m_deathLayer != nullptr)
        m_deathLayer->setVisible(false);
    ++m_seen.respawns;
    VO_LOG_INFO("world: the local character is alive again");
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

bool WorldScene::handleCombatMessage(const net::NetEvent& e)
{
    const auto op = static_cast<vn::Opcode>(e.opcode);
    switch (op)
    {
    case vn::Opcode::ActionFail:
    case vn::Opcode::CombatEvent:
    case vn::Opcode::CastStart:
    case vn::Opcode::CastEnd:
    case vn::Opcode::EntityStats:
    case vn::Opcode::SelfStats:
    case vn::Opcode::XpGain:
    case vn::Opcode::LevelUp:
    case vn::Opcode::EntityDeath:
    case vn::Opcode::RespawnRes:
    case vn::Opcode::EntityState:
        break;
    default:
        return false;
    }
    const std::uint32_t me = m_model.localEntityId();
    const auto malformed = [&]() {
        VO_LOG_ERROR("world: malformed {} payload ({} bytes)", vn::opcodeName(op), e.payload.size());
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return true;
    };
    const game::ItemCatalog* catalog =
        ctx().resources.itemData() != nullptr && ctx().resources.itemData()->catalogLoaded
            ? &ctx().resources.itemData()->catalog
            : nullptr;
    const auto weaponOf = [&](std::uint16_t templateId) -> const game::ItemTemplate* {
        return catalog != nullptr && templateId != 0 ? catalog->find(templateId) : nullptr;
    };

    switch (op)
    {
    case vn::Opcode::CombatEvent:
    {
        vn::CombatEvent m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        world::RemoteEntity* src = m.sourceId != me ? m_model.find(m.sourceId) : nullptr;
        world::RemoteEntity* tgt = m.targetId != me ? m_model.find(m.targetId) : nullptr;
        const bool heal = m.kind == vn::kCombatKindHeal;
        const bool damage = m.kind == vn::kCombatKindDamage;
        // The target: hp, number, hit reaction.
        Vec3 numberAt{};
        bool haveNumberAt = false;
        const bool miss = m.kind == vn::kCombatKindMiss; // M11: a dodged monster attack
        if (miss)
        {
            if (m.targetId == me)
            {
                ++m_statsSeen.dodges;
                showCombatMessage("Kaçındın!");
            }
            else if (tgt != nullptr && !tgt->isMonster())
                ++m_statsSeen.missesSeen;
            VO_LOG_INFO("world: COMBAT_EVENT miss: {} dodged {}'s attack", m.targetId == me ? std::string("we")
                                                                           : tgt != nullptr ? tgt->name : std::to_string(m.targetId),
                        src != nullptr ? src->name : std::to_string(m.sourceId));
        }
        if (m.targetId == me)
        {
            if (m_levelUpWatch && damage && m.targetHp + m.amount >= m_self.maxHp)
                m_levelUpSawFull = true; // the hit found a full bar
            m_self.hp = m.targetHp;
            numberAt = m_model.local().visualPosition() + Vec3{0.0f, 2.0f, 0.0f};
            haveNumberAt = true;
            if (damage)
            {
                ++m_seen.hitsTaken;
                m_hitFlash = 1.0f;
                m_lastAttacker = m.sourceId;
                m_lastAttackedAt = m_time;
                (void)m_localAnim.playHit();
            }
            if (heal)
                ++m_seen.healed;
        }
        else if (tgt != nullptr)
        {
            tgt->hp = m.targetHp;
            if (!tgt->isMonster())
                tgt->hpShownUntil = tgt->age + 8.0f;
            numberAt = headPoint(*tgt) + Vec3{0.0f, 0.35f, 0.0f};
            haveNumberAt = true;
            if (damage && !tgt->casting)
                (void)tgt->animator.playHit();
            if (!tgt->isMonster() && damage)
                ++m_seen.remoteDamageSeen;
            if (!tgt->isMonster() && heal)
                ++m_seen.remoteHealsSeen;
        }
        const bool periodic = (m.flags & vn::kCombatFlagPeriodic) != 0;
        // M12: a periodic tick shows a smaller number, a fully absorbed hit a grey "Emildi".
        // (a heal that found a full bar, e.g. a heal over time tick, shows nothing: no "+0")
        if (haveNumberAt && (damage || (heal && m.amount > 0) || miss)) // a miss: grey "Iska" over the dodger
            m_numbers.add(world::classifyCombatNumber(m.kind, m.flags, m.targetId == me, m.amount), m.amount, numberAt);
        if (periodic)
            ++m_skillsSeen.ticks;
        if ((m.flags & vn::kCombatFlagAbsorbed) != 0 && m.targetId == me)
            ++m_skillsSeen.absorbed;
        // M12: a damage tick of a burn / poison: a wisp rising from the target's chest.
        if (periodic && damage)
        {
            if (const std::string_view wisp = world::tickArt(m.skillId); !wisp.empty() && m_art.size() < 64)
            {
                SkillArt a;
                a.mesh = std::format("effects/{}.vmesh", wisp);
                a.motion = world::ArtMotion::Wisp;
                a.entity = m.targetId;
                a.duration = 0.6f;
                a.seed = static_cast<float>((m_artSeed++ * 2654435761u) % 1000u) / 1000.0f;
                m_art.push_back(std::move(a));
            }
        }
        // The source: swing / punch animation, timers.
        // v6 (PROTOCOL_SKILLS.md §3.8): a damage / heal over time tick is not an action of its source.
        // M12: a skill's swing / release starts with its SKILL_USED (WorldSkills.cpp); only basic
        // attacks animate here.
        const game::SkillDef* skill = m.skillId != 0 && !periodic ? game::findSkill(m.skillId) : nullptr;
        const bool basic = !periodic && m.skillId == 0;
        if (periodic)
        {
            // only the number above
        }
        else if (m.sourceId == me)
        {
            m_skillTimers.onAction(m_time);
            if (skill != nullptr && skill->castMs == 0)
                m_skillTimers.onSkillUsed(skill->id, m_time);
            if (damage)
                ++m_seen.ownHits;
            if (skill != nullptr && damage)
                ++m_seen.ownSkillHits;
            if (heal)
                ++m_seen.heals;
            // M12: how many targets the last own skill use hit (an area skill hits several).
            if (skill != nullptr && skill->id == m_lastUsedSkill && (damage || heal) && m_time - m_lastUseTime < 1.0)
                ++m_lastUseHits;
            if (basic)
            {
                const game::ItemTemplate* weapon = weaponOf(localEquipment().mainHand);
                // The mage's staff "shot" is its basic attack; its skill shot comes from CAST_END.
                (void)m_localAnim.playAttack(world::attackClipFor(weapon, false, m_localSwings++));
            }
        }
        else if (src != nullptr)
        {
            src->facing = m.targetId;
            src->facingUntil = src->age + 1.5f;
            if (basic && !src->casting)
            {
                const game::ItemTemplate* weapon = src->isMonster() ? nullptr : weaponOf(src->mainHand);
                if (src->animator.playAttack(world::attackClipFor(weapon, src->isMonster(), src->swings++)))
                    ++m_seen.remoteAttackClips;
            }
        }
        if (m.sourceId != me && m.targetId != me && damage)
            ++m_seen.otherHits;
        // (M12: Köz Oku's bolt, Kırağı Oku's frost bolt and the other effect art start with SKILL_USED.)
        return true;
    }
    case vn::Opcode::CastStart:
    {
        vn::CastStart m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        if (m.casterId == me)
        {
            m_skillTimers.onCastStart(m.skillId, m.castMs, m_time);
            (void)m_localAnim.beginCast();
        }
        else if (world::RemoteEntity* r = m_model.find(m.casterId))
        {
            r->casting = true;
            r->facing = m.targetId;
            r->facingUntil = r->age + static_cast<float>(m.castMs) / 1000.0f + 1.0f;
            (void)r->animator.beginCast();
        }
        return true;
    }
    case vn::Opcode::CastEnd:
    {
        vn::CastEnd m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        const bool completed = m.result == vn::kCastCompleted;
        if (m.casterId == me)
        {
            m_skillTimers.onCastEnd(m.skillId, completed, m_time);
            m_localAnim.endCast(completed);
            if (!completed)
                showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::Interrupted));
        }
        else if (world::RemoteEntity* r = m_model.find(m.casterId))
        {
            r->casting = false;
            r->animator.endCast(completed);
            if (completed)
                ++m_seen.remoteAttackClips;
        }
        return true;
    }
    case vn::Opcode::EntityStats:
    {
        vn::EntityStats m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        if (m.entityId == me)
        {
            m_self.hp = m.hp;
            m_self.maxHp = std::max<std::uint32_t>(m.maxHp, 1);
        }
        else if (world::RemoteEntity* r = m_model.find(m.entityId))
        {
            if (!r->isMonster() && m.hp < r->hp)
                r->hpShownUntil = r->age + 8.0f;
            r->hp = m.hp;
            r->maxHp = m.maxHp;
            r->level = m.level;
        }
        return true;
    }
    case vn::Opcode::SelfStats:
    {
        vn::SelfStats m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_self.hp = m.hp;
        m_self.maxHp = std::max<std::uint32_t>(m.maxHp, 1);
        m_self.mp = m.mp;
        m_self.maxMp = m.maxMp;
        m_self.attack = m.attack;
        m_self.defense = m.defense;
        if (m_levelUpWatch && m.hp >= m.maxHp)
            m_levelUpSawFull = true; // automation `levelupfull`: the level-up refilled the bar
        return true;
    }
    case vn::Opcode::XpGain:
    {
        vn::XpGain m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_self.xp = m.xp;
        m_self.xpToNext = m.xpToNext;
        if (m.amount > 0)
            showCombatMessage(std::format("+{} deneyim", m.amount));
        if (m_kunyeOpen)
            refreshKunye(); // its experience line
        VO_LOG_INFO("world: XP_GAIN +{} -> {}/{} (from {})", m.amount, m.xp, m.xpToNext, m.sourceEntityId);
        return true;
    }
    case vn::Opcode::LevelUp:
    {
        vn::LevelUp m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        if (m.entityId == me)
        {
            // M11: 3 stat points per level gained (the toast names them; CHAR_STATS follows).
            m_levelUpPoints = m.level > m_self.level ? game::kPointsPerLevel * static_cast<std::uint32_t>(m.level - m_self.level)
                                                     : 0u;
            // M12: and skill points (the new SKILL_STATE follows after this batch).
            m_levelUpSkillPoints = m.level > m_self.level
                                       ? game::skills().rules().pointsPerLevel * static_cast<std::uint32_t>(m.level - m_self.level)
                                       : 0u;
            m_skillBook.setLevel(m.level);
            m_self.level = m.level;
            m_levelUpLevel = m.level;
            m_levelUpAge = 0.0f;
            ++m_seen.levelUps;
            // Automation `levelupfull`: was the bar full right after the level-up (its SELF_STATS,
            // or the hp a hit found when another monster struck before that)?
            m_levelUpWatch = true;
            m_levelUpSawFull = false;
            const auto& entry = ctx().net.account().world;
            if (m_levelLabel != nullptr && entry)
                m_levelLabel->setText(describeCharacter(entry->character.race, entry->character.charClass, 0));
            refreshItemRules(); // the level rule (and the base attributes, M11)
            refreshKunye();
            VO_LOG_INFO("world: LEVEL_UP -> {} (+{} stat points)", m.level, m_levelUpPoints);
        }
        else if (world::RemoteEntity* r = m_model.find(m.entityId))
        {
            r->level = m.level;
        }
        return true;
    }
    case vn::Opcode::EntityDeath:
    {
        vn::EntityDeath m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        if (m.entityId == me)
        {
            onLocalDeath();
            return true;
        }
        if (world::RemoteEntity* r = m_model.find(m.entityId))
        {
            r->state |= vn::kEntityStateDead;
            r->hp = 0;
            r->casting = false;
            r->animator.die();
            if (m.killerId == me && r->isMonster())
                ++m_seen.kills;
            else
                ++m_seen.otherDeaths;
            VO_LOG_INFO("world: ENTITY_DEATH {} '{}' (killer {})", r->id, r->name, m.killerId);
        }
        if (m.entityId == m_target)
        {
            // The server ends an auto-attack on its target's death.
            m_autoAttack = false;
            m_attackOnArrive = false;
            m_chasing = false;
            m_queuedSkill = 0;
        }
        return true;
    }
    case vn::Opcode::EntityState:
    {
        vn::EntityState m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        if (m.entityId == me)
        {
            const std::uint8_t old = m_self.state;
            m_self.state = m.state;
            const bool sit = (m.state & vn::kEntityStateSitting) != 0;
            if (sit != ((old & vn::kEntityStateSitting) != 0))
                m_localAnim.setSeated(sit, false); // edge-triggered: a local stand-up is not undone
            if ((m.state & vn::kEntityStateDead) != 0)
                onLocalDeath();
            else if ((old & vn::kEntityStateDead) != 0 || m_self.dead)
                onLocalRevive();
            return true;
        }
        if (world::RemoteEntity* r = m_model.find(m.entityId))
        {
            const std::uint8_t old = r->state;
            r->state = m.state;
            if ((m.state & vn::kEntityStateDead) != 0 && (old & vn::kEntityStateDead) == 0)
                r->animator.die();
            else if ((m.state & vn::kEntityStateDead) == 0 && (old & vn::kEntityStateDead) != 0)
                r->animator.revive();
            const bool sit = (m.state & vn::kEntityStateSitting) != 0;
            if (sit != ((old & vn::kEntityStateSitting) != 0))
                r->animator.setSeated(sit, false);
        }
        return true;
    }
    case vn::Opcode::RespawnRes:
    {
        vn::RespawnRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_self.respawnPending = false;
        if (m.result == vn::RespawnResult::TooEarly)
        {
            m_self.respawnNotBefore = m_time + 1.0;
            showCombatMessage("Henüz yeniden doğamazsın");
            VO_LOG_INFO("world: RESPAWN_RES TooEarly");
            return true;
        }
        if (m.result == vn::RespawnResult::NotDead)
        {
            VO_LOG_INFO("world: RESPAWN_RES NotDead");
            onLocalRevive();
            return true;
        }
        m_model.local().teleport({m.x, m.y, m.z}, m.yaw, m_terrain);
        m_camera.reset(m_model.local().mover().position() + Vec3{0.0f, 1.55f, 0.0f}, m.yaw);
        m_self.hp = m.hp;
        m_self.mp = m.mp;
        onLocalRevive();
        VO_LOG_INFO("world: RESPAWN_RES ok at ({:.1f}, {:.1f}) with {} hp, {} mp", m.x, m.z, m.hp, m.mp);
        return true;
    }
    case vn::Opcode::ActionFail:
    {
        vn::ActionFail m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_lastCombatFail = std::string(vn::actionFailReasonName(m.reason));
        VO_LOG_INFO("world: ACTION_FAIL action {} skill {}: {}", static_cast<int>(m.action), m.skillId,
                    vn::actionFailReasonName(m.reason));
        const bool attack = m.action == vn::CombatAction::Attack;
        if (attack && m.reason == vn::ActionFailReason::OutOfRange && m_chasing)
            return true; // we are walking there already
        if (attack && (m.reason == vn::ActionFailReason::NotAttackable || m.reason == vn::ActionFailReason::TargetDead ||
                       m.reason == vn::ActionFailReason::NoTarget || m.reason == vn::ActionFailReason::YouAreDead))
            stopAttack(false);
        if (m.action == vn::CombatAction::Skill)
            m_queuedSkill = 0;
        showCombatMessage(world::actionFailText(m.action, m.reason));
        return true;
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------

void WorldScene::updateCombat(float dt)
{
    m_combatMessageAge += dt;
    m_levelUpAge += dt;
    if (m_levelUpWatch && m_levelUpAge > 2.0f)
        m_levelUpWatch = false; // levelupfull only looks at the first 2 s (no regeneration)
    m_hitFlash = std::max(0.0f, m_hitFlash - dt * 2.5f);
    m_numbers.update(dt);
    for (Bolt& b : m_bolts)
        b.age += dt;
    std::erase_if(m_bolts, [](const Bolt& b) { return b.age >= b.duration; });

    // Monsters: bound once their model is loaded; remote facing while fighting.
    for (world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active)
            continue;
        if (e.isMonster())
            bindMonsterAnimator(e);
        if (!e.isMonster() && e.facing != 0 && e.age < e.facingUntil && !e.moving)
        {
            Vec3 at{};
            bool known = true;
            if (e.facing == m_model.localEntityId())
                at = m_model.local().mover().position();
            else if (const world::RemoteEntity* f = m_model.find(e.facing))
                at = f->position;
            else
                known = false;
            if (known && world::horizontalDistance(at, e.position) > 0.3f)
                e.yaw = std::atan2(at.x - e.position.x, at.z - e.position.z);
        }
    }

    // The target vanished (despawn, leave): forget it.
    if (m_target != 0 && m_target != m_model.localEntityId() && m_model.find(m_target) == nullptr)
    {
        VO_LOG_INFO("world: target {} gone", m_target);
        m_target = 0;
        m_targetSent = false;
        m_autoAttack = false;
        m_attackOnArrive = false;
        m_chasing = false;
        m_queuedSkill = 0;
    }
    if (!m_targetSent && sendCombat(vn::TargetReq{m_target}))
        m_targetSent = true;

    world::LocalMover& mover = m_model.local().mover();
    world::RemoteEntity* t = m_target != 0 ? m_model.find(m_target) : nullptr;
    if (m_self.dead || t == nullptr)
    {
        m_chasing = false;
        m_queuedSkill = 0;
    }
    if (t != nullptr && !m_self.dead)
    {
        const float d = world::horizontalDistance(t->position, mover.position());
        if (m_attackOnArrive && !m_autoAttack && t->isMonster() && !t->dead() && sendCombat(vn::AttackReq{m_target}))
        {
            m_autoAttack = true;
            m_attackOnArrive = false;
        }
        if (m_chasing)
        {
            const game::SkillDef* queued = m_queuedSkill != 0 ? game::findSkill(m_queuedSkill) : nullptr;
            // M12: the range of the rank we have (Gölge Adımı reaches further at higher ranks).
            const float range = queued != nullptr ? queued->at(std::max<std::uint8_t>(m_skillBook.rank(queued->id), 1)).range
                                                  : attackRange();
            if (d <= stopDistance(range))
            {
                m_chasing = false;
                m_route.clear();
                m_routeNext = 0;
                mover.clearDestination();
                if (queued != nullptr)
                {
                    m_queuedSkill = 0;
                    sendSkill(queued->id);
                }
            }
            else
            {
                m_chaseReplan -= dt;
                if (m_chaseReplan <= 0.0f && !m_localAnim.seated())
                {
                    m_chaseReplan = kChaseReplan;
                    const Vec3 p = mover.position();
                    if (m_terrain == nullptr || game::isSegmentWalkable(*m_terrain, p.x, p.z, t->position.x, t->position.z))
                    {
                        m_route.clear();
                        m_routeNext = 0;
                        mover.setDestination(t->position.x, t->position.z);
                    }
                    else if (!routePending())
                    {
                        std::string ignored;
                        m_chaseReplan = 1.0f; // grid paths are dearer
                        if (!setRouteNear(p, t->position.x, t->position.z, 3.0f, ignored))
                            mover.setDestination(t->position.x, t->position.z);
                    }
                }
            }
        }
        // Standing in range while fighting: turn towards the target (cosmetic; the server checks range only).
        if (m_autoAttack && !m_chasing && !mover.moving() && !mover.destination() && !routePending() && d > 0.3f &&
            !m_localAnim.seated())
        {
            const float want = std::atan2(t->position.x - mover.position().x, t->position.z - mover.position().z);
            const float delta = world::angleDelta(mover.yaw(), want);
            if (std::fabs(delta) > 0.02f)
            {
                const float step = std::clamp(delta, -10.0f * dt, 10.0f * dt);
                mover.setPosition(mover.position(), mover.yaw() + step, m_terrain);
            }
        }
    }

    if (m_huntLevel != 0)
        updateHunt();

    // HUD state.
    if (m_safeLabel != nullptr)
        m_safeLabel->setVisible(m_phase == Phase::Ready && inSafeZone() && !m_yield.zone);
    if (m_slotLabel != nullptr)
    {
        const game::MonsterSlot* slot = m_phase == Phase::Ready && !m_yield.zone ? currentSlot() : nullptr;
        if (slot != m_shownSlot)
        {
            m_shownSlot = slot;
            if (slot != nullptr)
                m_slotLabel->setText(slotLineText(*slot));
        }
        m_slotLabel->setVisible(slot != nullptr);
    }
    if (m_deathLayer != nullptr && m_self.dead)
    {
        const double left = static_cast<double>(game::kRespawnMinDelay.count()) / 1000.0 - (m_time - m_self.diedAt);
        const bool can = left <= 0.0 && !m_self.respawnPending && m_time >= m_self.respawnNotBefore;
        if (m_deathCountdown != nullptr)
        {
            if (left > 0.0)
                m_deathCountdown->setText(std::format("{} saniye sonra yeniden doğabilirsin", static_cast<int>(std::ceil(left))));
            else
                m_deathCountdown->setText("Ocak ateşi seni bekliyor.");
        }
        if (m_respawnButton != nullptr)
            m_respawnButton->setEnabled(can);
    }
}

void WorldScene::updateHunt()
{
    if (m_self.dead)
    {
        // `hunt` and M10 `lootfarm` keep going after a death (a level-1 character can lose to an
        // unlucky pull; what the flows check is the level-up / the pickups, not the fight): respawn
        // as soon as the server allows it, then hunt on.
        const double since = m_time - m_self.diedAt;
        if (since * 1000.0 > static_cast<double>(game::kRespawnMinDelay.count()) + 500.0 && !m_self.respawnPending &&
            m_time >= m_self.respawnNotBefore && m_time >= m_huntNextTry)
        {
            VO_LOG_INFO("world: {}: died, respawning", m_farming ? "loot farm" : "hunt");
            requestRespawn();
            m_huntNextTry = m_time + 2.0;
        }
        return;
    }
    if (m_self.level >= m_huntLevel)
    {
        VO_LOG_INFO("world: hunt done: level {}", m_self.level);
        m_huntLevel = 0;
        return;
    }
    // M10 `lootfarm`: done once enough pickups of the template happened (and no bag is open).
    if (m_farming && m_farmRows == 0 && m_farmSeen >= m_farmWant && !m_loot.isOpen() && m_loot.takesInFlight() == 0)
    {
        VO_LOG_INFO("world: loot farm done: {} pickup(s) of template {}", m_farmSeen, m_farmTemplate);
        m_farmTemplate = 0;
        m_farming = false;
        m_farmDone = true;
        m_huntLevel = 0;
        return;
    }
    if (m_time < m_huntNextTry)
        return;
    const world::RemoteEntity* t = m_target != 0 ? m_model.find(m_target) : nullptr;
    const bool fighting = t != nullptr && t->isMonster() && !t->dead() && (m_autoAttack || m_attackOnArrive);
    if (fighting)
    {
        // Ağır Darbe (or the class skill) whenever it is ready and in range.
        if (m_skill != nullptr && m_skill->target == game::SkillTarget::Enemy && m_skillTimers.ready(m_skill->id, m_time) &&
            m_self.mp >= m_skill->mpCost && !m_chasing &&
            game::inRange(world::horizontalDistance(t->position, m_model.local().mover().position()), m_skill->range))
        {
            useSkill();
            m_huntNextTry = m_time + 0.8;
        }
        return;
    }
    // Something hits us: fight back first.
    if (m_time - m_lastAttackedAt < 3.0)
        if (const world::RemoteEntity* a = m_model.find(m_lastAttacker); a != nullptr && a->isMonster() && !a->dead())
        {
            if (m_localAnim.seated())
                m_localAnim.standUp();
            setTarget(a->id);
            startAttack();
            m_huntNextTry = m_time + 0.5;
            return;
        }
    // M10 `lootfarm`: loot every own / free bag nearby before resting or the next fight.
    if (m_farming && farmLootStep())
        return;
    const bool sitting = (m_self.state & vn::kEntityStateSitting) != 0;
    // Rest before the next pull below 75 % (a second pull from 60 % could kill a level-1 character).
    const bool low = m_self.hp * 100u < 75u * m_self.maxHp;
    const bool topUp = sitting && m_self.hp * 100u < 95u * m_self.maxHp;
    if (low || topUp)
    {
        // Rest: sit down once out of combat (the server refuses while in combat).
        if (!sitting && (m_self.state & vn::kEntityStateInCombat) == 0 && !m_model.local().mover().moving() &&
            !m_model.local().mover().destination() && !routePending())
            requestStance(true);
        m_huntNextTry = m_time + 1.0;
        return;
    }
    if (sitting || m_localAnim.seated())
    {
        toggleSit(); // stand up
        m_huntNextTry = m_time + 1.0;
        return;
    }
    // The nearest living monster of that name, preferring one without another living monster
    // within 9 m (a safer pull: one at a time).
    const Vec3 me = m_model.local().mover().position();
    const world::RemoteEntity* best = nullptr;
    float bestD = 1e9f;
    for (const world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active || !e.isMonster() || e.dead() || e.name != m_huntName)
            continue;
        bool crowded = false;
        for (const world::RemoteEntity& o : m_model.entities())
            if (&o != &e && o.active && o.isMonster() && !o.dead() &&
                world::horizontalDistance(o.position, e.position) < 9.0f)
                crowded = true;
        const float d = world::horizontalDistance(e.position, me) + (crowded ? 1000.0f : 0.0f);
        if (d < bestD)
        {
            bestD = d;
            best = &e;
        }
    }
    if (best == nullptr)
    {
        m_huntNextTry = m_time + 1.0;
        return;
    }
    setTarget(best->id);
    startAttack();
    m_huntNextTry = m_time + 0.5;
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

void WorldScene::addCombatDraws()
{
    render::WorldRenderer& r = *ctx().worldRenderer;
    // Selection ring under the target.
    if (m_target != 0 && m_terrain != nullptr)
    {
        Vec3 at{};
        float radius = 0.7f;
        bool hostile = false;
        bool known = true;
        if (m_target == m_model.localEntityId())
            at = m_model.local().visualPosition();
        else if (const world::RemoteEntity* e = m_model.find(m_target))
        {
            at = e->position;
            radius = 0.7f * std::max(e->scale, 0.8f);
            hostile = e->isMonster();
        }
        else
            known = false;
        if (known)
        {
            render::DecalDraw ring;
            ring.center = {at.x, m_terrain->heightAt(at.x, at.z), at.z};
            ring.normal = m_terrain->normalAt(at.x, at.z);
            ring.radius = radius;
            ring.color = hostile ? std::array<float, 4>{0.95f, 0.28f, 0.16f, 0.95f}
                                 : std::array<float, 4>{0.95f, 0.78f, 0.3f, 0.9f};
            ring.ring = true;
            ring.phase = 0.0f;
            r.addDecal(ring);
        }
    }
    // Level-up: a golden ring widening around the character.
    if (m_levelUpAge < 1.6f && m_terrain != nullptr)
    {
        const Vec3 p = m_model.local().visualPosition();
        const float u = m_levelUpAge / 1.6f;
        render::DecalDraw ring;
        ring.center = {p.x, m_terrain->heightAt(p.x, p.z), p.z};
        ring.normal = m_terrain->normalAt(p.x, p.z);
        ring.radius = 0.6f + 2.2f * u;
        ring.color = {1.0f, 0.82f, 0.32f, 0.95f * (1.0f - u)};
        ring.ring = true;
        ring.phase = std::fmod(u * 2.0f, 1.0f);
        r.addDecal(ring);
    }
    // Köz Oku's ember bolts and (M12) Kırağı Oku's frost bolts: from the hand to the target's chest.
    for (const Bolt& b : m_bolts)
    {
        const render::GpuMesh* bolt = ctx().assets->mesh(std::format("effects/{}.vmesh", b.mesh));
        if (bolt == nullptr)
            continue;
        Vec3 to = b.to;
        if (b.target == m_model.localEntityId())
            to = localDrawPosition() + Vec3{0.0f, 1.1f, 0.0f};
        else if (const world::RemoteEntity* e = m_model.find(b.target))
            to = e->position + Vec3{0.0f, 1.0f * e->scale, 0.0f};
        const float u = std::clamp(b.age / b.duration, 0.0f, 1.0f);
        const Vec3 pos = assets::lerp(b.from, to, u) + Vec3{0.0f, 0.25f * 4.0f * u * (1.0f - u), 0.0f};
        Vec3 z = to - b.from;
        if (assets::length(z) < 1e-3f)
            continue;
        z = assets::normalize(z);
        Vec3 x = assets::cross(Vec3{0.0f, 1.0f, 0.0f}, z);
        if (assets::length(x) < 1e-3f)
            x = {1.0f, 0.0f, 0.0f};
        x = assets::normalize(x);
        Vec3 y = assets::cross(z, x);
        if (b.mesh == "frost_bolt")
        {
            // ~2 turns a second about the flight axis (docs/SKILL_ART.md).
            const float roll = b.age * 2.0f * world::kTwoPi;
            const Vec3 rx = x * std::cos(roll) + y * std::sin(roll);
            y = assets::cross(z, rx);
            x = rx;
        }
        world::Mat4 w = world::Mat4::identity();
        const float k = b.scale; // the 0.5 m models read better a little larger
        w.at(0, 0) = x.x * k;
        w.at(1, 0) = x.y * k;
        w.at(2, 0) = x.z * k;
        w.at(0, 1) = y.x * k;
        w.at(1, 1) = y.y * k;
        w.at(2, 1) = y.z * k;
        w.at(0, 2) = z.x * k;
        w.at(1, 2) = z.y * k;
        w.at(2, 2) = z.z * k;
        w.at(0, 3) = pos.x;
        w.at(1, 3) = pos.y;
        w.at(2, 3) = pos.z;
        r.addEffectMesh(bolt, w);
    }
}
void WorldScene::drawCombatOverlays(const world::Mat4& vp)
{
    auto& batch = ctx().batch;
    const float s = ui().scale();
    const float w = ctx().viewportWidth;
    const float h = ctx().viewportHeight;
    const Vec3 eye = m_camera.eye();
    // Hp bars over monsters (damaged or targeted) and damaged / targeted players, above their name.
    for (const world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active || e.maxHp == 0)
            continue;
        const bool targeted = e.id == m_target;
        const bool show = e.isMonster() ? (!e.dead() && (e.hp < e.maxHp || targeted))
                                        : (!e.dead() && (targeted || e.age < e.hpShownUntil));
        if (!show)
            continue;
        const Vec3 head = headPoint(e) + Vec3{0.0f, 0.16f, 0.0f};
        if (assets::length(head - eye) > 45.0f)
            continue;
        world::Vec2 p{};
        if (!world::projectToScreen(vp, head, w, h, p))
            continue;
        const render::Font& nameFont = ctx().fonts.get(render::FontWeight::Bold, 15.0f * s);
        const float bw = std::round(64.0f * s);
        const float bh = std::max(3.0f, std::round(6.0f * s));
        const Rect bar{std::round(p.x - bw * 0.5f), std::round(p.y - nameFont.lineHeight() - bh - 3.0f * s), bw, bh};
        const float f = static_cast<float>(e.hp) / static_cast<float>(e.maxHp);
        batch.fillRect({bar.x - 1, bar.y - 1, bar.w + 2, bar.h + 2}, Color::hex(0x000000, 0.7f));
        batch.fillRect({bar.x, bar.y, std::round(bar.w * f), bar.h},
                       e.isMonster() ? Color::hex(0xD23A2A) : Color::hex(0x4FBF4A));
    }
    // Köz Oku: a soft ember glow around the bolt (the mesh alone is small at range).
    for (const Bolt& b : m_bolts)
    {
        Vec3 to = b.to;
        if (const world::RemoteEntity* e = m_model.find(b.target))
            to = e->position + Vec3{0.0f, 1.0f * e->scale, 0.0f};
        const float u = std::clamp(b.age / b.duration, 0.0f, 1.0f);
        const Vec3 pos = assets::lerp(b.from, to, u) + Vec3{0.0f, 0.25f * 4.0f * u * (1.0f - u), 0.0f};
        world::Vec2 p{};

        if (!world::projectToScreen(vp, pos, w, h, p))
            continue;
        const float dist = std::max(assets::length(pos - eye), 1.0f);
        const float r0 = std::clamp(260.0f / dist, 6.0f, 40.0f) * s;
        const bool frost = b.mesh == "frost_bolt";
        for (int i = 4; i >= 1; --i)
        {
            const float r = r0 * static_cast<float>(i) / 4.0f;
            const std::uint32_t rgb = frost ? (i == 1 ? 0xE8F6FF : 0x6FB8F0) : (i == 1 ? 0xFFE08A : 0xFF7A1E);
            batch.fillRoundedRect({p.x - r, p.y - r, 2.0f * r, 2.0f * r}, r,
                                  Color::hex(rgb, 0.10f + 0.12f * static_cast<float>(4 - i)));
        }
    }
    // Floating numbers.
    for (const world::FloatingNumber& n : m_numbers.items())
    {
        const world::FloatStyle st = world::floatStyle(n.kind);
        const Vec3 at = n.anchor + Vec3{0.0f, world::FloatingNumbers::rise(n), 0.0f};
        world::Vec2 p{};
        if (!world::projectToScreen(vp, at, w, h, p))
            continue;
        const float alpha = world::FloatingNumbers::alpha(n);
        const render::Font& font = ctx().fonts.get(render::FontWeight::Bold, std::round(20.0f * st.fontScale * s));
        const std::string text = world::floatText(n.kind, n.amount);
        render::TextStyle ts;
        ts.color = Color::hex(st.rgb, alpha);
        ts.outline = true;
        ts.outlineColor = Color::hex(0x000000, 0.85f * alpha);
        const float tw = render::measureText(font, text);
        render::drawText(batch, font, text, {std::round(p.x - tw * 0.5f + n.offsetX * s), std::round(p.y - font.lineHeight())},
                         ts);
    }
}

void WorldScene::drawCombatHud()
{
    if (m_phase != Phase::Ready)
        return;
    auto& batch = ctx().batch;
    const ui::Theme& th = theme();
    const float s = ui().scale();
    const float w = ctx().viewportWidth;
    const float h = ctx().viewportHeight;
    const auto px = [s](float v) { return std::round(v * s); };

    // Red flash at the screen edges when the local player is hit.
    if (m_hitFlash > 0.0f)
    {
        const float a = 0.35f * m_hitFlash;
        const float edge = std::round(h * 0.12f);
        batch.fillRectGradient({0.0f, 0.0f, w, edge}, Color::hex(0x8A0C0C, a), Color::hex(0x8A0C0C, 0.0f));
        batch.fillRectGradient({0.0f, h - edge, w, edge}, Color::hex(0x8A0C0C, 0.0f), Color::hex(0x8A0C0C, a));
    }

    // (The own hp / mp bars are in the player frame: drawPlayerFrame; the experience bar is part of the
    // HUD bar: HudBarFrame.)
    const world::HudBottomLines lines = bottomLines();

    // ---- Target frame (top centre, between the player frame and the zone name) ----
    if (m_target != 0 && !m_yield.target)
    {
        std::string name;
        std::string kind; // "Canavar", "Boss canavar", "Kargan · Savaşçı"
        std::uint16_t level = 0;
        std::uint32_t hp = 0;
        std::uint32_t maxHp = 0;
        bool dead = false;
        bool elite = false;
        bool monster = false;
        Color nameColor = Color::hex(0xF4F1E8);
        Color levelColor = th.text;
        if (m_target == m_model.localEntityId())
        {
            name = m_name;
            level = m_self.level;
            hp = m_self.hp;
            maxHp = m_self.maxHp;
            dead = m_self.dead;
            nameColor = th.textTitle;
            kind = describeCharacter(m_race, m_charClass, 0);
        }
        else if (const world::RemoteEntity* e = m_model.find(m_target))
        {
            name = e->name;
            level = e->level;
            hp = e->hp;
            maxHp = e->maxHp;
            dead = e->dead();
            monster = e->isMonster();
            if (monster)
            {
                nameColor = Color::hex(world::levelTintRgb(world::levelTint(m_self.level, e->level)));
                levelColor = nameColor;
                const game::MonsterTemplate* t = monsterTemplate(*e);
                elite = t != nullptr && t->elite;
                kind = elite ? "Boss canavar" : "Canavar"; // M15: elites are the bosses
            }
            else
                kind = describeCharacter(e->race, e->charClass, 0);
        }
        const Rect frame = targetFrameRect(); // M12: shared with the target's effect row
        const float radius = px(7.0f);
        const float line = std::max(1.0f, std::round(s));
        batch.fillRoundedRect({frame.x - px(2.0f), frame.y + px(2.0f), frame.w + px(4.0f), frame.h + px(4.0f)}, radius + px(2.0f),
                              Color::hex(0x000000, 0.28f));
        batch.fillRoundedRectGradient(frame, radius, Color::hex(0x2A313A, 0.95f), Color::hex(0x101419, 0.97f));
        batch.strokeRoundedRect(frame, radius, line, elite ? th.ornament : th.panelBorder);
        batch.strokeRoundedRect(frame.inset(px(3.0f)), std::max(0.0f, radius - px(1.5f)), line, th.panelHighlight);
        // Level chip on the right (the level tint for monsters), "Boss" in gold before it (M15; "Elit" before).
        const render::Font& nf = ctx().fonts.get(render::FontWeight::Bold, 16.0f * s);
        const render::Font& sf = ctx().fonts.get(render::FontWeight::Regular, 12.5f * s);
        const render::Font& cf = ctx().fonts.get(render::FontWeight::Bold, 12.0f * s);
        const std::string lvl = std::format("Sv. {}", level);
        const float chipH = px(19.0f);
        const float chipW = std::round(render::measureText(cf, lvl) + px(14.0f));
        const Rect chip{frame.right() - px(10.0f) - chipW, frame.y + px(8.0f), chipW, chipH};
        batch.fillRoundedRect(chip, chipH * 0.5f, Color::hex(0x07090C, 0.85f));
        batch.strokeRoundedRect(chip, chipH * 0.5f, line, levelColor.withAlpha(0.75f));
        render::TextStyle cs;
        cs.color = levelColor;
        render::drawTextAligned(batch, cf, lvl, chip, render::HAlign::Center, render::VAlign::Middle, cs);
        float nameRight = chip.x - px(8.0f);
        if (elite)
        {
            const float ew = std::round(render::measureText(cf, "Boss") + px(14.0f));
            const Rect echip{chip.x - px(5.0f) - ew, chip.y, ew, chipH};
            batch.fillRoundedRectGradient(echip, chipH * 0.5f, Color::hex(0xE8C060), Color::hex(0x8A6420));
            render::TextStyle es;
            es.color = Color::hex(0x2A1A02);
            render::drawTextAligned(batch, cf, "Boss", echip, render::HAlign::Center, render::VAlign::Middle, es);
            nameRight = echip.x - px(8.0f);
            batch.fillDiamond({frame.x + px(5.0f), frame.y + px(5.0f)}, px(4.0f), th.ornament);
        }
        render::TextStyle ns;
        ns.color = dead ? th.textDim : nameColor;
        ns.shadow = true;
        const Rect nameBox{frame.x + px(12.0f), frame.y + px(6.0f), std::max(0.0f, nameRight - frame.x - px(12.0f)), nf.lineHeight()};
        batch.pushClip(nameBox);
        render::drawTextAligned(batch, nf, name, nameBox, render::HAlign::Left, render::VAlign::Top, ns);
        batch.popClip();
        render::TextStyle ks;
        ks.color = dead ? Color::hex(0xC8746A) : th.textDim;
        ks.shadow = true;
        const Rect kindBox{frame.x + px(12.0f), frame.y + px(27.0f), frame.w - px(24.0f), sf.lineHeight()};
        batch.pushClip(kindBox);
        render::drawTextAligned(batch, sf, dead ? std::string("Ölü") : kind, kindBox, render::HAlign::Left, render::VAlign::Top, ks);
        batch.popClip();
        const Rect bar{frame.x + px(12.0f), frame.y + px(45.0f), frame.w - px(24.0f), px(14.0f)};
        const std::string hpText = dead ? std::string("Ölü") : std::format("{} / {}", hp, maxHp);
        drawBar(batch, ctx().fonts.get(render::FontWeight::Bold, 11.0f * s), bar,
                maxHp > 0 ? static_cast<float>(hp) / static_cast<float>(maxHp) : 0.0f, Color::hex(0xD9442E),
                Color::hex(0x8E1E14), hpText, s);
        (void)monster;
    }

    // ---- Cast bar (above the HUD bar) ----
    if (m_skillTimers.casting())
    {
        const game::SkillDef* sk = game::findSkill(m_skillTimers.castSkill());
        const float bw = px(240.0f);
        const Rect bar{std::round((w - bw) * 0.5f), std::round(lines.castBar), bw, px(18.0f)};
        drawBar(batch, ctx().fonts.get(render::FontWeight::Bold, 12.0f * s), bar, m_skillTimers.castProgress(m_time),
                th.accentBright, th.accent, sk != nullptr ? sk->name : std::string_view("Büyü"), s);
    }

    // ---- Combat message ----
    if (m_combatMessageAge < kCombatMessageTime && !m_combatMessage.empty())
    {
        const float a = std::clamp((kCombatMessageTime - m_combatMessageAge) / 0.6f, 0.0f, 1.0f);
        const render::Font& mf = ctx().fonts.get(render::FontWeight::Bold, 17.0f * s);
        render::TextStyle ms;
        ms.color = Color::hex(0xF7D58A, a);
        ms.outline = true;
        ms.outlineColor = Color::hex(0x000000, 0.85f * a);
        render::drawTextAligned(batch, mf, m_combatMessage, {0.0f, std::round(lines.message), w, mf.lineHeight()},
                                render::HAlign::Center, render::VAlign::Top, ms);
    }

    // ---- Level up ----
    if (m_levelUpAge < kLevelUpTime)
    {
        const float t = m_levelUpAge;
        float a = std::clamp(t / 0.25f, 0.0f, 1.0f);
        if (t > kLevelUpTime - 0.8f)
            a = std::clamp((kLevelUpTime - t) / 0.8f, 0.0f, 1.0f);
        const float grow = 1.0f + 0.25f * std::max(0.0f, 1.0f - t / 0.35f);
        // M12: up to four lines ("Seviye N!", "Can ve mana yenilendi", the stat points, the skill
        // points). The block sits above the own name label (the M11 third line could overlap it):
        // its bottom stays 8 px over the label's top, below the target frame; if that space is too
        // small the whole block is drawn smaller.
        const std::size_t extra = (m_levelUpPoints > 0 ? 1u : 0u) + (m_levelUpSkillPoints > 0 ? 1u : 0u);
        const auto blockHeight = [&](float k) {
            const float title = ctx().fonts.get(render::FontWeight::Bold, std::round(44.0f * s * k)).lineHeight();
            const float line = ctx().fonts.get(render::FontWeight::Regular, std::round(16.0f * s * k)).lineHeight();
            return title + px(4.0f) * k + line + static_cast<float>(extra) * (line + px(4.0f) * k);
        };
        float minTop = px(16.0f);
        if (m_target != 0)
            minTop = targetFrameRect().bottom() + px(8.0f) +
                     (shownEffects(true).empty() ? 0.0f : px(EffectRowWidget::kRowH));
        float k = 1.0f;
        float y = std::round(h * 0.3f);
        if (m_localLabelTop > 0.0f)
        {
            const float limit = m_localLabelTop - px(8.0f);
            if (y + blockHeight(1.0f) > limit)
            {
                const float room = limit - minTop;
                k = std::clamp(room / blockHeight(1.0f), 0.6f, 1.0f);
                y = std::max(minTop, limit - blockHeight(k));
            }
        }
        y = std::round(y);
        const render::Font& lf = ctx().fonts.get(render::FontWeight::Bold, std::round(44.0f * s * k * grow));
        const float titleH = ctx().fonts.get(render::FontWeight::Bold, std::round(44.0f * s * k)).lineHeight();
        // A soft golden glow band behind it.
        const float bandW = std::min(w, px(520.0f));
        const Rect band{std::round((w - bandW) * 0.5f), y - px(10.0f), bandW, titleH + px(20.0f)};
        const float half = std::round(band.h * 0.5f);
        constexpr int kStrips = 32; // fades out towards both ends as well
        const float strip = band.w / static_cast<float>(kStrips);
        for (int i = 0; i < kStrips; ++i)
        {
            const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(kStrips);
            float ramp = std::min(1.0f, (1.0f - std::fabs(u * 2.0f - 1.0f)) * 2.2f);
            ramp = ramp * ramp * (3.0f - 2.0f * ramp);
            const float x0 = std::round(band.x + strip * static_cast<float>(i));
            const float x1 = std::round(band.x + strip * static_cast<float>(i + 1));
            const Color c = Color::hex(0xF2B94A, 0.24f * a * ramp);
            batch.fillRectGradient({x0, band.y, x1 - x0, half}, c.withAlpha(0.0f), c);
            batch.fillRectGradient({x0, band.y + half, x1 - x0, band.h - half}, c, c.withAlpha(0.0f));
        }
        render::TextStyle ls;
        ls.color = Color::hex(0xFFD45C, a);
        ls.outline = true;
        ls.outlineColor = Color::hex(0x3A2400, 0.9f * a);
        render::drawTextAligned(batch, lf, std::format("Seviye {}!", m_levelUpLevel),
                                {0.0f, y - std::round((lf.lineHeight() - titleH) * 0.5f), w, lf.lineHeight()},
                                render::HAlign::Center, render::VAlign::Top, ls);
        const render::Font& sf = ctx().fonts.get(render::FontWeight::Regular, std::round(16.0f * s * k));
        const render::Font& pf = ctx().fonts.get(render::FontWeight::Bold, std::round(16.0f * s * k));
        float pen = y + titleH + std::round(px(4.0f) * k);
        render::TextStyle ss = ls;
        ss.color = Color::hex(0xF4E6C0, a);
        render::drawTextAligned(batch, sf, "Can ve mana yenilendi", {0.0f, pen, w, sf.lineHeight()}, render::HAlign::Center,
                                render::VAlign::Top, ss);
        pen += sf.lineHeight() + std::round(px(4.0f) * k);
        render::TextStyle ps = ls;
        ps.color = Color::hex(0xFFE08A, a);
        if (m_levelUpPoints > 0)
        {
            // M11: the stat points this level-up gave, and where to spend them.
            render::drawTextAligned(batch, pf, world::levelUpPointsText(m_levelUpPoints), {0.0f, pen, w, pf.lineHeight()},
                                    render::HAlign::Center, render::VAlign::Top, ps);
            pen += pf.lineHeight() + std::round(px(4.0f) * k);
        }
        if (m_levelUpSkillPoints > 0)
        {
            // M12: "+1 yetenek puanı  ·  Yetenekler: K".
            render::drawTextAligned(batch, pf, world::levelUpSkillPointsText(m_levelUpSkillPoints),
                                    {0.0f, pen, w, pf.lineHeight()}, render::HAlign::Center, render::VAlign::Top, ps);
            pen += pf.lineHeight();
        }
        m_levelUpToastBottom = pen; // automation: toastabovelabel
    }
    else
        m_levelUpToastBottom = -1.0f;
}
void WorldScene::drawPlayerFrame(ui::DrawContext& dc, const Rect& r)
{
    // The player frame (top left): a level medallion on the left, the name and "race · class" lines
    // (Label children, drawn after this), the hp and mp bars under them.
    auto& b = dc.batch;
    const ui::Theme& th = dc.theme;
    const float s = dc.scale;
    const auto px = [s](float v) { return std::round(v * s); };
    const float radius = px(8.0f);
    const float line = std::max(1.0f, std::round(s));
    const bool inCombat = (m_self.state & vn::kEntityStateInCombat) != 0 && !m_self.dead;
    b.fillRoundedRect({r.x - px(2.0f), r.y + px(2.0f), r.w + px(4.0f), r.h + px(4.0f)}, radius + px(2.0f),
                      Color::hex(0x000000, 0.28f));
    b.fillRoundedRectGradient(r, radius, Color::hex(0x2A313A, 0.95f), Color::hex(0x101419, 0.97f));
    b.strokeRoundedRect(r, radius, line, th.panelBorder);
    b.strokeRoundedRect(r.inset(px(3.0f)), std::max(0.0f, radius - px(1.5f)), line, th.panelHighlight);
    {
        const float len = px(9.0f);
        const float o = px(5.0f);
        const float w = std::max(1.0f, 1.5f * s);
        const Color c = th.ornament.withAlpha(0.8f);
        b.drawLine({r.right() - o, r.y + o}, {r.right() - o - len, r.y + o}, w, c);
        b.drawLine({r.right() - o, r.y + o}, {r.right() - o, r.y + o + len}, w, c);
        b.drawLine({r.right() - o, r.bottom() - o}, {r.right() - o - len, r.bottom() - o}, w, c);
        b.drawLine({r.right() - o, r.bottom() - o}, {r.right() - o, r.bottom() - o - len}, w, c);
    }

    // Medallion: gold rim (red-gold while in combat), dark face, the level in the middle.
    const Vec2 c{std::round(r.x + px(37.0f)), std::round(r.y + r.h * 0.5f)};
    const float R = px(27.0f);
    if (inCombat)
    {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(m_time) * 5.0f);
        const float g = R + px(4.0f) + px(2.0f) * pulse;
        b.fillRoundedRect({c.x - g, c.y - g, 2.0f * g, 2.0f * g}, g, Color::hex(0xD9442E, 0.18f + 0.12f * pulse));
    }
    b.fillRoundedRect({c.x - R - line, c.y - R - line, 2.0f * (R + line), 2.0f * (R + line)}, R + line, Color::hex(0x05070A, 0.9f));
    b.fillRoundedRectGradient({c.x - R, c.y - R, 2.0f * R, 2.0f * R}, R,
                              inCombat ? Color::hex(0xF0A060) : Color::hex(0xF2D27A),
                              inCombat ? Color::hex(0x8E2A14) : Color::hex(0x7A5A24));
    const float Ri = R - px(3.5f);
    b.fillRoundedRectGradient({c.x - Ri, c.y - Ri, 2.0f * Ri, 2.0f * Ri}, Ri, Color::hex(0x2E3540), Color::hex(0x0C0F13));
    const float Rr = Ri - px(2.5f);
    b.strokeRoundedRect({c.x - Rr, c.y - Rr, 2.0f * Rr, 2.0f * Rr}, Rr, line, th.ornament.withAlpha(0.45f));
    // Four small studs on the rim (top, right, bottom, left).
    for (const Vec2 d : {Vec2{0.0f, -1.0f}, Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{-1.0f, 0.0f}})
        b.fillDiamond({c.x + d.x * (R - px(1.5f)), c.y + d.y * (R - px(1.5f))}, px(2.5f), Color::hex(0xFFF0C0));
    render::TextStyle ls;
    ls.color = m_self.dead ? th.textDim : th.textTitle;
    ls.shadow = true;
    ls.shadowColor = Color::hex(0x000000, 0.9f);
    ls.shadowOffset = std::max(1.0f, std::round(s));
    const render::Font& lf = dc.fonts.get(render::FontWeight::Bold, std::round((m_self.level >= 100 ? 17.0f : 22.0f) * s));
    const render::Font& tf = dc.fonts.get(render::FontWeight::Bold, std::round(8.5f * s));
    render::drawTextAligned(b, lf, std::to_string(m_self.level), {c.x - Ri, c.y - Ri + px(3.0f), 2.0f * Ri, 2.0f * Ri},
                            render::HAlign::Center, render::VAlign::Middle, ls);
    render::TextStyle ts;
    ts.color = th.textDim;
    render::drawTextAligned(b, tf, "SV", {c.x - Ri, c.y - Ri + px(5.0f), 2.0f * Ri, tf.lineHeight()}, render::HAlign::Center,
                            render::VAlign::Top, ts);

    // Hp and mp bars.
    const float bx = r.x + px(70.0f);
    const float bw = r.right() - px(12.0f) - bx;
    const Rect hpBar{bx, r.y + px(52.0f), bw, px(16.0f)};
    const Rect mpBar{bx, hpBar.bottom() + px(4.0f), bw, px(12.0f)};
    drawBar(b, dc.fonts.get(render::FontWeight::Bold, 11.5f * s), hpBar,
            static_cast<float>(m_self.hp) / static_cast<float>(std::max<std::uint32_t>(m_self.maxHp, 1)), Color::hex(0xD9442E),
            Color::hex(0x8E1E14), std::format("{} / {}", m_self.hp, m_self.maxHp), s);
    drawBar(b, dc.fonts.get(render::FontWeight::Bold, 10.0f * s), mpBar,
            m_self.maxMp > 0 ? static_cast<float>(m_self.mp) / static_cast<float>(m_self.maxMp) : 0.0f, Color::hex(0x4A86E8),
            Color::hex(0x1F4A9A), std::format("{} / {}", m_self.mp, m_self.maxMp), s);
}

void WorldScene::layoutCombatHud()
{
    // Death screen: dims everything, "Öldün", the countdown and the respawn button.
    ui::Widget& root = ui().root();
    auto& layer = root.add<DeathLayer>();
    layer.setAnchors(anchor::kFill);
    // Upper middle of the screen: the fallen character below it stays visible.
    auto& panel = layer.add<ui::Panel>(ui::Panel::Style::Window, "Kül Oldun");
    panel.setAnchors(anchor::kTop | anchor::kCenterX).setSize(320.0f, 196.0f).setMargins({0.0f, 64.0f, 0.0f, 0.0f});
    panel.setPadding({22.0f, 10.0f, 22.0f, 14.0f});
    auto& stack = panel.add<ui::StackPanel>(ui::Orientation::Vertical, 6.0f);
    stack.setAnchors(anchor::kFill);
    auto& title = stack.add<ui::Label>("Öldün");
    title.setFontSize(32.0f).setWeight(render::FontWeight::Bold).setColor(Color::hex(0xE8584A)).setShadow(true);
    title.setAlign(render::HAlign::Center);
    title.setSize(0.0f, 40.0f);
    title.setName("world.death.title");
    m_deathCountdown = &stack.add<ui::Label>("", ui::TextRole::Normal);
    m_deathCountdown->setColor(theme().text).setShadow(true).setAlign(render::HAlign::Center);
    m_deathCountdown->setSize(0.0f, 24.0f);
    m_deathCountdown->setName("world.death.countdown");
    m_respawnButton = &stack.add<ui::Button>("Yeniden Doğ", [this]() { requestRespawn(); });
    m_respawnButton->setSize(0.0f, 40.0f);
    m_respawnButton->setFocusable(false);
    m_respawnButton->setName("world.death.respawn");
    m_respawnButton->setEnabled(false);
    m_deathLayer = &layer;
    m_deathLayer->setVisible(false);
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::combatStates(std::vector<std::string>& out) const
{
    out.emplace_back(m_target != 0 ? "target" : "notarget");
    const world::RemoteEntity* t = m_target != 0 ? const_cast<world::WorldModel&>(m_model).find(m_target) : nullptr;
    if (t != nullptr && t->isMonster())
        out.emplace_back("targetmonster");
    if (t != nullptr && t->dead())
        out.emplace_back("targetdead");
    if (t != nullptr)
    {
        const float d = world::horizontalDistance(t->position, m_model.local().mover().position());
        if (game::inRange(d, attackRange()))
            out.emplace_back("targetinrange");
    }
    if (m_autoAttack)
        out.emplace_back("attacking");
    if (m_chasing)
        out.emplace_back("chasing");
    if (m_huntLevel != 0)
        out.emplace_back("hunting");
    if (m_skillTimers.casting())
        out.emplace_back("casting");
    out.emplace_back(m_self.dead ? "dead" : "alive");
    if (m_self.dead && m_respawnButton != nullptr && m_respawnButton->isEnabledSelf())
        out.emplace_back("canrespawn");
    out.emplace_back("level" + std::to_string(m_self.level));
    out.emplace_back(inSafeZone() ? "safezone" : "outsidesafe");
    if (const game::MonsterSlot* slot = currentSlot()) // M15: "slot", "slot_<key>"
    {
        out.emplace_back("slot");
        out.emplace_back("slot_" + slot->key);
    }
    if ((m_self.state & vn::kEntityStateInCombat) != 0)
        out.emplace_back("incombat");
    if ((m_self.state & vn::kEntityStateSitting) != 0)
        out.emplace_back("serversitting");
    if (m_self.hp >= m_self.maxHp)
        out.emplace_back("fullhp");
    for (const std::uint32_t pct : {25u, 50u, 75u, 90u})
        if (m_self.hp * 100u >= pct * m_self.maxHp)
            out.emplace_back("hpabove" + std::to_string(pct));
    if (m_skill != nullptr && m_skillTimers.ready(m_skill->id, m_time) && m_self.mp >= m_skill->mpCost)
        out.emplace_back("skillready");
    if (m_levelUpAge < kLevelUpTime)
        out.emplace_back("levelupfx");
    if (m_levelUpSawFull)
        out.emplace_back("levelupfull");
    if (!m_bolts.empty())
        out.emplace_back("bolt");
    if (!m_numbers.items().empty())
        out.emplace_back("numbers");
    if (m_model.monsterCount() > 0)
        out.emplace_back("monsters");
    const auto counter = [&out](const char* name, std::uint32_t n) {
        for (std::uint32_t i = 1; i <= std::min<std::uint32_t>(n, 12); ++i)
            out.emplace_back(name + std::to_string(i));
    };
    counter("kills", m_seen.kills);
    counter("ownhits", m_seen.ownHits);
    counter("skillhits", m_seen.ownSkillHits);
    counter("hitstaken", m_seen.hitsTaken);
    counter("heals", m_seen.heals);
    counter("healed", m_seen.healed);
    counter("seenhits", m_seen.otherHits);
    counter("seendeaths", m_seen.otherDeaths);
    counter("seenattacks", m_seen.remoteAttackClips);
    counter("seenheals", m_seen.remoteHealsSeen);
    counter("seenplayerdamage", m_seen.remoteDamageSeen);
    counter("levelups", m_seen.levelUps);
    counter("respawns", m_seen.respawns);
    counter("bolts", m_boltsLaunched);
}

bool WorldScene::combatAutomation(std::string_view command, bool& handled, std::string& error)
{
    handled = false;
    std::string parseError;
    const auto cmd = world::parseCombatCommand(command, parseError);
    if (!cmd)
    {
        if (parseError.empty())
            return true; // not a combat command
        handled = true;
        error = parseError;
        return false;
    }
    handled = true;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    const Vec3 me = m_model.local().mover().position();
    const auto nearestMonster = [&](std::string_view name) -> world::RemoteEntity* {
        world::RemoteEntity* best = nullptr;
        float bestD = 1e9f;
        for (world::RemoteEntity& e : m_model.entities())
        {
            if (!e.active || !e.isMonster() || e.dead() || (!name.empty() && e.name != name))
                continue;
            const float d = world::horizontalDistance(e.position, me);
            if (d < bestD)
            {
                bestD = d;
                best = &e;
            }
        }
        return best;
    };
    switch (cmd->verb)
    {
    case world::CombatVerb::Target:
        if (cmd->mode == "none")
        {
            clearTarget();
            return true;
        }
        if (cmd->mode == "self")
        {
            setTarget(m_model.localEntityId());
            return true;
        }
        if (cmd->mode == "player")
        {
            for (const world::RemoteEntity& e : m_model.entities())
                if (e.active && !e.isMonster() && e.name == cmd->name)
                {
                    setTarget(e.id);
                    return true;
                }
            return fail(std::format("no remote player '{}'", cmd->name));
        }
        if (world::RemoteEntity* e = nearestMonster(cmd->name))
        {
            setTarget(e->id);
            VO_LOG_INFO("world: targeted '{}' {} at {:.1f} m (level {}, hp {}/{})", e->name, e->id,
                        world::horizontalDistance(e->position, me), e->level, e->hp, e->maxHp);
            return true;
        }
        return fail(std::format("no living monster{} in view ({} monster(s))",
                                cmd->name.empty() ? std::string() : " '" + cmd->name + "'", m_model.monsterCount()));
    case world::CombatVerb::Tab:
        cycleTarget();
        return true;
    case world::CombatVerb::Attack:
        if (m_target == 0)
            return fail("attack: no target");
        startAttack();
        return true;
    case world::CombatVerb::AttackStop:
        stopAttack(true);
        return true;
    case world::CombatVerb::Cast:
        useBarSlot(0); // as the 1 key (M12: slot 1 of the bar)
        return true;
    case world::CombatVerb::Respawn:
        if (!m_self.dead)
            return fail("respawn: not dead");
        requestRespawn();
        return true;
    case world::CombatVerb::Stance:
        requestStance(cmd->mode == "sit");
        return true;
    case world::CombatVerb::ExpectHp:
    case world::CombatVerb::ExpectTargetHp:
    {
        std::uint32_t hp = m_self.hp;
        std::uint32_t maxHp = m_self.maxHp;
        if (cmd->verb == world::CombatVerb::ExpectTargetHp)
        {
            const world::RemoteEntity* t = m_target != 0 ? m_model.find(m_target) : nullptr;
            if (t == nullptr)
                return fail("expecttargethp: no target");
            hp = t->hp;
            maxHp = t->maxHp;
        }
        const float pct = maxHp > 0 ? 100.0f * static_cast<float>(hp) / static_cast<float>(maxHp) : 0.0f;
        VO_LOG_INFO("world: {} hp {}/{} ({:.0f}%)", cmd->verb == world::CombatVerb::ExpectHp ? "own" : "target", hp, maxHp,
                    pct);
        if (pct < cmd->a - 0.01f || pct > cmd->b + 0.01f)
            return fail(std::format("hp {}/{} = {:.1f}%, expected {:.0f}..{:.0f}%", hp, maxHp, pct, cmd->a, cmd->b));
        return true;
    }
    case world::CombatVerb::ExpectLevel:
        if (m_self.level != static_cast<std::uint16_t>(cmd->a))
            return fail(std::format("level {}, expected {}", m_self.level, cmd->a));
        return true;
    case world::CombatVerb::ExpectCombatMsg:
        if (m_combatMessage.find(cmd->name) == std::string::npos)
            return fail(std::format("last combat message is '{}', expected '{}'", m_combatMessage, cmd->name));
        return true;
    case world::CombatVerb::WalkToMonster:
    {
        world::RemoteEntity* e = nearestMonster(cmd->name);
        if (e == nullptr)
            return fail(std::format("no living monster '{}' in view", cmd->name));
        const float d = world::horizontalDistance(e->position, me);
        VO_LOG_INFO("world: walking to '{}' {} ({:.1f} m away, stopping {:.1f} m short)", e->name, e->id, d, cmd->a);
        // The point `stopShort` before it on the way there; if that is blocked (a house), other points
        // of the circle around it (alternating +-30 degrees).
        const float base = std::atan2(me.x - e->position.x, me.z - e->position.z);
        std::string lastError;
        for (int step = 0; step <= 12; ++step)
        {
            const int k = (step + 1) / 2;
            const float a = base + static_cast<float>(step % 2 == 1 ? k : -k) * (world::kPi / 6.0f);
            const float x = e->position.x + cmd->a * std::sin(a);
            const float z = e->position.z + cmd->a * std::cos(a);
            // (never a spot inside the safe zone: nobody can attack from there)
            const world::ItemData* data = ctx().resources.itemData();
            if (data != nullptr && data->monstersLoaded &&
                data->monsters.inSafeZone(m_model.zoneId() != 0 ? m_model.zoneId() : game::kStartZoneId, x, z))
            {
                lastError = "every spot near it is inside the safe zone";
                continue;
            }
            if (setRouteNear(me, x, z, 2.0f, lastError))
                return true;
        }
        return fail(lastError);
    }
    case world::CombatVerb::Hunt:
        if (cmd->mode == "stop")
        {
            m_huntLevel = 0;
            stopAttack(true);
            return true;
        }
        m_huntName = cmd->name;
        m_huntLevel = static_cast<std::uint16_t>(cmd->a);
        m_huntNextTry = 0.0;
        VO_LOG_INFO("world: hunting '{}' until level {}", m_huntName, m_huntLevel);
        return true;
    case world::CombatVerb::Log:
    {
        VO_LOG_INFO("world: combat: level {} xp {}/{} hp {}/{} mp {}/{}{}; target {} attacking {} chasing {} casting {}; "
                    "kills {} hits {} (skill {}) taken {} heals {}/{} seen hits {} deaths {} attacks {}",
                    m_self.level, m_self.xp, m_self.xpToNext, m_self.hp, m_self.maxHp, m_self.mp, m_self.maxMp,
                    m_self.dead ? " DEAD" : "", m_target, m_autoAttack, m_chasing, m_skillTimers.casting(), m_seen.kills,
                    m_seen.ownHits, m_seen.ownSkillHits, m_seen.hitsTaken, m_seen.heals, m_seen.healed, m_seen.otherHits,
                    m_seen.otherDeaths, m_seen.remoteAttackClips);
        for (const world::RemoteEntity& e : m_model.entities())
            if (e.active && e.isMonster())
                VO_LOG_INFO("world:   monster {} '{}' lvl {} hp {}/{} at ({:.1f}, {:.1f}) {:.1f} m{} anim {} {}", e.id, e.name,
                            e.level, e.hp, e.maxHp, e.position.x, e.position.z, world::horizontalDistance(e.position, me),
                            e.dead() ? " dead" : "", world::animStateName(e.animator.state()),
                            e.animator.bound() ? "" : "(unbound)");
        return true;
    }
    }
    return true;
}

} // namespace vo::client::scenes
