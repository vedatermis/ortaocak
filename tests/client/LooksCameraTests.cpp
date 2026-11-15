// Polish round 1: character look resolution (client/world/CharacterAssets.h) against fake file sets,
// the cosmetic jump / sit actions of the Animator, camera collision with static obstacles
// (CameraObstacles, segmentEnterOccluder, OrbitCamera pull-in), the zone table and the
// "nearest walkable spot" path search used by the automation.
#include "client/world/Animator.h"
#include "client/world/CameraObstacles.h"
#include "client/world/CharacterAssets.h"
#include "client/world/OrbitCamera.h"
#include "client/world/TerrainQuery.h"
#include "shared/game/PathFind.h"
#include "shared/game/Zones.h"
#include "tests/client/WorldTestUtil.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>

using Catch::Approx;
namespace world = vo::client::world;
namespace assets = vo::assets;
using world::AnimAction;
using world::AnimState;
using world::Vec3;

namespace {

constexpr std::uint8_t kAydor = 1;
constexpr std::uint8_t kKargan = 2;

// A fake assets/ tree: exactly the listed paths exist.
world::AssetExists files(std::set<std::string> paths)
{
    return [paths = std::move(paths)](std::string_view p) { return paths.count(std::string(p)) != 0; };
}

std::set<std::string> legacyRace(const std::string& race)
{
    std::set<std::string> s{"characters/" + race + ".vmesh"};
    for (const char* clip : {"idle", "walk", "run", "jump_start", "jump_loop", "jump_land", "sit_enter", "sit_idle",
                             "sit_exit"})
        s.insert("characters/" + race + "/" + clip + ".vanim");
    return s;
}

} // namespace

TEST_CASE("Looks: today's legacy files give one look per race", "[client][looks]")
{
    std::set<std::string> fs = legacyRace("aydor");
    fs.merge(legacyRace("kargan"));
    world::CharacterLookTable t;
    t.discover(files(fs));
    CHECK(t.lookCount(kAydor) == 1);
    CHECK(t.lookCount(kKargan) == 1);
    const world::LookFiles& a = t.files(kAydor, 0);
    CHECK(a.available);
    CHECK(a.legacy);
    CHECK(a.mesh == "characters/aydor.vmesh");
    CHECK(a.clips[static_cast<std::size_t>(world::CharacterClip::Walk)] == "characters/aydor/walk.vanim");
    CHECK(a.clips[static_cast<std::size_t>(world::CharacterClip::SitExit)] == "characters/aydor/sit_exit.vanim");
    CHECK(t.files(kKargan, 0).mesh == "characters/kargan.vmesh");
    // Every other look index (old characters may carry face 1-3) falls back to look 0.
    for (int look = 1; look < world::kMaxLooks; ++look)
    {
        CHECK_FALSE(t.available(kAydor, look));
        CHECK(t.effectiveLook(kAydor, look) == 0);
        CHECK(t.files(kKargan, look).mesh == "characters/kargan.vmesh");
    }
    // Selector helpers with a single look.
    CHECK(t.nthAvailable(kAydor, 0) == 0);
    CHECK(t.nthAvailable(kAydor, 1) == 0);
    CHECK(t.nthAvailable(kAydor, -1) == 0);
}

