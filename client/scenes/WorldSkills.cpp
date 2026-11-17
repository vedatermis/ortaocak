// WorldScene, skills half (M12 "Yetenek Yolları", docs/PROTOCOL_SKILLS.md §7): SKILL_STATE, the
// skill bar (keys 1-8, drag & drop, right click), the "Yetenekler" window and its HUD button, learning
// with the once-per-session confirmation, per-skill cooldowns, timed effects (icons under the player's
// panel and the target frame, tooltips), the effect meshes in the world (docs/SKILL_ART.md), the leap
// glide and the skill automation. The pure rules live in client/world/SkillsView.h; this file wires
// them to the network, the input and the screen.
#include "client/scenes/WorldScene.h"

#include "client/net/GameClient.h"
#include "client/platform/Input.h"
#include "client/render/AssetCache.h"
#include "client/render/TextRenderer.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/CharacterPreview.h"
#include "client/scenes/ItemTooltip.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/SkillWindow.h"
#include "client/ui/Ui.h"
#include "client/world/ItemData.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Monsters.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;
using world::Vec3;

namespace {

constexpr float kBarSlot = 48.0f; // logical; the HUD bar (resized for narrow screens by layoutHud)
constexpr float kBarGap = 6.0f;
constexpr float kLeapTime = 0.25f;  // SKILL_LEAP glide (s)
constexpr float kLeapArc = 0.35f;   // metres of hop at the middle of the glide
constexpr std::size_t kMaxArt = 80;

// Every effect mesh of docs/SKILL_ART.md §3 (requested at world entry).
constexpr std::array<std::string_view, 23> kEffectMeshes = {
    "frost_bolt",       "ember_mote",         "light_strike",         "ground_ring_flame",  "ground_ring_frost",
    "ground_ring_heal", "ground_ring_fog",    "ground_ring_neutral",  "shield_bubble_gold", "shield_bubble_frost",
    "shield_bubble_neutral", "buff_aura_gold", "buff_aura_red",       "buff_aura_steel",    "buff_aura_green",
    "buff_aura_neutral", "tick_wisp_burn",    "tick_wisp_poison",     "stun_marker",        "root_spikes",
    "leap_trail",       "fog_cloud",          "ember_bolt"};

// A Label that is never drawn: carries text for automation (expecttext).
class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

std::uint32_t peekEntityId(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < 4)
        return 0;
    return static_cast<std::uint32_t>(payload[0]) | (static_cast<std::uint32_t>(payload[1]) << 8) |
           (static_cast<std::uint32_t>(payload[2]) << 16) | (static_cast<std::uint32_t>(payload[3]) << 24);
}

// Rotation about Y by `yaw`, then non-uniform scale (x, y, z) of the mesh's own axes.
world::Mat4 spinMatrix(Vec3 pos, float yaw, float sx, float sy, float sz)
{
    world::Mat4 w = world::makeWorld(pos, yaw, 1.0f);
    for (int r = 0; r < 3; ++r)
    {
        w.at(r, 0) *= sx;
        w.at(r, 1) *= sy;
        w.at(r, 2) *= sz;
    }
    return w;
}

// On sloping ground: the mesh's +Y along the terrain normal `n`, turned by `yaw` about it, scaled.
world::Mat4 groundMatrix(Vec3 pos, Vec3 n, float yaw, float sx, float sy, float sz)
{
    const Vec3 up = assets::length(n) > 1e-4f ? assets::normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 f = world::forwardFromYaw(yaw);
    Vec3 z = f - up * assets::dot(f, up);
    z = assets::length(z) > 1e-4f ? assets::normalize(z) : Vec3{0.0f, 0.0f, 1.0f};
    const Vec3 x = assets::cross(up, z);
    world::Mat4 w = world::Mat4::identity();
    const Vec3 cols[3] = {x * sx, up * sy, z * sz};
    for (int c = 0; c < 3; ++c)
    {
        w.at(0, c) = cols[c].x;
        w.at(1, c) = cols[c].y;
        w.at(2, c) = cols[c].z;
    }
    w.at(0, 3) = pos.x;
    w.at(1, 3) = pos.y;
    w.at(2, 3) = pos.z;
    return w;
}

// The mesh's +Z along `dir`, scaled (x, y, z).
world::Mat4 aimMatrix(Vec3 pos, Vec3 dir, float sx, float sy, float sz)
{
    Vec3 z = assets::length(dir) > 1e-4f ? assets::normalize(dir) : Vec3{0.0f, 0.0f, 1.0f};
    Vec3 x = assets::cross(Vec3{0.0f, 1.0f, 0.0f}, z);
    if (assets::length(x) < 1e-3f)
        x = {1.0f, 0.0f, 0.0f};
    x = assets::normalize(x);
    const Vec3 y = assets::cross(z, x);
    world::Mat4 w = world::Mat4::identity();
    const Vec3 cols[3] = {x * sx, y * sy, z * sz};
    for (int c = 0; c < 3; ++c)
    {
        w.at(0, c) = cols[c].x;
        w.at(1, c) = cols[c].y;
        w.at(2, c) = cols[c].z;
    }
    w.at(0, 3) = pos.x;
    w.at(1, 3) = pos.y;
    w.at(2, 3) = pos.z;
    return w;
}

float hash01(std::uint32_t n)
{
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return static_cast<float>(n & 0xFFFFu) / 65535.0f;
}

