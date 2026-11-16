// M9 "İlk Kan" client logic (client/world/CombatView.h, the Animator's combat actions, WorldModel's
// monster spawns): name label colours, picking, Tab targeting, attack clip choice, the skill
// cooldown / cast state machine, floating numbers, ACTION_FAIL texts and the combat automation
// command parser (including its error messages).
#include "client/world/Animator.h"
#include "client/world/CombatView.h"
#include "client/world/WorldModel.h"
#include "shared/game/Combat.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>
#include <vector>

using Catch::Approx;
namespace world = vo::client::world;
namespace assets = vo::assets;
namespace game = vo::game;
namespace vn = vo::net;
using world::AnimAction;
using world::AnimState;
using world::Vec3;

TEST_CASE("Combat view: label colour by level difference", "[client][combat]")
{
    using world::LevelTint;
    CHECK(world::levelTint(5, 1) == LevelTint::Trivial); // 4 below
    CHECK(world::levelTint(5, 2) == LevelTint::Trivial); // 3 below: reduced experience
    CHECK(world::levelTint(5, 3) == LevelTint::Even);
    CHECK(world::levelTint(5, 5) == LevelTint::Even);
    CHECK(world::levelTint(1, 2) == LevelTint::Higher);
    CHECK(world::levelTint(1, 3) == LevelTint::Higher);
    CHECK(world::levelTint(1, 4) == LevelTint::Hard);
    CHECK(world::levelTint(1, 5) == LevelTint::Hard);
    CHECK(world::levelTint(1, 6) == LevelTint::Deadly); // the Kor Bekçisi for a new character
    CHECK(world::levelTint(10, 1) == LevelTint::Trivial);
    // Distinct colours, white for an even fight, red for a deadly one.
    std::set<std::uint32_t> colours;
    for (const LevelTint t : {LevelTint::Trivial, LevelTint::Even, LevelTint::Higher, LevelTint::Hard, LevelTint::Deadly})
        colours.insert(world::levelTintRgb(t));
    CHECK(colours.size() == 5);
    CHECK(world::levelTintRgb(LevelTint::Even) == 0xF4F1E8);
    CHECK(std::string(world::levelTintName(LevelTint::Deadly)) == "red");
}

TEST_CASE("Combat view: ray picking against vertical capsules", "[client][combat]")
{
    // Camera 10 m south of an entity at the origin, looking north at chest height.
    const world::Ray ray{{0.0f, 1.2f, -10.0f}, {0.0f, 0.0f, 1.0f}};
    const auto hit = world::rayHitsCapsule(ray, {0.0f, 0.0f, 0.0f}, 0.4f, 1.8f);
    REQUIRE(hit.has_value());
    CHECK(*hit == Approx(9.6f).margin(1e-3f));
    // Beside it, above its head, behind the camera: no hit.
    CHECK_FALSE(world::rayHitsCapsule(ray, {1.0f, 0.0f, 0.0f}, 0.4f, 1.8f));
    CHECK_FALSE(world::rayHitsCapsule({{0.0f, 2.5f, -10.0f}, {0.0f, 0.0f, 1.0f}}, {0.0f, 0.0f, 0.0f}, 0.4f, 1.8f));
    CHECK_FALSE(world::rayHitsCapsule(ray, {0.0f, 0.0f, -20.0f}, 0.4f, 1.8f));
    // Grazing the rounded top still hits, a little further in.
    const auto top = world::rayHitsCapsule({{0.0f, 1.75f, -10.0f}, {0.0f, 0.0f, 1.0f}}, {0.0f, 0.0f, 0.0f}, 0.4f, 1.8f);
    REQUIRE(top.has_value());
    CHECK(*top > 9.6f);
    // A steep ray from above (the usual third-person camera) onto the feet.
    const Vec3 eye{0.0f, 8.0f, -6.0f};
    const Vec3 dir = assets::normalize(Vec3{0.0f, 0.3f, 0.0f} - eye);
    CHECK(world::rayHitsCapsule({eye, dir}, {0.0f, 0.0f, 0.0f}, 0.4f, 1.8f).has_value());

    // The nearest of several along the ray wins; nothing in range gives 0.
    const std::vector<world::PickCandidate> c = {
        {7, {0.0f, 0.0f, 4.0f}, 0.4f, 1.8f}, {3, {0.0f, 0.0f, 0.0f}, 0.4f, 1.8f}, {9, {5.0f, 0.0f, 0.0f}, 0.4f, 1.8f}};
    CHECK(world::pickEntity(ray, c, 100.0f) == 3);
    CHECK(world::pickEntity(ray, c, 5.0f) == 0);
    CHECK(world::pickEntity({{5.0f, 1.0f, -10.0f}, {0.0f, 0.0f, 1.0f}}, c, 100.0f) == 9);
}

