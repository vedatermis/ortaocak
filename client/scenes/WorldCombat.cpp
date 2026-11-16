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
constexpr float kBoltTime = 0.25f;        // Köz Oku flight (presentation only)
constexpr double kCombatWindow = 1.0;     // the server's 10/s window ...
constexpr std::size_t kCombatPerWindow = 8; // ... we stay below it

std::string_view skillIcon(std::uint16_t id)
{
    switch (id)
    {
    case game::kSkillHeavyBlow: return "ui/skills/agir_darbe.png";
    case game::kSkillTwinCut: return "ui/skills/cifte_kesik.png";
    case game::kSkillEmberBolt: return "ui/skills/koz_oku.png";
    case game::kSkillPaleSun: return "ui/skills/soluk_gunes.png";
    default: return {};
    }
}

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

// The class skill: icon, key hint, cooldown sweep and seconds, greyed without mana. Click = key 1.
class SkillButton final : public ui::Widget
{
public:
    struct View
    {
        const render::Texture2D* icon = nullptr;
        float sweep = 0.0f;     // 0 ready .. 1 just started
        double remaining = 0.0; // seconds
        bool noMana = false;
        bool disabled = false;  // dead
        bool active = false;    // casting / queued
    };
    SkillButton(std::function<void()> onClick, std::function<View()> view)
        : m_onClick(std::move(onClick)), m_view(std::move(view))
    {
        setName("world.skill");
    }
    bool onMouseDown(const platform::InputEvent& e) override
    {
        if (e.button != platform::MouseButton::Left)
            return false;
        if (m_onClick)
            m_onClick();
        return true;
    }
    void drawSelf(ui::DrawContext& ctx) override
    {
        const View v = m_view ? m_view() : View{};
        const ui::Theme& t = ctx.theme;
        const Rect r = rect();
        auto& b = ctx.batch;
        const float radius = ctx.pxf(6.0f);
        b.fillRoundedRect({r.x - ctx.px(3.0f), r.y - ctx.px(3.0f), r.w + ctx.px(6.0f), r.h + ctx.px(6.0f)},
                          radius + ctx.pxf(2.0f), Color::hex(0x07090C, 0.75f));
        b.fillRoundedRectGradient(r, radius, t.panelTop, t.panelBottom);
        const float inset = ctx.px(4.0f);
        const Rect icon{r.x + inset, r.y + inset, r.w - 2 * inset, r.h - 2 * inset};
        const bool grey = v.noMana || v.disabled;
        if (v.icon != nullptr)
            b.drawImage(*v.icon, icon, grey ? Color{0.45f, 0.45f, 0.5f, 1.0f} : colors::kWhite);
        if (v.noMana && !v.disabled)
            b.fillRect(icon, Color::hex(0x10204A, 0.45f)); // blue-ish: no mana
        if (v.sweep > 0.0f)
        {
            // Clockwise sweep of the remaining time: radial strokes clipped to the icon.
            b.pushClip(icon);
            const Vec2 c{icon.centerX(), icon.centerY()};
            const float len = std::max(icon.w, icon.h) * 0.75f;
            const int steps = 96;
            const int lit = static_cast<int>(std::ceil(static_cast<float>(steps) * v.sweep));
            for (int i = steps - lit; i < steps; ++i)
            {
                const float a = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps) * world::kTwoPi;
                const Vec2 e{c.x + std::sin(a) * len, c.y - std::cos(a) * len};
                b.drawLine(c, e, len * world::kTwoPi / static_cast<float>(steps) * 1.25f, Color::hex(0x000000, 0.6f));
            }
            b.popClip();
            if (v.remaining > 0.6)
            {
                render::TextStyle st;
                st.color = Color::hex(0xFFF3D0);
                st.outline = true;
                const std::string s = std::format("{}", static_cast<int>(std::ceil(v.remaining)));
                render::drawTextAligned(b, ctx.font(20.0f, render::FontWeight::Bold), s, icon, render::HAlign::Center,
                                        render::VAlign::Middle, st);
            }
        }
        const Color border = v.active ? t.accentBright : (isHovered() ? t.buttonBorderHover : t.buttonBorder);
        b.strokeRoundedRect(r, radius, std::max(1.0f, ctx.pxf(v.active ? 2.0f : 1.0f)), border);
        // Key hint in the top-left corner.
        const render::Font& kf = ctx.font(12.0f, render::FontWeight::Bold);
        const Rect key{r.x + ctx.px(2.0f), r.y + ctx.px(1.0f), ctx.px(14.0f), kf.lineHeight()};
        render::TextStyle ks;
        ks.color = t.accentBright;
        ks.outline = true;
        render::drawTextAligned(b, kf, "1", key, render::HAlign::Center, render::VAlign::Top, ks);
    }