bool neutralMesh(std::string_view mesh)
{
    return mesh.ends_with("_neutral");
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void WorldScene::requestSkillAssets()
{
    render::AssetCache* assets = ctx().assets;
    if (assets == nullptr)
        return;
    for (const std::string_view m : kEffectMeshes)
        assets->request(render::AssetCache::Kind::Mesh, std::format("effects/{}.vmesh", m));
}

void WorldScene::buildSkills()
{
    ui::Widget& root = ui().root();
    // The slots: inside the HUD bar's middle group (placed and sized by layoutHud()).
    m_skillBar = &m_hudBar->add<SkillSlotsWidget>(static_cast<SkillUiHost&>(*this), "skillbar.slot", kBarSlot, kBarGap, true);
    m_skillBar->setName("skillbar");
    m_skillBar->setAnchors(anchor::kTopLeft);
    m_skillBar->setVisible(false);

    // The player's effects under the player frame; the target's under the target frame (placed per frame).
    m_selfEffects = &root.add<EffectRowWidget>(static_cast<SkillUiHost&>(*this), false, 10);
    m_selfEffects->setAnchors(anchor::kTopLeft)
        .setMargins({world::kHudPlayerFrame.x + 2.0f, world::kHudSelfEffectsTop, 0.0f, 0.0f})
        .setSize(10.0f * (EffectRowWidget::kIcon + EffectRowWidget::kGap), 2.0f * EffectRowWidget::kRowH);
    m_selfEffects->setVisible(false);
    m_targetEffects = &root.add<EffectRowWidget>(static_cast<SkillUiHost&>(*this), true, 9);
    m_targetEffects->setAnchors(anchor::kTopLeft)
        .setSize(9.0f * (EffectRowWidget::kIcon + EffectRowWidget::kGap), 2.0f * EffectRowWidget::kRowH);
    m_targetEffects->setVisible(false);

    // (M12's "Yetenekler (K)" button is one of the HUD bar's menu buttons now: buildHudBar.)
    auto hudTip = std::make_unique<TextProbe>();
    hudTip->setName("hud.tooltip");
    hudTip->setSize(0.0f, 0.0f);
    m_hudTipProbe = hudTip.get();
    root.addChild(std::move(hudTip));
    auto tip = std::make_unique<TextProbe>();
    tip->setName("skills.tooltip");
    tip->setSize(0.0f, 0.0f);
    m_skillTipProbe = tip.get();
    root.addChild(std::move(tip));
    auto etip = std::make_unique<TextProbe>();
    etip->setName("effects.tooltip");
    etip->setSize(0.0f, 0.0f);
    m_effectTipProbe = etip.get();
    root.addChild(std::move(etip));

    m_skillsOpen = false;
    m_skillDrag = {};
    m_lastSkillMessage.clear();
    m_levelUpSkillPoints = 0;
    m_skillsSeen = {};
    m_art.clear();
    m_leaps.clear();
    m_artCounts.clear();
    m_lastUsedSkill = 0;
    m_lastUseHits = 0;
    m_pulling = false;
    m_pullDone = false;
    m_skillSends.fill(-1e9);
    m_skillSendNext = 0;
    m_deferredLearn = 0;
    m_deferredBar.reset();
}

void WorldScene::buildSkillWindow()
{
    m_skillWindow = &ui().root().add<SkillWindow>(static_cast<SkillUiHost&>(*this), m_charClass);
    // Left half, vertically centred, like the Künye (drawn above it).
    m_skillWindow->setAnchors(anchor::kLeft | anchor::kCenterY).setMargins({14.0f, 0.0f, 0.0f, 0.0f});
    m_skillWindow->setVisible(false);
}

void WorldScene::setSkillsOpen(bool open)
{
    if (m_skillWindow == nullptr || (open && m_phase != Phase::Ready))
        return;
    if (open && (m_leavingWorld || m_model.leaving()))
        return;
    if (open == m_skillsOpen)
        return;
    m_skillsOpen = open;
    if (open)
        m_skillWindow->setClassLine(describeCharacter(m_race, m_charClass, m_self.level));
    m_skillWindow->setVisible(open);
    VO_LOG_INFO("world: Yetenekler {}", open ? "opened" : "closed");
}

void WorldScene::showSkillMessage(std::string_view text, bool error)
{
    m_lastSkillMessage = std::string(text);
    if (m_skillWindow != nullptr)
        m_skillWindow->setMessage(std::string(text), error);
    if (!m_skillsOpen && !text.empty())
        showCombatMessage(text);
    if (!text.empty())
        VO_LOG_INFO("world: skill message: {}", text);
}

// ---------------------------------------------------------------------------------------------
// SkillUiHost
// ---------------------------------------------------------------------------------------------

SkillUiHost::SlotView WorldScene::slotView(std::size_t slot) const
{
    SlotView v;
    const std::uint16_t id = m_skillBook.slot(slot);
    const game::SkillDef* s = id != 0 ? game::findSkill(id) : nullptr;
    if (s == nullptr || s->charClass != m_charClass)
        return v;
    v.skill = s;
    v.rank = m_skillBook.rank(id);
    v.look = world::slotLook(s, v.rank, m_self.mp, m_self.dead, skillSelection());
    v.sweep = m_skillTimers.sweep(id, m_time);
    v.remaining = m_skillTimers.remaining(id, m_time);
    const double since = m_time - m_skillTimers.cooldownEnd(id);
    if (since >= 0.0 && since < 0.25 && m_skillTimers.cooldownEnd(id) > 0.0)
        v.readyFlash = static_cast<float>(1.0 - since / 0.25);
    v.active = (m_skillTimers.casting() && m_skillTimers.castSkill() == id) || m_queuedSkill == id;
    return v;
}

const render::Texture2D* WorldScene::skillImage(std::string_view path) const
{
    return ctx().resources.uiImage(path);
}

std::span<const world::ShownEffect> WorldScene::shownEffects(bool target) const
{
    if (target)
        return m_target != 0 ? m_effects.of(m_target) : std::span<const world::ShownEffect>{};
    return m_effects.of(m_model.localEntityId());
}

world::SkillSelection WorldScene::skillSelection() const
{
    world::SkillSelection s;
    if (m_target == 0)
        return s;
    s.id = m_target;
    if (m_target == m_model.localEntityId())
    {
        s.self = true;
        s.dead = m_self.dead;
        return s;
    }
    const world::RemoteEntity* e = const_cast<world::WorldModel&>(m_model).find(m_target);
    if (e == nullptr)
    {
        s.id = 0;
        return s;
    }
    s.monster = e->isMonster();
    s.player = !e->isMonster() && !e->isLoot();
    s.dead = e->dead();
    s.distance = world::horizontalDistance(e->position, m_model.local().mover().position());
    return s;
}

void WorldScene::useBarSlot(std::size_t slot)
{
    if (m_phase != Phase::Ready || m_menuOpen || slot >= game::kSkillBarSlots)
        return;
    const std::uint16_t id = m_skillBook.slot(slot);
    if (id == 0)
        return;
    useSkillById(id);
}

void WorldScene::useSkillById(std::uint16_t skillId)
{
    const game::SkillDef* s = game::findSkill(skillId);
    if (s == nullptr || s->charClass != m_charClass || m_phase != Phase::Ready || m_menuOpen)
        return;
    const std::uint8_t rank = m_skillBook.rank(skillId);
    if (m_self.dead)
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::YouAreDead));
        return;
    }
    if (rank == 0)
    {
        showCombatMessage(world::kNotLearnedText);
        return;
    }
    if (m_skillTimers.casting())
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::Busy));
        return;
    }
    if (!m_skillTimers.ready(skillId, m_time))
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::OnCooldown));
        return;
    }
    if (m_self.mp < s->at(rank).mpCost)
    {
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::NotEnoughMp));
        return;
    }
    const world::SkillUse use = world::decideSkillUse(*s, rank, skillSelection());
    switch (use.verdict)
    {
    case world::SkillUseVerdict::NotLearned: showCombatMessage(world::kNotLearnedText); return;
    case world::SkillUseVerdict::NoTarget: showCombatMessage(world::kNoTargetText); return;
    case world::SkillUseVerdict::WrongTarget:
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::WrongTargetType));
        return;
    case world::SkillUseVerdict::TargetDead:
        showCombatMessage(world::actionFailText(vn::CombatAction::Skill, vn::ActionFailReason::TargetDead));
        return;
    case world::SkillUseVerdict::WalkFirst:
        if (m_localAnim.seated())
            m_localAnim.standUp();
        // Walk into range first, then use it (cancelled by moving by hand).
        m_queuedSkill = skillId;
        m_chasing = true;
        m_chaseReplan = 0.0f;
        return;
    case world::SkillUseVerdict::Send: break;
    }
    if (m_localAnim.seated())
        m_localAnim.standUp();
    sendSkill(skillId);
}

void WorldScene::clearBarSlot(std::size_t slot)
{
    if (slot < game::kSkillBarSlots && m_skillBook.slot(slot) != 0)
        sendSkillBar(world::barWithCleared(m_skillBook.bar(), slot));
}

void WorldScene::beginSkillDrag(std::uint16_t skillId, int fromSlot)
{
    m_skillDrag.active = true;
    m_skillDrag.skill = skillId;
    m_skillDrag.fromSlot = fromSlot;
    m_skillDrag.x = static_cast<float>(ctx().input.mouseX());
    m_skillDrag.y = static_cast<float>(ctx().input.mouseY());
}

void WorldScene::moveSkillDrag(float x, float y)
{
    m_skillDrag.x = x;
    m_skillDrag.y = y;
}

void WorldScene::endSkillDrag(float x, float y, bool drop)
{
    if (!m_skillDrag.active)
        return;
    const SkillDrag d = m_skillDrag;
    m_skillDrag = {};
    if (!drop)
        return;
    int slot = m_skillBar != nullptr ? m_skillBar->slotAt(x, y) : -1;
    if (slot < 0 && m_skillsOpen && m_skillWindow != nullptr && m_skillWindow->barSlots() != nullptr)
        slot = m_skillWindow->barSlots()->slotAt(x, y);
    game::SkillBar bar = m_skillBook.bar();
    if (slot >= 0)
    {
        if (d.fromSlot >= 0)
        {
            if (d.fromSlot == slot)
                return;
            bar = world::barWithSwapped(bar, static_cast<std::size_t>(d.fromSlot), static_cast<std::size_t>(slot));
        }
        else
            bar = world::barWithPlaced(bar, static_cast<std::size_t>(slot), d.skill);
    }
    else if (d.fromSlot >= 0)
        bar = world::barWithCleared(bar, static_cast<std::size_t>(d.fromSlot)); // dragged off the bar
    else
        return; // a skill from the window dropped elsewhere: nothing
    VO_LOG_INFO("world: skill {} dragged from {} to slot {}", d.skill, d.fromSlot, slot);
    sendSkillBar(bar);
}