TEST_CASE("Combat view: Tab cycles the nearest monsters", "[client][combat]")
{
    const std::vector<world::TabCandidate> c = {{11, 12.0f}, {12, 5.0f}, {13, 30.0f}, {14, 55.0f}, {15, 5.0f}};
    CHECK(world::nextTabTarget(c, 0) == 12);  // nearest (tie by id)
    CHECK(world::nextTabTarget(c, 12) == 15);
    CHECK(world::nextTabTarget(c, 15) == 11);
    CHECK(world::nextTabTarget(c, 11) == 13);
    CHECK(world::nextTabTarget(c, 13) == 12); // wraps; 14 is beyond kTabRange
    CHECK(world::nextTabTarget(c, 14) == 12); // a target out of range starts again at the nearest
    CHECK(world::nextTabTarget(std::vector<world::TabCandidate>{{1, 60.0f}}, 0) == 0);
    CHECK(world::nextTabTarget(std::vector<world::TabCandidate>{}, 5) == 0);
    CHECK(world::nextTabTarget(std::vector<world::TabCandidate>{{4, 3.0f}}, 4) == 4); // only one: stays
}

TEST_CASE("Combat view: basic attack clip by main-hand grip", "[client][combat]")
{
    game::ItemTemplate t;
    for (const game::Grip g : {game::Grip::Sword, game::Grip::Dagger, game::Grip::Mace, game::Grip::Axe2h})
    {
        t.grip = g;
        CHECK(world::attackClipFor(&t, false, 0) == AnimState::SwordAttack);
        CHECK(world::attackClipFor(&t, false, 1) == AnimState::SwordAttack);
    }
    t.grip = game::Grip::Staff;
    CHECK(world::attackClipFor(&t, false, 0) == AnimState::SpellShoot);
    // Bare-handed and monsters: jab and cross alternate.
    CHECK(world::attackClipFor(nullptr, false, 0) == AnimState::PunchJab);
    CHECK(world::attackClipFor(nullptr, false, 1) == AnimState::PunchCross);
    CHECK(world::attackClipFor(nullptr, false, 2) == AnimState::PunchJab);
    t.grip = game::Grip::Sword;
    CHECK(world::attackClipFor(&t, true, 0) == AnimState::PunchJab); // a monster never "holds" anything
    CHECK(world::attackClipFor(&t, true, 1) == AnimState::PunchCross);
}