private:
    std::function<void()> m_onClick;
    std::function<View()> m_view;
};

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
    if (m_skill != nullptr)
        assets->request(render::AssetCache::Kind::UiImage, skillIcon(m_skill->id));
}

void WorldScene::buildCombatHud()
{
    ui::Widget& root = ui().root();
    m_combatHud = &root.add<CombatHudWidget>(*this);
    m_combatHud->setAnchors(anchor::kFill);

    // The skill button: bottom centre, above the experience bar and the help line.
    auto& skill = root.add<SkillButton>([this]() { useSkill(); },
                                        [this]() {
                                            SkillButton::View v;
                                            if (m_skill == nullptr)
                                                return v;
                                            if (ctx().assets != nullptr)
                                                v.icon = ctx().assets->texture(skillIcon(m_skill->id));
                                            v.sweep = m_skillTimers.sweep(m_skill->id, m_time);
                                            v.remaining = m_skillTimers.remaining(m_skill->id, m_time);
                                            v.noMana = m_self.mp < m_skill->mpCost;
                                            v.disabled = m_self.dead;
                                            v.active = m_skillTimers.casting() || m_queuedSkill != 0;
                                            return v;
                                        });
    skill.setFocusable(false);
    skill.setAnchors(anchor::kBottom | anchor::kCenterX).setMargins({0.0f, 0.0f, 0.0f, 74.0f}).setSize(52.0f, 52.0f);
    skill.setVisible(false);
    m_skillButton = &skill;

    // Safe zone indicator under the inventory button.
    m_safeLabel = &root.add<ui::Label>("● Güvenli Bölge", ui::TextRole::Small);
    m_safeLabel->setColor(Color::hex(0x9FE08A)).setShadow(true).setAlign(render::HAlign::Right, render::VAlign::Middle);
    m_safeLabel->setAnchors(anchor::kTopRight).setMargins({0.0f, 88.0f, 20.0f, 0.0f}).setSize(220.0f, 20.0f);
    m_safeLabel->setName("world.safezone");
    m_safeLabel->setVisible(false);

    // Second help line (the combat keys), above the movement / camera one.
    m_combatHelp = &root.add<ui::Label>("Sol tık: hedef seç  ·  Sağ tık / çift tık / F: saldır  ·  Tab: sıradaki hedef  ·  "
                                        "1: beceri  ·  X: otur / kalk  ·  Esc: hedefi bırak",
                                        ui::TextRole::Small);
    m_combatHelp->setColor(theme().text).setShadow(true).setAlign(render::HAlign::Center);
    m_combatHelp->setAnchors(anchor::kBottom | anchor::kFillX).setMargins({10.0f, 0.0f, 10.0f, 28.0f}).setSize(0.0f, 20.0f);
    m_combatHelp->setName("world.help.combat");
    m_combatHelp->setVisible(false);
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
    std::uint32_t target = 0;
    if (s->target == game::SkillTarget::Enemy)
        target = m_target;
    else if (m_target != 0 && m_target != m_model.localEntityId())
    {
        const world::RemoteEntity* e = m_model.find(m_target);
        target = e != nullptr && !e->isMonster() ? m_target : 0;
    }
    if (sendCombat(vn::SkillReq{skillId, target}))
        VO_LOG_INFO("world: SKILL_REQ {} ({}) on {}", skillId, s->name, target);
    else
        showCombatMessage("Çok hızlı: biraz bekle");
}