void WorldScene::raiseSkill(std::uint16_t skillId)
{
    if (!m_skillBook.known() || m_skillBook.learnBusy())
        return;
    const game::SkillDef* s = game::findSkill(skillId);
    const vn::SkillLearnResult r = m_skillBook.check(skillId);
    if (r != vn::SkillLearnResult::Ok)
    {
        showSkillMessage(world::learnResultText(r, s, m_skillBook.rank(skillId)), true);
        return;
    }
    if (ctx().net.account().skillSpendConfirmed)
    {
        sendSkillLearn(skillId);
        return;
    }
    // Spending is permanent: asked once per session (PROTOCOL_SKILLS.md §7.2).
    confirm("Yetenekler", std::string(world::kSkillConfirmText), "Evet", "Hayır", [this, skillId](bool yes) {
        if (!yes)
            return;
        ctx().net.account().skillSpendConfirmed = true;
        sendSkillLearn(skillId);
    });
}

void WorldScene::sendSkillLearn(std::uint16_t skillId)
{
    if (m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked())
        return; // never after LEAVE_WORLD_REQ
    if (!m_skillBook.canRaise(skillId))
        return;
    if (!skillRequestBudget())
    {
        m_deferredLearn = skillId; // sent by updateSkills() once the budget allows (the last click wins)
        VO_LOG_INFO("world: SKILL_LEARN_REQ for skill {} waits for the request budget", skillId);
        return;
    }
    const auto req = m_skillBook.beginLearn(skillId);
    if (!req)
        return;
    noteSkillRequest();
    if (!ctx().net.send(*req))
    {
        VO_LOG_WARN("world: SKILL_LEARN_REQ could not be sent");
        m_skillBook.abandonLearn();
        return;
    }
    VO_LOG_INFO("world: SKILL_LEARN_REQ seq {} skill {} (rank {} -> {})", req->seq, skillId, m_skillBook.rank(skillId),
                m_skillBook.rank(skillId) + 1);
    showSkillMessage("Kaydediliyor…", false);
}