TEST_CASE("Combat view: skill cooldown, global cooldown and cast bar", "[client][combat]")
{
    world::SkillTimers s;
    const game::SkillDef& heavy = *game::findSkill(game::kSkillHeavyBlow); // 6 s, instant
    CHECK(s.ready(heavy.id, 0.0));
    CHECK(s.sweep(heavy.id, 0.0) == 0.0f);
    // A basic swing: only the 500 ms global cooldown.
    s.onAction(10.0);
    CHECK_FALSE(s.ready(heavy.id, 10.2));
    CHECK(s.remaining(heavy.id, 10.2) == Approx(0.3).margin(1e-6));
    CHECK(s.sweep(heavy.id, 10.25) == Approx(0.5f).margin(1e-3f));
    CHECK(s.ready(heavy.id, 10.5));
    // Ağır Darbe: its own 6 s.
    s.onSkillUsed(heavy.id, 20.0);
    CHECK(s.remaining(heavy.id, 20.0) == Approx(6.0).margin(1e-6));
    CHECK(s.sweep(heavy.id, 23.0) == Approx(0.5f).margin(1e-3f));
    CHECK_FALSE(s.ready(heavy.id, 25.9));
    CHECK(s.ready(heavy.id, 26.0));

    // Çifte Kesik: two COMBAT_EVENTs of one use do not restart the cooldown.
    world::SkillTimers r;
    const game::SkillDef& twin = *game::findSkill(game::kSkillTwinCut);
    r.onSkillUsed(twin.id, 5.0);
    r.onSkillUsed(twin.id, 5.05);
    CHECK(r.remaining(twin.id, 5.0) == Approx(5.0).margin(1e-6));

    // Köz Oku: cast bar from CAST_START, cooldown only once the cast completes.
    world::SkillTimers m;
    const game::SkillDef& bolt = *game::findSkill(game::kSkillEmberBolt);
    m.onCastStart(bolt.id, bolt.castMs, 1.0);
    CHECK(m.casting());
    CHECK(m.castSkill() == bolt.id);
    CHECK(m.castProgress(1.3) == Approx(0.5f).margin(1e-3f));
    CHECK(m.castProgress(5.0) == 1.0f);
    CHECK(m.ready(bolt.id, 1.3)); // nothing spent yet
    m.onCastEnd(bolt.id, false, 1.4); // interrupted by moving: costs nothing
    CHECK_FALSE(m.casting());
    CHECK(m.ready(bolt.id, 1.4));
    m.onCastStart(bolt.id, bolt.castMs, 2.0);
    m.onCastEnd(bolt.id, true, 2.6);
    CHECK_FALSE(m.casting());
    CHECK(m.remaining(bolt.id, 2.6) == Approx(3.0).margin(1e-6));
    CHECK(m.ready(bolt.id, 5.6));
    m.onCastStart(bolt.id, bolt.castMs, 7.0);
    m.cancelCast(); // death
    CHECK_FALSE(m.casting());
    // An unknown skill id changes nothing.
    m.onSkillUsed(99, 8.0);
    CHECK(m.ready(99, 8.0));
}

TEST_CASE("Combat view: floating numbers, styles and lifetime", "[client][combat]")
{
    CHECK(world::classifyCombatNumber(vn::kCombatKindDamage, 0, false) == world::FloatKind::Damage);
    CHECK(world::classifyCombatNumber(vn::kCombatKindDamage, vn::kCombatFlagCritical, false) ==
          world::FloatKind::Critical);
    CHECK(world::classifyCombatNumber(vn::kCombatKindDamage, vn::kCombatFlagCritical, true) ==
          world::FloatKind::TakenDamage); // damage we take is red even when critical
    CHECK(world::classifyCombatNumber(vn::kCombatKindHeal, 0, true) == world::FloatKind::Heal);
    // Critical numbers are larger and yellow, heals green, taken damage red, normal white.
    CHECK(world::floatStyle(world::FloatKind::Critical).fontScale > world::floatStyle(world::FloatKind::Damage).fontScale);
    CHECK(world::floatStyle(world::FloatKind::Damage).rgb == 0xFFFFFF);
    CHECK(world::floatStyle(world::FloatKind::Heal).rgb == 0x6BE36B);
    CHECK(world::floatStyle(world::FloatKind::TakenDamage).rgb == 0xFF5A4A);

    world::FloatingNumbers n;
    n.add(world::FloatKind::Damage, 12, {0.0f, 2.0f, 0.0f});
    n.add(world::FloatKind::Critical, 30, {0.0f, 2.0f, 0.0f});
    REQUIRE(n.items().size() == 2);
    CHECK(n.items()[0].offsetX != n.items()[1].offsetX); // fanned out
    CHECK(world::FloatingNumbers::alpha(n.items()[0]) == 0.0f); // fades in
    n.update(0.5f);
    CHECK(world::FloatingNumbers::alpha(n.items()[0]) == 1.0f);
    CHECK(world::FloatingNumbers::rise(n.items()[0]) > 0.0f);
    const float riseHalf = world::FloatingNumbers::rise(n.items()[0]);
    n.update(0.5f); // 1.0 s: the normal one (1.1 s) is fading, the critical one (1.5 s) not yet
    CHECK(world::FloatingNumbers::rise(n.items()[0]) > riseHalf);
    CHECK(world::FloatingNumbers::alpha(n.items()[0]) < 0.5f);
    n.update(0.2f);
    REQUIRE(n.items().size() == 1); // the normal number expired
    CHECK(n.items()[0].kind == world::FloatKind::Critical);
    n.update(0.4f);
    CHECK(n.items().empty());
    // Bounded: the oldest go first.
    for (std::uint32_t i = 0; i < world::FloatingNumbers::kMax + 5; ++i)
        n.add(world::FloatKind::Damage, i, {});
    CHECK(n.items().size() == world::FloatingNumbers::kMax);
    CHECK(n.items().front().amount == 5);
    CHECK(n.added() == world::FloatingNumbers::kMax + 7);
}