void WorldScene::useSkill()
{
    if (m_skill == nullptr || m_phase != Phase::Ready || m_menuOpen)
        return;
    const game::SkillDef& s = *m_skill;
    if (m_self.dead)
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::YouAreDead));
        return;
    }
    if (m_skillTimers.casting())
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::Busy));
        return;
    }
    if (!m_skillTimers.ready(s.id, m_time))
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::OnCooldown));
        return;
    }
    if (m_self.mp < s.mpCost)
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::NotEnoughMp));
        return;
    }
    const Vec3 me = m_model.local().mover().position();
    const world::RemoteEntity* t = m_target != 0 ? m_model.find(m_target) : nullptr;
    if (s.target == game::SkillTarget::Enemy)
    {
        if (t == nullptr)
        {
            showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::NoTarget));
            return;
        }
        if (!t->isMonster())
        {
            showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::WrongTargetType));
            return;
        }
        if (t->dead())
        {
            showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::TargetDead));
            return;
        }
    }
    else if (t == nullptr || t->isMonster() || t->dead())
    {
        t = nullptr; // Soluk Güneş: on ourselves without a friendly target (or with a hostile one)
    }
    if (m_localAnim.seated())
        m_localAnim.standUp();
    if (t != nullptr && !game::inRange(world::horizontalDistance(t->position, me), s.range))
    {
        // Walk into range first, then cast (cancelled by moving by hand).
        m_queuedSkill = s.id;
        m_chasing = true;
        m_chaseReplan = 0.0f;
        return;
    }
    sendSkill(s.id);
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
        if (m.targetId == me)
        {
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
        if (haveNumberAt && (damage || heal))
            m_numbers.add(world::classifyCombatNumber(m.kind, m.flags, m.targetId == me), m.amount, numberAt);
        // The source: swing / punch animation, timers.
        const game::SkillDef* skill = m.skillId != 0 ? game::findSkill(m.skillId) : nullptr;
        const bool melee = m.skillId == 0 || (skill != nullptr && skill->castMs == 0 && !skill->projectile &&
                                              skill->target == game::SkillTarget::Enemy);
        if (m.sourceId == me)
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
            if (melee && (m.skillId == 0 || damage))
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
            if (melee && !src->casting)
            {
                const game::ItemTemplate* weapon = src->isMonster() ? nullptr : weaponOf(src->mainHand);
                if (src->animator.playAttack(world::attackClipFor(weapon, src->isMonster(), src->swings++)))
                    ++m_seen.remoteAttackClips;
            }
        }
        if (m.sourceId != me && m.targetId != me && damage)
            ++m_seen.otherHits;
        // Köz Oku: the bolt flies from the caster's hand to the target (presentation only).
        if (skill != nullptr && skill->projectile)
        {
            Vec3 from{};
            Vec3 to{};
            bool ok = true;
            if (m.sourceId == me)
            {
                const Vec3 p = m_model.local().visualPosition();
                from = p + world::forwardFromYaw(m_model.local().mover().yaw()) * 0.45f + Vec3{0.0f, 1.35f, 0.0f};
            }
            else if (src != nullptr)
                from = src->position + world::forwardFromYaw(src->yaw) * 0.45f + Vec3{0.0f, 1.35f, 0.0f};
            else
                ok = false;
            if (m.targetId == me)
                to = m_model.local().visualPosition() + Vec3{0.0f, 1.1f, 0.0f};
            else if (tgt != nullptr)
                to = tgt->position + Vec3{0.0f, 1.0f * tgt->scale, 0.0f};
            else
                ok = false;
            if (ok && m_bolts.size() < 8)
            {
                m_bolts.push_back({m.targetId, from, to, 0.0f});
                ++m_boltsLaunched;
            }
        }
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
            m_self.level = m.level;
            m_levelUpLevel = m.level;
            m_levelUpAge = 0.0f;
            ++m_seen.levelUps;
            const auto& entry = ctx().net.account().world;
            if (m_levelLabel != nullptr && entry)
                m_levelLabel->setText(describeCharacter(entry->character.race, entry->character.charClass, m.level));
            m_inventory.setRules(ctx().resources.itemData() != nullptr && ctx().resources.itemData()->catalogLoaded
                                     ? &ctx().resources.itemData()->catalog
                                     : nullptr,
                                 {m_charClass, m.level});
            VO_LOG_INFO("world: LEVEL_UP -> {}", m.level);
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
    m_hitFlash = std::max(0.0f, m_hitFlash - dt * 2.5f);
    m_numbers.update(dt);
    for (Bolt& b : m_bolts)
        b.age += dt;
    std::erase_if(m_bolts, [](const Bolt& b) { return b.age >= kBoltTime; });

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
            const float range = queued != nullptr ? queued->range : attackRange();
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
        m_safeLabel->setVisible(m_phase == Phase::Ready && inSafeZone());
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
        VO_LOG_WARN("world: hunt ended: the character died");
        m_huntLevel = 0;
        return;
    }
    if (m_self.level >= m_huntLevel)
    {
        VO_LOG_INFO("world: hunt done: level {}", m_self.level);
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
    const bool sitting = (m_self.state & vn::kEntityStateSitting) != 0;
    const bool low = m_self.hp * 100u < 60u * m_self.maxHp;
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
    // The nearest living monster of that name.
    const Vec3 me = m_model.local().mover().position();
    const world::RemoteEntity* best = nullptr;
    float bestD = 1e9f;
    for (const world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active || !e.isMonster() || e.dead() || e.name != m_huntName)
            continue;
        const float d = world::horizontalDistance(e.position, me);
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
    // Köz Oku bolts.
    if (const render::GpuMesh* bolt = ctx().assets->mesh("effects/ember_bolt.vmesh"))
    {
        for (const Bolt& b : m_bolts)
        {
            Vec3 to = b.to;
            if (b.target == m_model.localEntityId())
                to = m_model.local().visualPosition() + Vec3{0.0f, 1.1f, 0.0f};
            else if (const world::RemoteEntity* e = m_model.find(b.target))
                to = e->position + Vec3{0.0f, 1.0f * e->scale, 0.0f};
            const float u = std::clamp(b.age / kBoltTime, 0.0f, 1.0f);
            const Vec3 pos = assets::lerp(b.from, to, u) + Vec3{0.0f, 0.25f * 4.0f * u * (1.0f - u), 0.0f};
            Vec3 z = to - b.from;
            constexpr float kBoltScale = 1.6f; // the 0.5 m model reads better a little larger
            if (assets::length(z) < 1e-3f)
                continue;
            z = assets::normalize(z);
            Vec3 x = assets::cross(Vec3{0.0f, 1.0f, 0.0f}, z);
            if (assets::length(x) < 1e-3f)
                x = {1.0f, 0.0f, 0.0f};
            x = assets::normalize(x);
            const Vec3 y = assets::cross(z, x);
            world::Mat4 w = world::Mat4::identity();
            w.at(0, 0) = x.x * kBoltScale;
            w.at(1, 0) = x.y * kBoltScale;
            w.at(2, 0) = x.z * kBoltScale;
            w.at(0, 1) = y.x * kBoltScale;
            w.at(1, 1) = y.y * kBoltScale;
            w.at(2, 1) = y.z * kBoltScale;
            w.at(0, 2) = z.x * kBoltScale;
            w.at(1, 2) = z.y * kBoltScale;
            w.at(2, 2) = z.z * kBoltScale;
            w.at(0, 3) = pos.x;
            w.at(1, 3) = pos.y;
            w.at(2, 3) = pos.z;
            r.addEffectMesh(bolt, w);
        }
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
        const float u = std::clamp(b.age / kBoltTime, 0.0f, 1.0f);
        const Vec3 pos = assets::lerp(b.from, to, u) + Vec3{0.0f, 0.25f * 4.0f * u * (1.0f - u), 0.0f};
        world::Vec2 p{};

        if (!world::projectToScreen(vp, pos, w, h, p))
            continue;
        const float dist = std::max(assets::length(pos - eye), 1.0f);
        const float r0 = std::clamp(260.0f / dist, 6.0f, 40.0f) * s;
        for (int i = 4; i >= 1; --i)
        {
            const float r = r0 * static_cast<float>(i) / 4.0f;
            batch.fillRoundedRect({p.x - r, p.y - r, 2.0f * r, 2.0f * r}, r,
                                  Color::hex(i == 1 ? 0xFFE08A : 0xFF7A1E, 0.10f + 0.12f * static_cast<float>(4 - i)));
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
        const std::string text = n.kind == world::FloatKind::Heal ? std::format("+{}", n.amount)
                                 : n.kind == world::FloatKind::Critical ? std::format("{}!", n.amount)
                                                                        : std::format("{}", n.amount);
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

    // ---- Own hp / mp bars inside the top-left panel (below the name and class line) ----
    if (m_hudPanel != nullptr && m_hudPanel->isVisible())
    {
        const Rect p = m_hudPanel->rect();
        const Rect hpBar{p.x + px(12.0f), p.y + px(58.0f), p.w - px(24.0f), px(17.0f)};
        const Rect mpBar{p.x + px(12.0f), hpBar.bottom() + px(4.0f), p.w - px(24.0f), px(13.0f)};
        drawBar(batch, ctx().fonts.get(render::FontWeight::Bold, 12.0f * s), hpBar,
                static_cast<float>(m_self.hp) / static_cast<float>(std::max<std::uint32_t>(m_self.maxHp, 1)),
                Color::hex(0xD9442E), Color::hex(0x8E1E14), std::format("{} / {}", m_self.hp, m_self.maxHp), s);
        drawBar(batch, ctx().fonts.get(render::FontWeight::Bold, 10.0f * s), mpBar,
                m_self.maxMp > 0 ? static_cast<float>(m_self.mp) / static_cast<float>(m_self.maxMp) : 0.0f,
                Color::hex(0x4A86E8), Color::hex(0x1F4A9A), std::format("{} / {}", m_self.mp, m_self.maxMp), s);
    }

    // ---- Target frame (top centre, clear of the left panel and the right-hand buttons) ----
    if (m_target != 0)
    {
        std::string name;
        std::uint16_t level = 0;
        std::uint32_t hp = 0;
        std::uint32_t maxHp = 0;
        bool dead = false;
        bool elite = false;
        Color nameColor = Color::hex(0xF4F1E8);
        if (m_target == m_model.localEntityId())
        {
            name = m_name;
            level = m_self.level;
            hp = m_self.hp;
            maxHp = m_self.maxHp;
            dead = m_self.dead;
            nameColor = th.textTitle;
        }
        else if (const world::RemoteEntity* e = m_model.find(m_target))
        {
            name = e->name;
            level = e->level;
            hp = e->hp;
            maxHp = e->maxHp;
            dead = e->dead();
            if (e->isMonster())
            {
                nameColor = Color::hex(world::levelTintRgb(world::levelTint(m_self.level, e->level)));
                const game::MonsterTemplate* t = monsterTemplate(*e);
                elite = t != nullptr && t->elite;
            }
        }
        float left = px(14.0f);
        float right = w - px(14.0f);
        if (m_hudPanel != nullptr && m_hudPanel->isVisible())
            left = m_hudPanel->rect().right() + px(12.0f);
        if (m_invButton != nullptr && m_invButton->isVisible())
            right = std::min(right, m_invButton->rect().x - px(12.0f));
        if (m_zoneLabel != nullptr && m_zoneLabel->isVisible())
        {
            const render::Font& zf = ctx().fonts.get(render::FontWeight::Bold, 20.0f * s);
            right = std::min(right, m_zoneLabel->rect().right() - render::measureText(zf, m_zoneName) - px(12.0f));
        }
        float fw = std::min(px(280.0f), right - left);
        fw = std::max(fw, px(150.0f));
        float fx = std::round((w - fw) * 0.5f);
        fx = std::clamp(fx, left, std::max(left, right - fw));
        const float fh = px(60.0f);
        const Rect frame{fx, px(14.0f), fw, fh};
        batch.fillRoundedRectGradient(frame, px(6.0f), th.panelTop, th.panelBottom);
        batch.strokeRoundedRect(frame, px(6.0f), std::max(1.0f, s), th.panelBorder);
        const render::Font& nf = ctx().fonts.get(render::FontWeight::Bold, 16.0f * s);
        const render::Font& sf = ctx().fonts.get(render::FontWeight::Regular, 13.0f * s);
        render::TextStyle ns;
        ns.color = dead ? th.textDim : nameColor;
        ns.shadow = true;
        const std::string lvl = std::format("Sv. {}", level);
        const float lvlW = render::measureText(sf, lvl);
        const Rect nameBox{frame.x + px(10.0f), frame.y + px(6.0f), frame.w - px(20.0f) - lvlW - px(8.0f), nf.lineHeight()};
        batch.pushClip(nameBox);
        render::drawTextAligned(batch, nf, name, nameBox, render::HAlign::Left, render::VAlign::Top, ns);
        batch.popClip();
        render::TextStyle ls;
        ls.color = elite ? th.accentBright : th.textDim;
        ls.shadow = true;
        render::drawTextAligned(batch, sf, elite ? std::format("Elit · {}", lvl) : lvl,
                                {frame.x + px(10.0f), frame.y + px(8.0f), frame.w - px(20.0f), nf.lineHeight()},
                                render::HAlign::Right, render::VAlign::Top, ls);
        if (elite)
            batch.fillDiamond({frame.x + px(4.0f), frame.y + px(4.0f)}, px(4.0f), th.ornament);
        const Rect bar{frame.x + px(10.0f), frame.y + px(32.0f), frame.w - px(20.0f), px(18.0f)};
        const std::string hpText = dead ? std::string("Ölü") : std::format("{} / {}", hp, maxHp);
        drawBar(batch, ctx().fonts.get(render::FontWeight::Bold, 12.0f * s), bar,
                maxHp > 0 ? static_cast<float>(hp) / static_cast<float>(maxHp) : 0.0f, Color::hex(0xD9442E),
                Color::hex(0x8E1E14), hpText, s);
    }

    // ---- Experience bar (bottom, above the help line) ----
    {
        const float bw = std::min(px(560.0f), w - px(40.0f));
        const Rect bar{std::round((w - bw) * 0.5f), h - px(66.0f), bw, px(14.0f)};
        const float f = m_self.xpToNext > 0 ? static_cast<float>(m_self.xp) / static_cast<float>(m_self.xpToNext) : 1.0f;
        const std::string text = m_self.xpToNext > 0
                                     ? std::format("Seviye {}  ·  {} / {} deneyim", m_self.level, m_self.xp, m_self.xpToNext)
                                     : std::format("Seviye {}  ·  en yüksek seviye", m_self.level);
        drawBar(batch, ctx().fonts.get(render::FontWeight::Bold, 11.0f * s), bar, f, Color::hex(0xB98CF0),
                Color::hex(0x6A3FB0), text, s);
    }

    // ---- Cast bar (above the skill button) ----
    if (m_skillTimers.casting())
    {
        const game::SkillDef* sk = game::findSkill(m_skillTimers.castSkill());
        const float bw = px(240.0f);
        const Rect bar{std::round((w - bw) * 0.5f), h - px(152.0f), bw, px(18.0f)};
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
        render::drawTextAligned(batch, mf, m_combatMessage, {0.0f, h - px(180.0f), w, mf.lineHeight()},
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
        const render::Font& lf = ctx().fonts.get(render::FontWeight::Bold, std::round(44.0f * s * grow));
        const float y = std::round(h * 0.3f);
        // A soft golden glow band behind it.
        const float bandW = std::min(w, px(520.0f));
        const Rect band{std::round((w - bandW) * 0.5f), y - px(10.0f), bandW, lf.lineHeight() + px(20.0f)};
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
        render::drawTextAligned(batch, lf, std::format("Seviye {}!", m_levelUpLevel), {0.0f, y, w, lf.lineHeight()},
                                render::HAlign::Center, render::VAlign::Top, ls);
        const render::Font& sf = ctx().fonts.get(render::FontWeight::Regular, 16.0f * s);
        render::TextStyle ss = ls;
        ss.color = Color::hex(0xF4E6C0, a);
        render::drawTextAligned(batch, sf, "Can ve mana yenilendi", {0.0f, y + lf.lineHeight() + px(4.0f), w, sf.lineHeight()},
                                render::HAlign::Center, render::VAlign::Top, ss);
    }
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
        useSkill();
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