void WorldScene::sendSkillBar(const game::SkillBar& bar)
{
    if (m_leavingWorld || m_model.leaving() || leaving() || ctx().net.movesBlocked())
        return;
    if (!m_skillBook.barBusy() && !skillRequestBudget())
    {
        m_deferredBar = bar; // sent by updateSkills() once the budget allows
        VO_LOG_INFO("world: skill bar change waits for the request budget");
        return;
    }
    const auto req = m_skillBook.requestBar(bar);
    if (req)
        noteSkillRequest();
    if (!req)
    {
        if (m_skillBook.barQueued())
            VO_LOG_INFO("world: skill bar change waits for the request in flight");
        return;
    }
    if (!ctx().net.send(*req))
    {
        VO_LOG_WARN("world: SKILL_BAR_SET_REQ could not be sent");
        m_skillBook.abandonBar();
        return;
    }
    VO_LOG_INFO("world: SKILL_BAR_SET_REQ seq {} [{} {} {} {} {} {} {} {}]", req->seq, req->slots[0], req->slots[1],
                req->slots[2], req->slots[3], req->slots[4], req->slots[5], req->slots[6], req->slots[7]);
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

bool WorldScene::handleSkillMessage(const net::NetEvent& e)
{
    const auto op = static_cast<vn::Opcode>(e.opcode);
    switch (op)
    {
    case vn::Opcode::EntitySpawn:
    case vn::Opcode::EntityDespawn:
    case vn::Opcode::EntityDeath:
    {
        // A fresh spawn gets its EFFECT_LIST right after it; death and despawn clear the list without
        // EFFECT_REMOVE (PROTOCOL_SKILLS.md §1.5). The other handlers process the message itself.
        const std::uint32_t id = peekEntityId(e.payload);
        if (id != 0)
        {
            m_effects.clear(id);
            std::erase_if(m_leaps, [id](const Leap& l) { return l.entity == id; });
        }
        return false;
    }
    case vn::Opcode::SkillState:
    case vn::Opcode::SkillLearnRes:
    case vn::Opcode::SkillBar:
    case vn::Opcode::SkillBarSetRes:
    case vn::Opcode::SkillCooldowns:
    case vn::Opcode::EffectList:
    case vn::Opcode::EffectApply:
    case vn::Opcode::EffectRemove:
    case vn::Opcode::SkillLeap:
    case vn::Opcode::SkillUsed: break;
    default: return false;
    }
    const std::uint32_t me = m_model.localEntityId();
    const auto malformed = [&]() {
        VO_LOG_ERROR("world: malformed {} payload ({} bytes)", vn::opcodeName(op), e.payload.size());
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return true;
    };
    switch (op)
    {
    case vn::Opcode::SkillState:
    {
        vn::SkillState m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_skillBook.applyState(m);
        ++m_skillsSeen.states;
        std::string ranks;
        for (const vn::SkillRankEntry& r : m.skills)
            ranks += std::format(" {}:{}", r.skillId, r.rank);
        VO_LOG_INFO("world: SKILL_STATE points {} unspent {} ranks{}", m.points, m.unspent, ranks);
        return true;
    }
    case vn::Opcode::SkillLearnRes:
    {
        vn::SkillLearnRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        const world::SkillBook::LearnAnswer a = m_skillBook.finishLearn(m);
        VO_LOG_INFO("world: SKILL_LEARN_RES seq {} skill {} {} rank {} unspent {}{}", m.seq, m.skillId,
                    vn::skillLearnResultName(m.result), m.rank, m.unspent, a.matched ? "" : " (not the request in flight)");
        const game::SkillDef* s = game::findSkill(m.skillId);
        if (m.result == vn::SkillLearnResult::Ok)
        {
            ++m_skillsSeen.learned;
            showSkillMessage(world::learnResultText(m.result, s, m.rank), false);
        }
        else
        {
            ++m_skillsSeen.learnRefused;
            showSkillMessage(world::learnResultText(m.result, s, m.rank), true);
        }
        return true;
    }
    case vn::Opcode::SkillBar:
    {
        vn::SkillBar m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_skillBook.applyBar(m);
        VO_LOG_INFO("world: SKILL_BAR [{} {} {} {} {} {} {} {}]", m.slots[0], m.slots[1], m.slots[2], m.slots[3], m.slots[4],
                    m.slots[5], m.slots[6], m.slots[7]);
        return true;
    }
    case vn::Opcode::SkillBarSetRes:
    {
        vn::SkillBarSetRes m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        const world::SkillBook::BarAnswer a = m_skillBook.finishBar(m);
        VO_LOG_INFO("world: SKILL_BAR_SET_RES seq {} result {} [{} {} {} {} {} {} {} {}]", m.seq, static_cast<int>(m.result),
                    m.slots[0], m.slots[1], m.slots[2], m.slots[3], m.slots[4], m.slots[5], m.slots[6], m.slots[7]);
        if (m.result == vn::SkillBarResult::Ok)
            ++m_skillsSeen.barSets;
        else
        {
            ++m_skillsSeen.barRefused;
            showSkillMessage(world::barResultText(m.result), true);
        }
        if (a.next)
        {
            noteSkillRequest();
            if (ctx().net.send(*a.next))
                VO_LOG_INFO("world: SKILL_BAR_SET_REQ seq {} (the change that waited)", a.next->seq);
            else
                m_skillBook.abandonBar();
        }
        return true;
    }
    case vn::Opcode::SkillCooldowns:
    {
        vn::SkillCooldowns m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        ++m_skillsSeen.cooldownLists;
        for (const vn::SkillCooldownEntry& c : m.entries)
        {
            m_skillTimers.setCooldown(c.skillId, c.remainingMs, c.totalMs, m_time);
            VO_LOG_DEBUG("world: SKILL_COOLDOWNS skill {} {}/{} ms", c.skillId, c.remainingMs, c.totalMs);
        }
        return true;
    }
    case vn::Opcode::EffectList:
    {
        vn::EffectList m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_effects.list(m.entityId, m.effects, m_time);
        VO_LOG_DEBUG("world: EFFECT_LIST {} ({} effect(s))", m.entityId, m.effects.size());
        return true;
    }
    case vn::Opcode::EffectApply:
    {
        vn::EffectApply m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        m_effects.apply(m.entityId, m.effect, m_time);
        ++m_skillsSeen.effectsApplied;
        if (m.entityId == me)
            ++m_skillsSeen.selfEffects;
        VO_LOG_INFO("world: EFFECT_APPLY {} skill {} kind {} amount {} for {} ms (source {})", m.entityId, m.effect.skillId,
                    m.effect.kind, m.effect.amount, m.effect.remainingMs, m.effect.sourceId);
        return true;
    }
    case vn::Opcode::EffectRemove:
    {
        vn::EffectRemove m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        (void)m_effects.remove(m.entityId, m.skillId, m.kind);
        VO_LOG_INFO("world: EFFECT_REMOVE {} skill {} kind {} reason {}", m.entityId, m.skillId, m.kind,
                    static_cast<int>(m.reason));
        return true;
    }
    case vn::Opcode::SkillLeap:
    {
        vn::SkillLeap m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        Vec3 to{m.x, m.y, m.z};
        if (m_terrain != nullptr)
            to.y = m_terrain->heightAt(m.x, m.z);
        Vec3 from = to;
        if (m.entityId == me)
        {
            from = localDrawPosition();
            m_model.local().teleport(to, m.yaw, m_terrain); // the POSITION_CORRECT after it is then stale
            m_route.clear();
            m_routeNext = 0;
            m_chasing = false;
        }
        else if (world::RemoteEntity* r = m_model.find(m.entityId))
        {
            from = r->position;
            r->yaw = m.yaw;
        }
        else
            return true;
        std::erase_if(m_leaps, [&](const Leap& l) { return l.entity == m.entityId; });
        m_leaps.push_back({m.entityId, from, to, 0.0f});
        ++m_skillsSeen.leaps;
        // The shadow ribbon from the take-off to the landing point.
        if (m_art.size() < kMaxArt && world::horizontalDistance(from, to) > 0.5f)
        {
            SkillArt a;
            a.mesh = "effects/leap_trail.vmesh";
            a.motion = world::ArtMotion::Trail;
            a.at = to;
            a.to = from;
            a.duration = 0.45f;
            a.opacity = 0.85f;
            m_art.push_back(std::move(a));
        }
        VO_LOG_INFO("world: SKILL_LEAP {} -> ({:.1f}, {:.1f}) {:.1f} m", m.entityId, m.x, m.z,
                    world::horizontalDistance(from, to));
        return true;
    }
    case vn::Opcode::SkillUsed:
    {
        vn::SkillUsed m;
        if (!vn::decodeMessage(e.payload, m))
            return malformed();
        const game::SkillDef* s = game::findSkill(m.skillId);
        if (s == nullptr)
            return true;
        // The release / swing (a cast skill's release came with CAST_END).
        const bool swing = s->hitsEnemies() && !s->projectile && s->area != game::SkillArea::AroundTarget;
        const game::ItemCatalog* catalog = ctx().resources.itemData() != nullptr && ctx().resources.itemData()->catalogLoaded
                                               ? &ctx().resources.itemData()->catalog
                                               : nullptr;
        if (m.casterId == me)
        {
            ++m_skillsSeen.ownUses;
            m_lastUsedSkill = m.skillId;
            m_lastUseHits = 0;
            m_lastUseTime = m_time;
            m_skillTimers.onSkillUsed(m.skillId, m_time, s->at(m.rank).cooldownMs);
            if (s->castMs == 0 && !m_self.dead)
            {
                const game::ItemTemplate* weapon =
                    catalog != nullptr && localEquipment().mainHand != 0 ? catalog->find(localEquipment().mainHand) : nullptr;
                const world::AnimState clip = swing || !m_localAnim.has(world::AnimState::SpellShoot)
                                                  ? world::attackClipFor(weapon, false, m_localSwings++)
                                                  : world::AnimState::SpellShoot;
                (void)m_localAnim.playAttack(clip);
            }
            // Face a target we used it on (cosmetic; the server checks the range only).
            Vec3 at{};
            if (m.targetId != me && entityFeet(m.targetId, at) && m_leaps.empty())
            {
                world::LocalMover& mover = m_model.local().mover();
                if (world::horizontalDistance(at, mover.position()) > 0.3f && !mover.moving())
                    mover.setPosition(mover.position(), std::atan2(at.x - mover.position().x, at.z - mover.position().z),
                                      m_terrain);
            }
            VO_LOG_INFO("world: SKILL_USED {} ({} rank {}) on {}", m.skillId, s->name, m.rank, m.targetId);
        }
        else if (world::RemoteEntity* r = m_model.find(m.casterId))
        {
            ++m_skillsSeen.otherUses;
            r->facing = m.targetId;
            r->facingUntil = r->age + 1.5f;
            if (s->castMs == 0 && !r->casting && !r->isMonster())
            {
                const game::ItemTemplate* weapon = catalog != nullptr && r->mainHand != 0 ? catalog->find(r->mainHand) : nullptr;
                const world::AnimState clip = swing || !r->animator.has(world::AnimState::SpellShoot)
                                                  ? world::attackClipFor(weapon, false, r->swings++)
                                                  : world::AnimState::SpellShoot;
                if (r->animator.playAttack(clip))
                    ++m_seen.remoteAttackClips;
            }
        }
        spawnSkillArt(m);
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------------------------

world::Vec3 WorldScene::localDrawPosition() const
{
    const std::uint32_t me = m_model.localEntityId();
    for (const Leap& l : m_leaps)
    {
        if (l.entity != me)
            continue;
        const float u = std::clamp(l.age / kLeapTime, 0.0f, 1.0f);
        const float e = u * u * (3.0f - 2.0f * u);
        return assets::lerp(l.from, m_model.local().visualPosition(), e) +
               Vec3{0.0f, kLeapArc * std::sin(u * world::kPi), 0.0f};
    }
    return m_model.local().visualPosition();
}

bool WorldScene::entityFeet(std::uint32_t id, Vec3& out) const
{
    if (id == 0)
        return false;
    if (id == m_model.localEntityId())
    {
        out = localDrawPosition();
        return true;
    }
    if (const world::RemoteEntity* e = const_cast<world::WorldModel&>(m_model).find(id))
    {
        out = e->position;
        return true;
    }
    return false;
}

float WorldScene::entityHeight(std::uint32_t id) const
{
    if (id == m_model.localEntityId())
    {
        render::AssetCache& cache = *ctx().assets;
        const render::GpuMesh* mesh = cache.mesh(cache.characterMeshPath(m_race, m_look));
        return mesh != nullptr && mesh->cpu ? mesh->cpu->boundsMax.y : 1.8f;
    }
    if (const world::RemoteEntity* e = const_cast<world::WorldModel&>(m_model).find(id))
        return headPoint(*e).y - e->position.y;
    return 1.8f;
}

Rect WorldScene::targetFrameRect() const
{
    const float s = ui().scale();
    const float w = ctx().viewportWidth;
    const auto px = [s](float v) { return std::round(v * s); };
    // Between the player frame and the zone name (fixed places: the frame does not move when a window
    // hides those pieces).
    const float left = px(world::kHudPlayerFrame.right() + 12.0f);
    const render::Font& zf = ctx().fonts.get(render::FontWeight::Bold, 20.0f * s);
    const float right = w - px(18.0f) - render::measureText(zf, m_zoneName) - px(14.0f);
    float fw = std::min(px(300.0f), right - left);
    fw = std::max(fw, px(150.0f));
    float fx = std::round((w - fw) * 0.5f);
    fx = std::clamp(fx, left, std::max(left, right - fw));
    return {fx, px(12.0f), fw, px(66.0f)};
}

// ---------------------------------------------------------------------------------------------
// Effect art
// ---------------------------------------------------------------------------------------------

void WorldScene::spawnSkillArt(const vn::SkillUsed& m)
{
    const game::SkillDef* s = game::findSkill(m.skillId);
    if (s == nullptr)
        return;
    for (const world::ArtSpawn& a : world::skillUseArt(*s, m.rank))
    {
        const std::uint32_t anchorId = a.at == world::ArtAt::Caster ? m.casterId : m.targetId;
        Vec3 p{};
        if (!entityFeet(anchorId, p))
            continue;
        if (a.motion == world::ArtMotion::Projectile)
        {
            Vec3 from{};
            if (!entityFeet(m.casterId, from) || m.targetId == m.casterId)
                continue;
            float yaw = 0.0f;
            if (m.casterId == m_model.localEntityId())
                yaw = m_model.local().mover().yaw();
            else if (const world::RemoteEntity* c = m_model.find(m.casterId))
                yaw = c->yaw;
            from = from + world::forwardFromYaw(yaw) * 0.45f + Vec3{0.0f, 1.35f, 0.0f};
            const Vec3 to = p + Vec3{0.0f, 1.0f, 0.0f};
            if (m_bolts.size() < 8)
            {
                Bolt b;
                b.target = m.targetId;
                b.from = from;
                b.to = to;
                b.mesh = a.mesh;
                b.duration = a.duration;
                b.scale = a.scale;
                m_bolts.push_back(b);
                ++m_boltsLaunched;
            }
            continue;
        }
        const int count = a.motion == world::ArtMotion::Fall ? std::max(1, a.count) : 1;
        for (int i = 0; i < count && m_art.size() < kMaxArt; ++i)
        {
            SkillArt x;
            x.mesh = std::format("effects/{}.vmesh", a.mesh);
            x.motion = a.motion;
            x.duration = a.duration;
            x.scale = a.scale;
            x.opacity = a.opacity;
            x.tint = a.tint;
            x.tintEmissive = neutralMesh(a.mesh);
            x.seed = hash01(m_artSeed++);
            x.at = p;
            switch (a.motion)
            {
            case world::ArtMotion::Burst:
            case world::ArtMotion::Strike:
            case world::ArtMotion::Wisp: x.entity = anchorId; break;
            case world::ArtMotion::Fall:
            {
                // Motes over the area, staggered over the first 60 % of the effect.
                const float ang = hash01(m_artSeed++) * world::kTwoPi;
                const float rad = std::sqrt(hash01(m_artSeed++)) * a.scale * 0.85f;
                x.at = p + Vec3{std::sin(ang) * rad, 0.0f, std::cos(ang) * rad};
                x.delay = a.duration * 0.6f * static_cast<float>(i) / static_cast<float>(count);
                x.duration = 0.55f + 0.2f * hash01(m_artSeed++);
                x.scale = 1.0f + 0.6f * hash01(m_artSeed++); // a little over the manifest's 0.8-1.3: read at range
                break;
            }
            default: break; // rings, clouds: the fixed point
            }
            m_art.push_back(std::move(x));
        }
    }
}

void WorldScene::addSkillDraws()
{
    render::WorldRenderer& r = *ctx().worldRenderer;
    render::AssetCache& cache = *ctx().assets;
    m_artCounts.clear();
    const auto count = [&](std::string_view mesh) {
        const std::string_view name = mesh.substr(mesh.find('/') + 1, mesh.size() - mesh.find('/') - 7);
        for (auto& [n, c] : m_artCounts)
            if (n == name)
            {
                ++c;
                return;
            }
        m_artCounts.emplace_back(std::string(name), 1u);
    };
    const auto draw = [&](const std::string& mesh, const world::Mat4& w, std::array<float, 3> tint, float opacity,
                          bool tintEmissive) {
        const render::GpuMesh* g = cache.mesh(mesh);
        if (g == nullptr || opacity <= 0.0f)
            return;
        r.addEffectMesh(g, w, tint, opacity, tintEmissive);
        count(mesh);
    };
    const auto ground = [&](Vec3 p) {
        return Vec3{p.x, (m_terrain != nullptr ? m_terrain->heightAt(p.x, p.z) : p.y) + 0.03f, p.z};
    };
    // Rings and clouds lie on the ground: tilted to the terrain's slope at their centre.
    const auto normal = [&](Vec3 p) { return m_terrain != nullptr ? m_terrain->normalAt(p.x, p.z) : Vec3{0.0f, 1.0f, 0.0f}; };
    for (const Bolt& b : m_bolts)
        count(std::format("effects/{}.vmesh", b.mesh)); // drawn by addCombatDraws

    for (const SkillArt& a : m_art)
    {
        const float t = a.age - a.delay;
        if (t < 0.0f)
            continue;
        const float u = std::clamp(t / std::max(a.duration, 1e-3f), 0.0f, 1.0f);
        Vec3 feet = a.at;
        if (a.entity != 0 && !entityFeet(a.entity, feet))
            continue;
        const float fadeOut = std::clamp((a.duration - t) / 0.3f, 0.0f, 1.0f);
        switch (a.motion)
        {
        case world::ArtMotion::Ring:
        {
            const float grow = std::min(1.0f, t / 0.15f);
            const float fade = std::clamp((a.duration - t) / 0.25f, 0.0f, 1.0f);
            draw(a.mesh, groundMatrix(ground(feet), normal(feet), t * 0.35f + a.seed * 6.0f, a.scale * grow, 1.0f, a.scale * grow), a.tint,
                 a.opacity * fade, a.tintEmissive);
            break;
        }
        case world::ArtMotion::Cloud:
        {
            const float fade = std::min(std::clamp(t / 0.3f, 0.0f, 1.0f), std::clamp((a.duration - t) / 0.6f, 0.0f, 1.0f));
            // XZ to about the radius; Y raised so the puffs stay round (r 1 m, 0.7 m high).
            const float k = a.scale * 0.75f * (0.85f + 0.15f * std::min(1.0f, t / 0.4f));
            draw(a.mesh, groundMatrix(ground(feet), normal(feet), t * 0.15f + a.seed * 6.0f, k, 1.5f, k), a.tint,
                 a.opacity * fade, a.tintEmissive);
            break;
        }
        case world::ArtMotion::Strike:
        {
            const float y = std::min(1.0f, t / 0.08f);
            const float xz = t < 0.2f ? 1.0f : std::max(0.0f, 1.0f - (t - 0.2f) / 0.25f);
            if (xz > 0.0f)
                draw(a.mesh, spinMatrix(ground(feet), a.seed * 6.0f, a.scale * xz, a.scale * y, a.scale * xz), a.tint,
                     a.opacity, a.tintEmissive);
            break;
        }
        case world::ArtMotion::Fall:
        {
            const Vec3 dir = assets::normalize(Vec3{(a.seed - 0.5f) * 0.6f, -1.0f, (hash01(static_cast<std::uint32_t>(a.seed * 9973.0f)) - 0.5f) * 0.6f});
            const float height = 7.0f;
            const Vec3 land = ground(feet);
            const Vec3 start = land - dir * (height / -dir.y);
            draw(a.mesh, aimMatrix(assets::lerp(start, land, u), dir, a.scale, a.scale, a.scale), a.tint, a.opacity,
                 a.tintEmissive);
            break;
        }
        case world::ArtMotion::Burst:
        {
            const float fade = std::min(std::clamp(t / 0.12f, 0.0f, 1.0f), std::clamp((a.duration - t) / 0.4f, 0.0f, 1.0f));
            draw(a.mesh, spinMatrix(feet, t * 0.8f, a.scale, a.scale, a.scale), a.tint, a.opacity * fade, a.tintEmissive);
            break;
        }
        case world::ArtMotion::Wisp:
        {
            const float side = (a.seed - 0.5f) * 0.4f;
            const Vec3 base = feet + Vec3{side, 1.2f * std::min(entityHeight(a.entity) / 1.8f, 1.6f) - 0.1f + 0.4f * u,
                                          (hash01(static_cast<std::uint32_t>(a.seed * 7919.0f)) - 0.5f) * 0.4f};
            const float k = a.scale * (1.0f - 0.7f * u);
            draw(a.mesh, spinMatrix(base, a.seed * 6.0f, k, k, k), a.tint, a.opacity, a.tintEmissive);
            break;
        }
        case world::ArtMotion::Trail:
        {
            const Vec3 dir = a.at - a.to;
            const float dist = world::horizontalDistance(a.at, a.to);
            const float xy = 1.0f - u;
            draw(a.mesh, aimMatrix(a.at + Vec3{0.0f, 0.9f, 0.0f}, Vec3{dir.x, 0.0f, dir.z}, xy, xy, dist), a.tint,
                 a.opacity * fadeOut, a.tintEmissive);
            break;
        }
        case world::ArtMotion::Projectile: break; // m_bolts
        }
    }

    // While an effect lasts: stun stars over the head, root spikes, shield bubbles, a faint buff aura.
    for (const std::uint32_t id : m_effects.entities())
    {
        Vec3 feet{};
        if (!entityFeet(id, feet))
            continue;
        float scale = 1.0f;
        if (id != m_model.localEntityId())
            if (const world::RemoteEntity* e = m_model.find(id))
            {
                if (e->dead())
                    continue;
                scale = e->isMonster() ? std::max(e->scale, 0.8f) : (e->race == static_cast<std::uint8_t>(game::Race::Kargan) ? 1.1f : 1.0f);
            }
        if (id == m_model.localEntityId() && m_race == static_cast<std::uint8_t>(game::Race::Kargan))
            scale = 1.1f;
        std::array<std::string_view, 8> drawn{};
        std::size_t drawnCount = 0;
        for (const world::ShownEffect& fx : m_effects.of(id))
        {
            const auto aura = world::effectAura(fx.skillId, fx.kind);
            if (!aura || std::find(drawn.begin(), drawn.begin() + drawnCount, aura->mesh) != drawn.begin() + drawnCount)
                continue;
            if (drawnCount < drawn.size())
                drawn[drawnCount++] = aura->mesh;
            const float left = static_cast<float>(world::effectRemaining(fx, m_time));
            const float fade = std::clamp(left / 0.3f, 0.0f, 1.0f);
            Vec3 at = feet;
            if (aura->at == world::AuraAt::Head)
                at = feet + Vec3{0.0f, entityHeight(id) + 0.25f, 0.0f};
            const float k = aura->scale * scale * (aura->opacity >= 1.0f ? fade : 1.0f);
            draw(std::format("effects/{}.vmesh", aura->mesh),
                 spinMatrix(at, static_cast<float>(m_time) * aura->spin + static_cast<float>(id % 7), k, k, k),
                 {1.0f, 1.0f, 1.0f}, aura->opacity * (aura->opacity < 1.0f ? fade : 1.0f), false);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------

bool WorldScene::skillRequestBudget() const
{
    std::size_t recent = 0;
    for (const double t : m_skillSends)
        recent += (m_time - t) < 1.05 ? 1 : 0;
    return recent < m_skillSends.size();
}

void WorldScene::noteSkillRequest()
{
    m_skillSends[m_skillSendNext] = m_time;
    m_skillSendNext = (m_skillSendNext + 1) % m_skillSends.size();
}

void WorldScene::updateSkills(float dt)
{
    m_effects.expire(m_time);
    // Requests that waited for the budget.
    if (m_deferredLearn != 0 && skillRequestBudget())
    {
        const std::uint16_t id = std::exchange(m_deferredLearn, std::uint16_t{0});
        sendSkillLearn(id);
    }
    if (m_deferredBar && skillRequestBudget())
    {
        const game::SkillBar bar = *m_deferredBar;
        m_deferredBar.reset();
        sendSkillBar(bar);
    }
    for (SkillArt& a : m_art)
        a.age += dt;
    std::erase_if(m_art, [](const SkillArt& a) { return a.age >= a.delay + a.duration; });
    for (Leap& l : m_leaps)
    {
        l.age += dt;
        if (l.entity == m_model.localEntityId())
            continue;
        // A remote leaper glides from its take-off point (its motion takes over afterwards).
        if (world::RemoteEntity* e = m_model.find(l.entity); e != nullptr && l.age < kLeapTime)
        {
            const float u = std::clamp(l.age / kLeapTime, 0.0f, 1.0f);
            const float s = u * u * (3.0f - 2.0f * u);
            e->position = assets::lerp(l.from, l.to, s) + Vec3{0.0f, kLeapArc * std::sin(u * world::kPi), 0.0f};
        }
    }
    std::erase_if(m_leaps, [](const Leap& l) { return l.age >= kLeapTime; });

    // The target's effect row under its frame.
    if (m_targetEffects != nullptr)
    {
        const bool show = m_phase == Phase::Ready && m_target != 0 && !m_yield.target;
        if (show)
        {
            const Rect f = targetFrameRect();
            const float s = std::max(ui().scale(), 0.01f);
            const Insets want{std::round(f.x / s), std::round(f.bottom() / s) + 5.0f, 0.0f, 0.0f};
            const Insets& m = m_targetEffects->layoutSpec().margins;
            if (m.left != want.left || m.top != want.top)
                m_targetEffects->setMargins(want);
        }
        if (m_targetEffects->isVisible() != show)
            m_targetEffects->setVisible(show);
    }
    if (m_skillsOpen && m_skillWindow != nullptr && m_skillBook.level() != 0)
        m_skillWindow->setClassLine(describeCharacter(m_race, m_charClass, m_self.level));
    if (m_skillDrag.active && !ctx().input.isMouseDown(platform::MouseButton::Left) && !ctx().scripted)
        m_skillDrag = {}; // the button went up somewhere the slot did not see
    if (m_pulling)
        updatePull();
}

void WorldScene::updatePull()
{
    if (m_self.dead || m_time < m_pullNext)
        return;
    m_pullNext = m_time + 0.5;
    const Vec3 me = m_model.local().mover().position();
    std::uint32_t chasing = 0;
    const world::RemoteEntity* best = nullptr;
    float bestD = 1e9f;
    const world::ItemData* data = ctx().resources.itemData();
    for (const world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active || !e.isMonster() || e.dead() || e.name != m_pullName)
            continue;
        const float d = world::horizontalDistance(e.position, me);
        if ((e.state & vn::kEntityStateInCombat) != 0)
        {
            if (d < 25.0f)
                ++chasing;
            continue;
        }
        if (data != nullptr && data->monstersLoaded &&
            data->monsters.inSafeZone(m_model.zoneId() != 0 ? m_model.zoneId() : game::kStartZoneId, e.position.x, e.position.z))
            continue;
        if (d < bestD)
        {
            bestD = d;
            best = &e;
        }
    }
    if (chasing >= m_pullWant)
    {
        VO_LOG_INFO("world: pullpack: {} '{}' chasing us", chasing, m_pullName);
        m_pulling = false;
        m_pullDone = true;
        m_route.clear();
        m_routeNext = 0;
        m_model.local().mover().clearDestination();
        return;
    }
    if (best == nullptr || bestD < 2.5f)
        return;
    std::string ignored;
    (void)setRouteNear(me, best->position.x, best->position.z, 2.5f, ignored);
}

// ---------------------------------------------------------------------------------------------
// Overlay: tooltips and the dragged skill
// ---------------------------------------------------------------------------------------------

void WorldScene::drawSkillOverlay()
{
    if (m_phase != Phase::Ready)
        return;
    ui::DrawContext dc{ctx().batch, ctx().fonts, theme(), ui().scale(), m_time};
    std::string skillTip;
    std::string effectTip;
    std::string hudTip;
    if (m_skillDrag.active)
    {
        if (const game::SkillDef* s = game::findSkill(m_skillDrag.skill))
            if (const render::Texture2D* icon = skillImage(world::skillIconPath(*s)))
            {
                const float size = dc.px(44.0f);
                ctx().batch.drawImage(*icon, Rect{m_skillDrag.x - size * 0.5f, m_skillDrag.y - size * 0.5f, size, size}.snapped(),
                                      Color{1.0f, 1.0f, 1.0f, 0.85f});
            }
    }
    else if (!ui().hasModal())
    {
        ui::Widget* hover = ui().hover();
        std::vector<ShownTipLine> lines;
        const game::SkillDef* skill = nullptr;
        bool above = false; // bar slots: the box over the bar
        if (const auto* slot = dynamic_cast<const SkillSlotWidget*>(hover))
        {
            skill = game::findSkill(m_skillBook.slot(slot->index()));
            above = true;
        }
        else if (const auto* cell = dynamic_cast<const SkillCellWidget*>(hover))
            skill = &cell->skill();
        if (skill != nullptr)
        {
            lines = shownTipLines(world::skillTipLines(*skill, m_skillBook.rank(skill->id)), theme());
            skillTip = tooltipPlainText(lines);
        }
        else if (const auto* icon = dynamic_cast<const EffectIconWidget*>(hover); icon != nullptr && icon->effect() != nullptr)
        {
            const world::ShownEffect& fx = *icon->effect();
            const game::SkillDef* s = game::findSkill(fx.skillId);
            lines.push_back({s != nullptr ? s->name : std::string("Etki"), theme().textTitle, true, false});
            lines.push_back({world::effectKindLine(fx.kind, fx.amount, fx.skillId),
                             world::effectHelpful(fx.kind) ? Color::hex(0x9FD27A) : Color::hex(0xE5736A), false, false});
            const std::string left = world::effectSecondsText(world::effectRemaining(fx, m_time));
            if (!left.empty())
                lines.push_back({std::format("{} sn kaldı", left), theme().textDim, false, false});
            effectTip = world::effectTipText(fx);
        }
        else
        {
            // HUD buttons and the experience bar (HudView.h texts), over the bar.
            std::vector<world::HudTipLine> tip;
            if (const auto* hb = dynamic_cast<const HudIconButton*>(hover))
                tip = world::hudButtonTipLines(hb->kind(), hudButtonState(hb->kind()));
            else if (hover != nullptr && hover == m_hudBar &&
                     m_hudBar->xpRect().inset(-2.0f).contains(static_cast<float>(ctx().input.mouseX()),
                                                              static_cast<float>(ctx().input.mouseY())))
                tip = world::xpTipLines(m_self.level, m_self.xp, m_self.xpToNext);
            for (const world::HudTipLine& l : tip)
            {
                Color c = theme().text;
                switch (l.tone)
                {
                case world::HudTipLine::Tone::Title: c = theme().textTitle; break;
                case world::HudTipLine::Tone::State: c = Color::hex(0xA9D6F5); break;
                case world::HudTipLine::Tone::Points: c = Color::hex(0xF5C84C); break;
                case world::HudTipLine::Tone::Dim: c = theme().textDim; break;
                case world::HudTipLine::Tone::Text: break;
                }
                lines.push_back({l.text, c, l.tone == world::HudTipLine::Tone::Title, false});
            }
            if (!tip.empty())
            {
                hudTip = world::hudTipPlainText(tip);
                above = true;
                if (hover == m_skillsButton)
                    skillTip = hudTip; // (M12 flows read the Yetenekler button's tooltip as skills.tooltip)
            }
        }
        if (!lines.empty())
            drawTooltipBox(dc, lines, hover->rect(), ctx().viewportWidth, ctx().viewportHeight, above);
    }
    if (m_hudTipProbe != nullptr)
        m_hudTipProbe->setText(hudTip);
    if (m_skillTipProbe != nullptr)
        m_skillTipProbe->setText(skillTip);
    if (m_effectTipProbe != nullptr)
        m_effectTipProbe->setText(effectTip);
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::skillsStates(std::vector<std::string>& out) const
{
    out.emplace_back(m_skillsOpen ? "skillsopen" : "skillsclosed");
    if (m_skillBook.known())
    {
        out.emplace_back("skillsknown");
        out.emplace_back("skillpoints" + std::to_string(m_skillBook.unspent()));
    }
    if (m_skillsButton != nullptr && m_skillsButton->markerShown())
        out.emplace_back("skillmarker");
    out.emplace_back(m_skillBook.learnBusy() || m_deferredLearn != 0 ? "learnbusy" : "learnidle");
    bool canRaise = false; // a "+" is offered somewhere
    for (const game::SkillDef& s : game::skills().skills())
        canRaise = canRaise || (s.charClass == m_charClass && m_skillBook.known() &&
                                m_skillBook.check(s.id) == vn::SkillLearnResult::Ok);
    out.emplace_back(canRaise ? "canraise" : "noraise");
    out.emplace_back(m_skillBook.barBusy() || m_skillBook.barQueued() || m_deferredBar ? "barbusy" : "baridle");
    if (m_levelUpAge < 3.2f && m_levelUpSkillPoints > 0)
        out.emplace_back("skilllevelup"); // the level-up toast shows the skill points line
    // The level-up toast ends above the own name label (the M11 overlap).
    if (m_levelUpToastBottom > 0.0f && (m_localLabelTop < 0.0f || m_levelUpToastBottom <= m_localLabelTop))
        out.emplace_back("toastabovelabel");
    // Tier gates of the window: locked<branch>_<tier> / unlocked<branch>_<tier>.
    for (std::uint8_t br = 0; br < 2; ++br)
        for (std::uint8_t tier = 2; tier <= 3; ++tier)
        {
            const bool locked = m_skillBook.branchSpent(br) < game::skills().rules().tierPoints[tier - 1u];
            out.emplace_back(std::format("{}{}_{}", locked ? "locked" : "unlocked", br, tier));
        }
    if (m_skillDrag.active)
        out.emplace_back("skilldrag");
    if (m_pulling)
        out.emplace_back("pulling");
    if (m_pullDone)
        out.emplace_back("pulled");
    if (m_skillTimers.globalActive(m_time))
        out.emplace_back("gcd");
    // The bar's skills: ready_<key> (own and global cooldown over) / cd_<key> (a relog keeps cooldowns 60 s).
    for (std::size_t i = 0; i < game::kSkillBarSlots; ++i)
        if (const game::SkillDef* s = game::findSkill(m_skillBook.slot(i)))
            out.emplace_back((m_skillTimers.ready(s->id, m_time) ? "ready_" : "cd_") + s->key);
    for (const Leap& l : m_leaps)
        if (l.entity == m_model.localEntityId())
            out.emplace_back("leaping");
    for (const world::ShownEffect& e : m_effects.of(m_model.localEntityId()))
        out.emplace_back("selfeffect_" + std::string(game::effectKindKey(static_cast<game::EffectKind>(e.kind))));
    if (m_target != 0)
        for (const world::ShownEffect& e : m_effects.of(m_target))
            out.emplace_back("targeteffect_" + std::string(game::effectKindKey(static_cast<game::EffectKind>(e.kind))));
    for (const auto& [name, n] : m_artCounts)
        out.emplace_back("art_" + name);
    const auto counter = [&out](const char* name, std::uint32_t n) {
        for (std::uint32_t i = 1; i <= std::min<std::uint32_t>(n, 12); ++i)
            out.emplace_back(name + std::to_string(i));
    };
    counter("skillstates", m_skillsSeen.states);
    counter("learned", m_skillsSeen.learned);
    counter("learnrefused", m_skillsSeen.learnRefused);
    counter("barsets", m_skillsSeen.barSets);
    counter("barrefused", m_skillsSeen.barRefused);
    counter("skilluses", m_skillsSeen.ownUses);
    counter("otheruses", m_skillsSeen.otherUses);
    counter("selfeffects", m_skillsSeen.selfEffects);
    counter("effectsapplied", m_skillsSeen.effectsApplied);
    counter("effectticks", m_skillsSeen.ticks);
    counter("absorbs", m_skillsSeen.absorbed);
    counter("leaps", m_skillsSeen.leaps);
    counter("cooldownlists", m_skillsSeen.cooldownLists);
    counter("usehits", m_lastUseHits);
    std::uint32_t close = 0;
    const Vec3 me = m_model.local().mover().position();
    for (const world::RemoteEntity& e : m_model.entities())
        if (e.active && e.isMonster() && !e.dead() && world::horizontalDistance(e.position, me) <= 3.0f)
            ++close;
    counter("closemonsters", close);
}

bool WorldScene::skillsAutomation(std::string_view command, bool& handled, std::string& error)
{
    handled = false;
    std::string parseError;
    const auto cmd = world::parseSkillsCommand(command, parseError);
    if (!cmd)
    {
        if (parseError.empty())
            return true; // not a skill command
        handled = true;
        error = parseError;
        return false;
    }
    handled = true;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    const std::uint32_t me = m_model.localEntityId();
    const auto entityOf = [&](const std::string& mode) { return mode == "self" ? me : m_target; };
    switch (cmd->verb)
    {
    case world::SkillsVerb::Window:
        setSkillsOpen(cmd->mode == "open");
        if (m_skillsOpen != (cmd->mode == "open"))
            return fail("the Yetenekler window cannot be " + std::string(cmd->mode == "open" ? "opened" : "closed") + " now");
        return true;
    case world::SkillsVerb::Learn:
        if (!m_skillBook.canRaise(cmd->skill->id))
            return fail(std::format("skilllearn {}: cannot be raised now ({})", cmd->skill->key,
                                    vn::skillLearnResultName(m_skillBook.check(cmd->skill->id))));
        raiseSkill(cmd->skill->id);
        return true;
    case world::SkillsVerb::BarSet:
        sendSkillBar(world::barWithPlaced(m_skillBook.bar(), cmd->a - 1, cmd->skill != nullptr ? cmd->skill->id : 0));
        return true;
    case world::SkillsVerb::UseSlot:
        useBarSlot(cmd->a - 1);
        return true;
    case world::SkillsVerb::ExpectRank:
        if (m_skillBook.rank(cmd->skill->id) != cmd->a)
            return fail(std::format("{} is rank {}, expected {}", cmd->skill->key, m_skillBook.rank(cmd->skill->id), cmd->a));
        return true;
    case world::SkillsVerb::ExpectPoints:
        if (!m_skillBook.known())
            return fail("expectskillpoints: no SKILL_STATE yet");
        if (m_skillBook.unspent() != cmd->a)
            return fail(std::format("{} unspent skill point(s), expected {}", m_skillBook.unspent(), cmd->a));
        return true;
    case world::SkillsVerb::ExpectSlot:
    {
        const std::uint16_t have = m_skillBook.slot(cmd->a - 1);
        const std::uint16_t want = cmd->skill != nullptr ? cmd->skill->id : 0;
        if (have != want)
            return fail(std::format("slot {} holds {}, expected {}", cmd->a, have, want));
        return true;
    }
    case world::SkillsVerb::ExpectSlotLook:
    {
        const SlotView v = slotView(cmd->a - 1);
        if (world::slotLookName(v.look) != cmd->mode)
            return fail(std::format("slot {} looks '{}', expected '{}'", cmd->a, world::slotLookName(v.look), cmd->mode));
        return true;
    }
    case world::SkillsVerb::ExpectCooldown:
    {
        const double left = m_skillTimers.ownRemaining(cmd->skill->id, m_time);
        if (left < cmd->lo - 1e-3 || left > cmd->hi + 1e-3)
            return fail(std::format("{} cooldown {:.2f} s, expected {:.2f}..{:.2f}", cmd->skill->key, left, cmd->lo, cmd->hi));
        return true;
    }
    case world::SkillsVerb::ExpectEffect:
    case world::SkillsVerb::ExpectNoEffect:
    {
        const std::uint32_t id = entityOf(cmd->mode);
        const std::uint16_t skill = cmd->skill != nullptr ? cmd->skill->id : 0;
        const bool has = id != 0 && m_effects.has(id, cmd->kind, skill);
        if (cmd->verb == world::SkillsVerb::ExpectNoEffect)
        {
            if (has)
                return fail(std::format("{} still has effect kind {}", cmd->mode, cmd->kind));
            return true;
        }
        if (!has)
            return fail(std::format("{} ({}) has no effect kind {}{} ({} effect(s))", cmd->mode, id, cmd->kind,
                                    skill != 0 ? std::format(" of skill {}", skill) : std::string(), m_effects.of(id).size()));
        // ... and its HUD row shows an icon for it.
        const auto list = m_effects.of(id);
        const EffectRowWidget* row = cmd->mode == "self" || id == me ? m_selfEffects : m_targetEffects;
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].kind == cmd->kind && (skill == 0 || list[i].skillId == skill))
            {
                const std::string name = std::format("{}.{}", row == m_selfEffects ? "effects.self" : "effects.target", i);
                const ui::Widget* w = row != nullptr ? ui().findByName(name) : nullptr;
                if (row == nullptr || !row->isVisible() || w == nullptr || !w->isVisible())
                    return fail(std::format("effect kind {} of {} is known but its icon {} is not shown", cmd->kind,
                                            cmd->mode, name));
            }
        return true;
    }
    case world::SkillsVerb::ExpectEffectTip:
    {
        const std::uint32_t id = entityOf(cmd->mode);
        const world::ShownEffect* fx = id != 0 ? m_effects.find(id, cmd->kind) : nullptr;
        if (fx == nullptr)
            return fail(std::format("{} has no effect kind {}", cmd->mode, cmd->kind));
        const std::string tip = world::effectTipText(*fx);
        if (tip.find(cmd->text) == std::string::npos)
            return fail(std::format("effect tooltip is '{}', expected '{}'", tip, cmd->text));
        return true;
    }
    case world::SkillsVerb::ExpectArt:
    {
        std::uint32_t n = 0;
        for (const auto& [name, c] : m_artCounts)
            if (name == cmd->text)
                n = c;
        if (n < cmd->a)
            return fail(std::format("{} instance(s) of effects/{} drawn last frame, expected at least {}", n, cmd->text, cmd->a));
        return true;
    }
    case world::SkillsVerb::ExpectMessage:
        if (m_lastSkillMessage.find(cmd->text) == std::string::npos)
            return fail(std::format("last skill message is '{}', expected '{}'", m_lastSkillMessage, cmd->text));
        return true;
    case world::SkillsVerb::PullPack:
        m_pullName = cmd->text;
        m_pullWant = cmd->a;
        m_pulling = true;
        m_pullDone = false;
        m_pullNext = 0.0;
        VO_LOG_INFO("world: pullpack: {} x '{}'", m_pullWant, m_pullName);
        return true;
    case world::SkillsVerb::Log:
    {
        std::string ranks;
        for (const game::SkillDef& s : game::skills().skills())
            if (s.charClass == m_charClass)
                ranks += std::format(" {}={}", s.key, m_skillBook.rank(s.id));
        const game::SkillBar& b = m_skillBook.bar();
        VO_LOG_INFO("world: skills: known {} points {} unspent {} spent {}/{}; bar [{} {} {} {} {} {} {} {}];{}",
                    m_skillBook.known(), m_skillBook.points(), m_skillBook.unspent(), m_skillBook.branchSpent(0),
                    m_skillBook.branchSpent(1), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], ranks);
        for (const std::uint32_t id : m_effects.entities())
            for (const world::ShownEffect& e : m_effects.of(id))
                VO_LOG_INFO("world:   effect on {}: skill {} kind {} amount {} {:.1f} s left", id, e.skillId, e.kind, e.amount,
                            world::effectRemaining(e, m_time));
        for (const auto& [name, n] : m_artCounts)
            VO_LOG_INFO("world:   art {} x{}", name, n);
        return true;
    }
    }
    return true;
}

} // namespace vo::client::scenes