TEST_CASE("Combat view: ACTION_FAIL reasons as Turkish messages", "[client][combat]")
{
    using R = vn::ActionFailReason;
    using A = vn::CombatAction;
    CHECK(world::actionFailText(A::Attack, R::OutOfRange) == "Menzil dışında");
    CHECK(world::actionFailText(A::Skill, R::NotEnoughMp) == "Yeterli mana yok");
    CHECK(world::actionFailText(A::Attack, R::InSafeZone) == "Güvenli bölgede saldırılmaz");
    CHECK(world::actionFailText(A::Skill, R::OnCooldown) == "Henüz hazır değil");
    CHECK(world::actionFailText(A::Stance, R::Busy) == "Şu an oturamazsın");
    for (std::uint8_t r = vn::kActionFailReasonMin; r <= vn::kActionFailReasonMax; ++r)
        CHECK_FALSE(world::actionFailText(A::Attack, static_cast<R>(r)).empty());
}

TEST_CASE("Combat view: automation command parsing and its errors", "[client][combat][script]")
{
    std::string error;
    auto ok = [&](std::string_view line) {
        auto c = world::parseCombatCommand(line, error);
        INFO(line << ": " << error);
        REQUIRE(c.has_value());
        CHECK(error.empty());
        return *c;
    };
    auto bad = [&](std::string_view line, std::string_view usage) {
        const auto c = world::parseCombatCommand(line, error);
        INFO(line);
        CHECK_FALSE(c.has_value());
        CHECK(error.find(usage) != std::string::npos);
    };
    // Monster names have spaces and Turkish letters.
    auto t = ok("target nearest Kül Çömezi");
    CHECK(t.verb == world::CombatVerb::Target);
    CHECK(t.mode == "nearest");
    CHECK(t.name == "Kül Çömezi");
    CHECK(ok("target nearest").name.empty());
    CHECK(ok("target none").mode == "none");
    CHECK(ok("target self").mode == "self");
    t = ok("target player Kalkanci");
    CHECK(t.name == "Kalkanci");
    CHECK(ok("tab").verb == world::CombatVerb::Tab);
    CHECK(ok("attack").verb == world::CombatVerb::Attack);
    CHECK(ok("attackstop").verb == world::CombatVerb::AttackStop);
    CHECK(ok("cast").verb == world::CombatVerb::Cast);
    CHECK(ok("respawn").verb == world::CombatVerb::Respawn);
    CHECK(ok("stance sit").mode == "sit");
    CHECK(ok("stance stand").mode == "stand");
    auto hp = ok("expecthp 45 55");
    CHECK(hp.verb == world::CombatVerb::ExpectHp);
    CHECK(hp.a == 45.0f);
    CHECK(hp.b == 55.0f);
    CHECK(ok("expecttargethp 0 0").verb == world::CombatVerb::ExpectTargetHp);
    CHECK(ok("expectlevel 2").a == 2.0f);
    CHECK(ok("expectcombatmessage Henüz yeniden doğamazsın").name == "Henüz yeniden doğamazsın");
    auto walk = ok("walktomonster Kor Bekçisi 3.5");
    CHECK(walk.name == "Kor Bekçisi");
    CHECK(walk.a == 3.5f);
    walk = ok("walktomonster Kül Çömezi");
    CHECK(walk.name == "Kül Çömezi");
    CHECK(walk.a == 2.0f);
    auto hunt = ok("hunt Kül Çömezi 2");
    CHECK(hunt.verb == world::CombatVerb::Hunt);
    CHECK(hunt.name == "Kül Çömezi");
    CHECK(hunt.a == 2.0f);
    CHECK(ok("hunt stop").mode == "stop");
    CHECK(ok("combatlog").verb == world::CombatVerb::Log);

    // Malformed combat commands name the expected form.
    bad("target", "usage: target nearest");
    bad("target somebody", "usage: target nearest");
    bad("target none extra", "usage: target none");
    bad("target player", "usage: target player <name>");
    bad("tab 2", "usage: tab");
    bad("attack now", "usage: attack");
    bad("stance lie", "usage: stance sit|stand");
    bad("stance", "usage: stance sit|stand");
    bad("expecthp 50", "usage: expecthp");
    bad("expecthp 60 40", "usage: expecthp"); // min > max
    bad("expecthp a b", "usage: expecthp");
    bad("expectlevel 0", "usage: expectlevel");
    bad("expectcombatmessage", "usage: expectcombatmessage");
    bad("walktomonster", "usage: walktomonster");
    bad("walktomonster Kül -2", "usage: walktomonster");
    bad("hunt Kül Çömezi", "usage: hunt");
    bad("hunt Kül Çömezi 11", "usage: hunt");
    bad("combatlog now", "usage: combatlog");

    // Other verbs are not combat commands: no error, the scene tries its own.
    CHECK_FALSE(world::parseCombatCommand("camera behind 25 8", error).has_value());
    CHECK(error.empty());
    CHECK_FALSE(world::parseCombatCommand("", error).has_value());
    CHECK(error.empty());
}