TEST_CASE("Looks: suffixed files per look, clips per look, legacy only for look 0", "[client][looks]")
{
    // Aydor: three suffixed looks (the art branch), dotted clip names for look 1, directory clip
    // names for look 2, plus the legacy files (look 0 must prefer aydor_0).
    std::set<std::string> fs = legacyRace("aydor");
    for (const char* f : {"characters/aydor_0.vmesh", "characters/aydor_0.idle.vanim", "characters/aydor_0.walk.vanim",
                          "characters/aydor_1.vmesh", "characters/aydor_1.idle.vanim", "characters/aydor_1.walk.vanim",
                          "characters/aydor_1.run.vanim", "characters/aydor_2.vmesh", "characters/aydor_2/idle.vanim",
                          "characters/aydor_2/walk.vanim"})
        fs.insert(f);
    // Kargan: only the legacy look plus look 2 (a gap at look 1).
    fs.merge(legacyRace("kargan"));
    fs.insert("characters/kargan_2.vmesh");
    world::CharacterLookTable t;
    t.discover(files(fs));

    CHECK(t.lookCount(kAydor) == 3);
    CHECK(t.files(kAydor, 0).mesh == "characters/aydor_0.vmesh");
    CHECK_FALSE(t.files(kAydor, 0).legacy);
    CHECK(t.files(kAydor, 0).clips[0] == "characters/aydor_0.idle.vanim");
    // A clip a look does not have keeps its first candidate (it fails to load; never another look's).
    CHECK(t.files(kAydor, 0).clips[static_cast<std::size_t>(world::CharacterClip::Run)] ==
          "characters/aydor_0.run.vanim");
    CHECK(t.files(kAydor, 1).mesh == "characters/aydor_1.vmesh");
    CHECK(t.files(kAydor, 1).clips[2] == "characters/aydor_1.run.vanim");
    CHECK(t.files(kAydor, 2).mesh == "characters/aydor_2.vmesh");
    CHECK(t.files(kAydor, 2).clips[1] == "characters/aydor_2/walk.vanim");
    CHECK(t.effectiveLook(kAydor, 3) == 0); // no aydor_3: look 0
    CHECK(t.files(kAydor, 3).mesh == "characters/aydor_0.vmesh");

    CHECK(t.lookCount(kKargan) == 2);
    CHECK(t.files(kKargan, 0).mesh == "characters/kargan.vmesh"); // legacy look 0
    CHECK(t.files(kKargan, 1).mesh == "characters/kargan.vmesh"); // missing look 1 -> look 0
    CHECK(t.files(kKargan, 2).mesh == "characters/kargan_2.vmesh");
    // The selector walks the available looks only: 0, 2, 0, ...
    CHECK(t.nthAvailable(kKargan, 0) == 0);
    CHECK(t.nthAvailable(kKargan, 1) == 2);
    CHECK(t.nthAvailable(kKargan, 2) == 0);
    CHECK(t.nthAvailable(kKargan, -1) == 2);
    CHECK(t.indexOfLook(kKargan, 2) == 1);
}

TEST_CASE("Looks: a race without look 0 and a race without any files", "[client][looks]")
{
    world::CharacterLookTable t;
    t.discover(files({"characters/aydor_1.vmesh"}));
    CHECK(t.lookCount(kAydor) == 1);
    CHECK(t.effectiveLook(kAydor, 0) == 1); // first available look
    CHECK(t.files(kAydor, 3).mesh == "characters/aydor_1.vmesh");
    CHECK(t.lookCount(kKargan) == 0);
    CHECK(t.effectiveLook(kKargan, 2) == 0);
    CHECK(t.files(kKargan, 2).mesh == "characters/kargan.vmesh"); // a path still (the load fails, logged)
    CHECK_FALSE(t.files(kKargan, 2).available);
    // Nothing discovered at all (no assets directory).
    const world::CharacterLookTable empty;
    CHECK(empty.lookCount(kAydor) == 0);
    CHECK(empty.files(kAydor, 1).mesh == "characters/aydor.vmesh");
    // Unknown races resolve like Aydor (validated elsewhere; never out of bounds).
    CHECK(empty.files(7, 9).mesh == "characters/aydor.vmesh");
}

// ---- Jump / sit ----

namespace {

struct ActionRig
{
    assets::MeshData mesh;
    std::vector<assets::AnimationClip> store;
    world::CharacterClips clips;