namespace {

// One-bone rig with every clip of the combat set (each clip bends the child bone differently).
struct CombatRig
{
    assets::MeshData mesh;
    std::vector<assets::AnimationClip> store;
    world::CharacterClips clips;

    static assets::AnimationClip makeClip(float angle, bool loop, float duration, float moveSpeed = 0.0f)
    {
        assets::AnimationClip c;
        c.sampleRate = 30.0f;
        c.frameCount = static_cast<std::uint32_t>(duration * 30.0f) + 1;
        c.duration = static_cast<float>(c.frameCount - 1) / 30.0f;
        c.loop = loop;
        c.moveSpeed = moveSpeed;
        assets::AnimationTrack t;
        t.bone = "child";
        assets::Transform k;
        k.translation = {0.0f, 1.0f, 0.0f};
        k.rotation = assets::quatFromAxisAngle({1.0f, 0.0f, 0.0f}, angle);
        t.keys.assign(c.frameCount, k);
        c.tracks.push_back(t);
        return c;
    }

    explicit CombatRig(bool groundSit)
    {
        assets::Bone root;
        root.name = "root";
        assets::Bone child;
        child.name = "child";
        child.parent = 0;
        child.bindLocal.translation = {0.0f, 1.0f, 0.0f};
        mesh.skeleton.bones = {root, child};
        mesh.vertexFormat = assets::kVertexNormal | assets::kVertexSkin;
        store.reserve(world::kAnimStateCount);
        for (std::size_t i = 0; i < world::kAnimStateCount; ++i)
        {
            const auto s = static_cast<AnimState>(i);
            const bool loop = s == AnimState::Idle || s == AnimState::Walk || s == AnimState::Run ||
                              s == AnimState::SitIdle || s == AnimState::SpellIdle || s == AnimState::SitGroundIdle ||
                              s == AnimState::JumpLoop;
            const float speed = s == AnimState::Walk ? 1.2f : (s == AnimState::Run ? 4.8f : 0.0f);
            store.push_back(makeClip(0.05f * static_cast<float>(i), loop, 1.0f, speed));
        }
        for (std::size_t i = 0; i < world::kAnimStateCount; ++i)
        {
            const auto s = static_cast<AnimState>(i);
            const bool ground = s == AnimState::SitGroundEnter || s == AnimState::SitGroundIdle || s == AnimState::SitGroundExit;
            if (!ground || groundSit)
                clips.set(s, &store[i]);
        }
        clips.bind(mesh.skeleton);
    }
};

void run(world::Animator& a, float seconds, float speed = 0.0f)
{
    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f)
        a.update(1.0f / 60.0f, speed, true);
}

} // namespace

TEST_CASE("Animator: attack, hit reaction, cast and death (M9)", "[client][world][anim][combat]")
{
    CombatRig rig(true);
    world::Animator a;
    a.bind(&rig.mesh, &rig.clips);
    run(a, 0.1f);
    // A swing returns to locomotion; a 1 s clip squeezed into 0.5 s plays twice as fast.
    REQUIRE(a.playAttack(AnimState::SwordAttack, 0.5f));
    CHECK(a.action() == AnimAction::Attack);
    CHECK(a.state() == AnimState::SwordAttack);
    a.update(1.0f / 60.0f, 0.0f, true);
    CHECK(a.currentRate() == Approx(2.0f).margin(1e-3f));
    CHECK_FALSE(a.playHit()); // no hit reaction mid-attack
    run(a, 0.6f);
    CHECK(a.action() == AnimAction::None);
    CHECK(a.state() == AnimState::Idle);
    CHECK_FALSE(a.playAttack(AnimState::Idle)); // not an attack clip

    // Hit reaction, then back.
    REQUIRE(a.playHit());
    CHECK(a.state() == AnimState::HitChest);
    run(a, 1.0f);
    CHECK(a.action() == AnimAction::None);

    // Cast: enter -> idle loop until the end, then the shot.
    REQUIRE(a.beginCast());
    CHECK(a.state() == AnimState::SpellEnter);
    run(a, 1.0f);
    CHECK(a.state() == AnimState::SpellIdle);
    run(a, 2.0f);
    CHECK(a.state() == AnimState::SpellIdle); // loops while the server's cast runs
    a.endCast(true);
    CHECK(a.state() == AnimState::SpellShoot);
    run(a, 1.0f);
    CHECK(a.action() == AnimAction::None);
    // An interrupted cast just returns to locomotion.
    REQUIRE(a.beginCast());
    a.endCast(false);
    CHECK(a.action() == AnimAction::None);

    // Death holds the last frame; nothing else plays; revive stands up.
    a.die();
    CHECK(a.dead());
    run(a, 3.0f);
    CHECK(a.state() == AnimState::Death);
    CHECK(a.phase() == 1.0f);
    CHECK_FALSE(a.playAttack(AnimState::PunchJab));
    CHECK_FALSE(a.playHit());
    CHECK_FALSE(a.beginCast());
    CHECK_FALSE(a.jump());
    a.revive();
    CHECK_FALSE(a.dead());
    run(a, 0.5f);
    CHECK(a.state() == AnimState::Idle);
    // A corpse that spawns dead is on its last frame at once.
    world::Animator corpse;
    corpse.bind(&rig.mesh, &rig.clips);
    corpse.die(true);
    CHECK(corpse.state() == AnimState::Death);
    CHECK(corpse.phase() == 1.0f);
}