    static assets::AnimationClip makeClip(const char* name, float angle, float moveSpeed, bool loop, float duration)
    {
        assets::AnimationClip c;
        c.name = name;
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

    explicit ActionRig(bool withActions)
    {
        assets::Bone root;
        root.name = "root";
        assets::Bone child;
        child.name = "child";
        child.parent = 0;
        child.bindLocal.translation = {0.0f, 1.0f, 0.0f};
        assets::Mat4 inv;
        inv.at(1, 3) = -1.0f;
        child.inverseBind = inv;
        mesh.skeleton.bones = {root, child};
        mesh.vertexFormat = assets::kVertexNormal | assets::kVertexSkin;
        store.reserve(9);
        store.push_back(makeClip("idle", 0.0f, 0.0f, true, 1.0f));
        store.push_back(makeClip("walk", 0.2f, 1.0f, true, 1.0f));
        store.push_back(makeClip("run", 0.4f, 5.5f, true, 1.0f));
        clips.set(AnimState::Idle, &store[0]);
        clips.set(AnimState::Walk, &store[1]);
        clips.set(AnimState::Run, &store[2]);
        if (withActions)
        {
            store.push_back(makeClip("jump_start", 0.6f, 0.0f, false, 1.33f));
            store.push_back(makeClip("jump_loop", 0.7f, 0.0f, true, 2.5f));
            store.push_back(makeClip("jump_land", 0.8f, 0.0f, false, 1.27f));
            store.push_back(makeClip("sit_enter", 0.9f, 0.0f, false, 1.3f));
            store.push_back(makeClip("sit_idle", 1.0f, 0.0f, true, 1.7f));
            store.push_back(makeClip("sit_exit", 1.1f, 0.0f, false, 1.03f));
            for (std::size_t i = 3; i < 9; ++i)
                clips.clips[i] = &store[i];
        }
        clips.bind(mesh.skeleton);
    }
};

float childAngle(const world::Animator& a)
{
    const assets::Mat4& m = a.skinMatrices()[1];
    return std::atan2(m.at(2, 1), m.at(1, 1));
}

} // namespace

TEST_CASE("Animator: cosmetic jump arc and clip sequence", "[client][world][anim][actions]")
{
    ActionRig rig(true);
    world::Animator a;
    a.bind(&rig.mesh, &rig.clips);
    a.update(0.1f, 0.0f, false);
    REQUIRE(a.canJump());
    REQUIRE(a.jump());
    CHECK_FALSE(a.jump()); // one at a time
    CHECK(a.action() == AnimAction::Jump);
    CHECK(a.state() == AnimState::JumpStart);
    std::set<AnimState> seen;
    float peak = 0.0f;
    float t = 0.0f;
    const float dt = 1.0f / 60.0f;
    while (a.action() == AnimAction::Jump && t < 5.0f)
    {
        a.update(dt, 0.0f, false);
        a.evaluate();
        seen.insert(a.state());
        peak = std::max(peak, a.heightOffset());
        CHECK(a.heightOffset() >= 0.0f);
        t += dt;
    }
    CHECK(a.action() == AnimAction::None);
    CHECK(seen.count(AnimState::JumpLoop) == 1);
    CHECK(seen.count(AnimState::JumpLand) == 1);
    CHECK(peak == Approx(world::Animator::kJumpHeight).margin(0.02f));
    CHECK(a.heightOffset() == 0.0f);
    CHECK(t > world::Animator::kJumpTakeoff + world::Animator::kJumpAirTime);
    CHECK(t < 2.0f); // short and snappy
    for (int i = 0; i < 30; ++i)
        a.update(dt, 0.0f, false);
    a.evaluate();
    CHECK(a.state() == AnimState::Idle);
    CHECK(childAngle(a) == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("Animator: sit down, stay seated, stand up; movement only after standing", "[client][world][anim][actions]")
{
    ActionRig rig(true);
    world::Animator a;
    a.bind(&rig.mesh, &rig.clips);
    a.update(0.1f, 0.0f, false);
    REQUIRE(a.toggleSit());
    CHECK(a.seated());
    CHECK(a.state() == AnimState::SitEnter);
    for (int i = 0; i < 120; ++i)
        a.update(1.0f / 60.0f, 0.0f, false);
    CHECK(a.state() == AnimState::SitIdle);
    CHECK(a.seated());
    CHECK_FALSE(a.jump()); // no jumping while seated
    // Stand up (any movement input calls standUp()); seated until sit_exit is nearly done.
    a.standUp();
    CHECK(a.state() == AnimState::SitExit);
    CHECK(a.seated());
    int frames = 0;
    while (a.seated() && frames < 200)
    {
        a.update(1.0f / 60.0f, 0.0f, false);
        ++frames;
    }
    CHECK_FALSE(a.seated());
    CHECK(frames > 30); // the exit clip actually plays
    CHECK(a.state() == AnimState::Idle);
    // toggleSit() also stands up.
    REQUIRE(a.toggleSit());
    a.update(0.5f, 0.0f, false);
    CHECK(a.toggleSit());
    CHECK(a.state() == AnimState::SitExit);
}

TEST_CASE("Animator: actions need their clips and a standing character", "[client][world][anim][actions]")
{
    ActionRig bare(false);
    world::Animator a;
    a.bind(&bare.mesh, &bare.clips);
    a.update(0.1f, 0.0f, false);
    CHECK_FALSE(a.canJump());
    CHECK_FALSE(a.jump());
    CHECK_FALSE(a.toggleSit());
    CHECK(a.action() == AnimAction::None);

    ActionRig rig(true);
    world::Animator b;
    b.bind(&rig.mesh, &rig.clips);
    for (int i = 0; i < 30; ++i)
        b.update(1.0f / 60.0f, 1.4f, false); // walking
    CHECK_FALSE(b.canSit());
    CHECK(b.canJump()); // jumping while walking is fine
}

// ---- Camera vs. obstacles ----

TEST_CASE("Ray vs oriented box: entry fraction, start inside, rotated and scaled boxes", "[client][world][camera]")
{
    // 2 x 3 x 1 box (local x -1..1, y 0..3, z -0.5..0.5) at (10, 0, 10), turned 90 degrees, scale 2:
    // world x 9..11, z 8..12, y 0..6.
    const auto box = world::makeOccluder(world::makeWorld({10.0f, 0.0f, 10.0f}, 3.14159265f * 0.5f, 2.0f),
                                         {-1.0f, 0.0f, -0.5f}, {1.0f, 3.0f, 0.5f});
    float t = 0.0f;
    bool inside = true;
    REQUIRE(world::segmentEnterOccluder(box, {0.0f, 2.0f, 10.0f}, {20.0f, 2.0f, 10.0f}, t, inside));
    CHECK_FALSE(inside);
    CHECK(t == Approx(9.0f / 20.0f).margin(1e-4f)); // enters at x = 9
    REQUIRE(world::segmentEnterOccluder(box, {10.0f, 2.0f, 0.0f}, {10.0f, 2.0f, 20.0f}, t, inside));
    CHECK(t == Approx(8.0f / 20.0f).margin(1e-4f)); // 4 m long along z: enters at z = 8
    // Diagonal from above: hits the roof plane y = 6.
    REQUIRE(world::segmentEnterOccluder(box, {10.0f, 10.0f, 10.0f}, {10.0f, 0.0f, 10.0f}, t, inside));
    CHECK(t == Approx(0.4f).margin(1e-4f));
    // Starting inside.
    REQUIRE(world::segmentEnterOccluder(box, {10.0f, 1.0f, 10.0f}, {30.0f, 1.0f, 10.0f}, t, inside));
    CHECK(inside);
    CHECK(t == 0.0f);
    // Misses: beside, above, stops short.
    CHECK_FALSE(world::segmentEnterOccluder(box, {0.0f, 2.0f, 13.0f}, {20.0f, 2.0f, 13.0f}, t, inside));
    CHECK_FALSE(world::segmentEnterOccluder(box, {0.0f, 7.0f, 10.0f}, {20.0f, 7.0f, 10.0f}, t, inside));
    CHECK_FALSE(world::segmentEnterOccluder(box, {0.0f, 2.0f, 10.0f}, {8.5f, 2.0f, 10.0f}, t, inside));
    CHECK_FALSE(world::segmentEnterOccluder(box, {11.3f, 2.0f, 0.0f}, {11.3f, 2.0f, 20.0f}, t, inside));
    // Agrees with the boolean helper on a sweep of random-ish segments.
    for (int i = 0; i < 200; ++i)
    {
        const float a0 = static_cast<float>(i) * 0.731f;
        const Vec3 a{10.0f + 9.0f * std::cos(a0), 1.0f + static_cast<float>(i % 7), 10.0f + 9.0f * std::sin(a0)};
        const Vec3 b{10.0f - 9.0f * std::cos(a0 * 1.3f), 4.0f - static_cast<float>(i % 5), 10.0f - 9.0f * std::sin(a0 * 0.7f)};
        const bool hit = world::segmentEnterOccluder(box, a, b, t, inside);
        CHECK(hit == world::segmentHitsOccluder(box, a, b));
    }
}

TEST_CASE("CameraObstacles: size filter, broad-phase grid, clip fraction", "[client][world][camera]")
{
    world::CameraObstacles o;
    // A house-sized box (6 x 4 x 5) at (100, 0, 100).
    CHECK(o.add(world::makeWorld({100.0f, 0.0f, 100.0f}, 0.0f, 1.0f), {-3.0f, 0.0f, -2.5f}, {3.0f, 4.0f, 2.5f}));
    // Lamp post (0.3 x 3.5 x 0.3), fence (2 x 1 x 0.1), crate (0.8 x 0.8 x 0.8): skipped.
    CHECK_FALSE(o.add(world::makeWorld({50.0f, 0.0f, 50.0f}, 0.0f, 1.0f), {-0.15f, 0.0f, -0.15f}, {0.15f, 3.5f, 0.15f}));
    CHECK_FALSE(o.add(world::makeWorld({60.0f, 0.0f, 50.0f}, 0.3f, 1.0f), {-1.0f, 0.0f, -0.05f}, {1.0f, 1.0f, 0.05f}));
    CHECK_FALSE(o.add(world::makeWorld({70.0f, 0.0f, 50.0f}, 0.0f, 1.0f), {-0.4f, 0.0f, -0.4f}, {0.4f, 0.8f, 0.4f}));
    // A scaled-up rock passes the filter because of its placement scale.
    CHECK(o.add(world::makeWorld({160.0f, 0.0f, 160.0f}, 0.0f, 3.0f), {-0.4f, 0.0f, -0.4f}, {0.4f, 0.6f, 0.4f}));
    o.finish();
    CHECK(o.size() == 2);
    // Boom from in front of the house through it: enters the grown box (margin) before the wall.
    const float t = o.clip({100.0f, 1.5f, 90.0f}, {100.0f, 1.5f, 110.0f});
    const float wallZ = 100.0f - 2.5f - world::CameraObstacles::kMargin;
    CHECK(t == Approx((wallZ - 90.0f) / 20.0f).margin(1e-3f));
    CHECK(o.contains({100.0f, 1.0f, 100.0f}));
    CHECK_FALSE(o.contains({100.0f, 1.0f, 90.0f}));
    // Far away (other grid cells): clear.
    CHECK(o.clip({20.0f, 1.5f, 20.0f}, {30.0f, 1.5f, 30.0f}) == 1.0f);
    // A segment starting inside a box ignores it without a terrain (no walk mask to find the walls).
    CHECK(o.clip({100.0f, 1.5f, 100.0f}, {100.0f, 1.5f, 80.0f}) == 1.0f);
    // The rock at x 158.55..161.45 (scale 3 + margin).
    CHECK(o.clip({150.0f, 0.5f, 160.0f}, {170.0f, 0.5f, 160.0f}) < 1.0f);
}

TEST_CASE("OrbitCamera: pulls in in front of a house wall behind the character and eases out", "[client][world][camera]")
{
    const auto flat = vo::client::test::makeFlat(0.0f, 128.0f);
    world::CameraObstacles o;
    // House whose front wall is at z = 60 (box z 60..66, x 54..74, y 0..5).
    REQUIRE(o.add(world::makeWorld({64.0f, 0.0f, 63.0f}, 0.0f, 1.0f), {-10.0f, 0.0f, -3.0f}, {10.0f, 5.0f, 3.0f}));
    o.finish();
    world::OrbitCamera cam;
    cam.setObstacles(&o);
    // Character at z = 57, facing -Z: the camera sits behind it, towards the house (+Z).
    const Vec3 focus{64.0f, 1.55f, 57.0f};
    cam.setPose(world::kPi, 0.3f, 10.0f);
    cam.reset(focus, world::kPi);
    for (int i = 0; i < 60; ++i)
    {
        cam.update(focus, &flat, 1.0f / 60.0f);
        const Vec3 e = cam.eye();
        REQUIRE_FALSE(o.contains(e));
        REQUIRE(e.z < 60.0f - world::CameraObstacles::kMargin + 1e-3f);
    }
    CHECK(cam.effectiveDistance() < 4.0f); // pulled in from 10 m
    CHECK(cam.effectiveDistance() > 2.5f);
    // Turn the camera away from the house: the boom eases back out to 10 m (not instantly).
    cam.setPose(0.0f, 0.3f, 10.0f);
    cam.update(focus, &flat, 1.0f / 60.0f);
    // setPose snaps; collide again from a short boom to see the ease-out.
    cam.setPose(world::kPi, 0.3f, 10.0f);
    for (int i = 0; i < 10; ++i)
        cam.update(focus, &flat, 1.0f / 60.0f);
    const float pulled = cam.effectiveDistance();
    cam.rotate(world::kPi / cam.params().radiansPerPixel, 0.0f); // half a turn: looking at the house
    cam.update(focus, &flat, 1.0f / 60.0f);
    CHECK(cam.effectiveDistance() > pulled);
    CHECK(cam.effectiveDistance() < 9.0f); // eased, not snapped
    for (int i = 0; i < 240; ++i)
        cam.update(focus, &flat, 1.0f / 60.0f);
    CHECK(cam.effectiveDistance() == Approx(10.0f).margin(0.05f));
    // Without obstacles the old behaviour: straight into the house.
    cam.setObstacles(nullptr);
    cam.setPose(world::kPi, 0.3f, 6.0f);
    cam.update(focus, &flat, 1.0f / 60.0f);
    CHECK(o.contains(cam.eye()));
}

TEST_CASE("OrbitCamera: a character under the eaves still keeps the camera out of the house", "[client][world][camera]")
{
    // House box (with a roof overhang) x 54..74, z 58..66; its walls (walk-mask footprint) z 60..66.
    const auto terrain = vo::client::test::makeTerrain(129, 128.0f, [](float, float) { return 0.0f; },
                                                       [](int i, int j) { return i >= 54 && i < 74 && j >= 60 && j < 66; });
    world::CameraObstacles o;
    o.setTerrain(&terrain);
    REQUIRE(o.add(world::makeWorld({64.0f, 0.0f, 62.0f}, 0.0f, 1.0f), {-10.0f, 0.0f, -4.0f}, {10.0f, 5.0f, 4.0f}));
    o.finish();
    world::OrbitCamera cam;
    cam.setObstacles(&o);
    const Vec3 focus{64.0f, 1.55f, 59.0f}; // inside the box (under the eaves), 1 m from the wall
    REQUIRE(o.contains(focus));
    cam.setPose(world::kPi, 0.2f, 8.0f);
    cam.reset(focus, world::kPi);
    for (int i = 0; i < 30; ++i)
        cam.update(focus, &terrain, 1.0f / 60.0f);
    CHECK(cam.eye().z < 60.0f);
    CHECK(cam.effectiveDistance() < 1.2f);
}

// ---- Zones and path search ----

TEST_CASE("Zones: display names", "[game][zones]")
{
    CHECK(vo::game::zoneDisplayName(1) == "Ortaocak");
    REQUIRE(vo::game::findZone(1) != nullptr);
    CHECK(vo::game::findZone(1)->tagline == "Eşik'in tarafsız köyü");
    CHECK(vo::game::findZone(77) == nullptr);
    CHECK(vo::game::zoneDisplayName(77) == "Bölge 77");
}

TEST_CASE("Path search: nearest clear spot when the target is blocked", "[game][path]")
{
    // A 6 x 6 m "house" at cells 30..35 in a 64 m zone.
    const auto t = vo::client::test::makeTerrain(65, 64.0f, [](float, float) { return 0.0f; },
                                                 [](int i, int j) { return i >= 30 && i < 36 && j >= 30 && j < 36; });
    // Target in the middle of the house: no direct path, the nearest clear spot instead.
    CHECK_FALSE(vo::game::findWalkPath(t, {10.0f, 33.0f}, {33.0f, 33.0f}));
    const auto spot = vo::game::findWalkPathNear(t, {10.0f, 33.0f}, {33.0f, 33.0f}, 4.0f, 0.5f, 0.5f);
    REQUIRE(spot);
    CHECK(vo::game::isClearSpot(t, spot->target.x, spot->target.z, 0.5f));
    CHECK(std::hypot(spot->target.x - 33.0f, spot->target.z - 33.0f) <= 4.0f + 1e-3f);
    CHECK(spot->points.back().x == spot->target.x);
    // Reachable targets are kept exactly.
    const auto direct = vo::game::findWalkPathNear(t, {10.0f, 10.0f}, {20.0f, 20.0f});
    REQUIRE(direct);
    CHECK(direct->target.x == 20.0f);
    CHECK(direct->target.z == 20.0f);
    // Nothing clear within the radius: nullopt.
    CHECK_FALSE(vo::game::findWalkPathNear(t, {10.0f, 33.0f}, {33.0f, 33.0f}, 1.0f, 0.5f, 0.5f));
}