TEST_CASE("Animator: server-driven ground sit (M9 stance)", "[client][world][anim][combat]")
{
    CombatRig rig(true);
    world::Animator a;
    a.bind(&rig.mesh, &rig.clips);
    run(a, 0.1f);
    a.setSeated(true, false);
    CHECK(a.seated());
    CHECK(a.state() == AnimState::SitGroundEnter);
    run(a, 1.2f);
    CHECK(a.state() == AnimState::SitGroundIdle);
    CHECK(a.sittingIdle());
    a.setSeated(true, false); // repeated state: no restart
    CHECK(a.state() == AnimState::SitGroundIdle);
    CHECK_FALSE(a.playAttack(AnimState::PunchJab)); // seated: the server stands us up first
    a.setSeated(false, false);
    CHECK(a.state() == AnimState::SitGroundExit);
    run(a, 1.2f);
    CHECK_FALSE(a.seated());
    // A remote that spawns sitting: straight into the seated loop.
    world::Animator r;
    r.bind(&rig.mesh, &rig.clips);
    r.setSeated(true, true);
    CHECK(r.sittingIdle());
    CHECK(r.state() == AnimState::SitGroundIdle);
    // The local sit key uses the ground clips too; without them the chair clips.
    REQUIRE(a.toggleSit());
    CHECK(a.state() == AnimState::SitGroundEnter);
    CombatRig chair(false);
    world::Animator c;
    c.bind(&chair.mesh, &chair.clips);
    run(c, 0.1f);
    REQUIRE(c.toggleSit());
    CHECK(c.state() == AnimState::SitEnter);
}

TEST_CASE("Animator: a monster drawn larger strides further (move scale)", "[client][world][anim][combat]")
{
    CombatRig rig(true);
    world::Animator a;
    a.bind(&rig.mesh, &rig.clips);
    run(a, 1.0f, 4.8f);
    CHECK(a.state() == AnimState::Run);
    CHECK(a.currentRate() == Approx(1.0f).margin(0.02f));
    a.setMoveScale(1.25f); // the Kor Bekçisi: 1.25x size, its run covers 1.25x the ground per cycle
    run(a, 0.2f, 4.8f);
    CHECK(a.currentRate() == Approx(0.8f).margin(0.02f));
}

TEST_CASE("World model: monsters are spawned with their combat fields", "[client][world][combat]")
{
    world::WorldModel model;
    vn::EnterWorldRes enter;
    enter.entityId = 1;
    enter.zoneId = 1;
    model.enter(enter, nullptr, 0.0, true);
    vn::EntitySpawn m;
    m.entityId = 50;
    m.entityType = vn::kEntityTypeMonster;
    m.name = "Kül Çömezi";
    m.level = 2;
    m.hp = 70;
    m.maxHp = 105;
    m.templateId = 2;
    m.state = vn::kEntityStateInCombat;
    model.handleSpawn(m, 0.0);
    vn::EntitySpawn p;
    p.entityId = 51;
    p.entityType = vn::kEntityTypePlayer;
    p.name = "Gozcu";
    p.state = vn::kEntityStateSitting | vn::kEntityStateDead;
    model.handleSpawn(p, 0.0);
    REQUIRE(model.activeCount() == 2);
    CHECK(model.playerCount() == 1);
    CHECK(model.monsterCount() == 1);
    const world::RemoteEntity* e = model.find(50);
    REQUIRE(e != nullptr);
    CHECK(e->isMonster());
    CHECK(e->hp == 70);
    CHECK(e->maxHp == 105);
    CHECK(e->templateId == 2);
    CHECK_FALSE(e->dead());
    CHECK(model.stats().monsterSpawns == 1);
    const world::RemoteEntity* q = model.find(51);
    REQUIRE(q != nullptr);
    CHECK(q->dead());
    CHECK(q->sitting());
    // An unknown entity type is still ignored.
    vn::EntitySpawn x;
    x.entityId = 52;
    x.entityType = 9;
    model.handleSpawn(x, 0.0);
    CHECK(model.activeCount() == 2);
    CHECK(model.stats().ignoredSpawns == 1);
    // Despawn of the monster.
    model.handleDespawn({50});
    CHECK(model.monsterCount() == 0);
    CHECK(model.playerCount() == 1);
}
