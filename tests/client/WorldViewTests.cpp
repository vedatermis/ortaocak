// Animation state selection / cross-fades, orbit camera math and terrain picking
// (client/world/Animator.h, OrbitCamera.h, TerrainQuery.h, WorldMath.h).
#include "client/world/Animator.h"
#include "client/world/OrbitCamera.h"
#include "client/world/TerrainQuery.h"
#include "client/world/WorldMath.h"
#include "tests/client/WorldTestUtil.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using vo::client::world::AnimState;
using vo::client::world::Vec3;
namespace world = vo::client::world;
namespace assets = vo::assets;

namespace {

// Two-bone skeleton and three clips that rotate the child bone by different constant angles, so the
// sampled pose tells which clip (and blend) is playing.
struct Rig
{
    assets::MeshData mesh;
    assets::AnimationClip idle;
    assets::AnimationClip walk;
    assets::AnimationClip run;
    world::CharacterClips clips;

    static assets::AnimationClip makeClip(const char* name, float angle, float moveSpeed)
    {
        assets::AnimationClip c;
        c.name = name;
        c.frameCount = 31;
        c.sampleRate = 30.0f;
        c.duration = 1.0f;
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

    Rig()
    {
        assets::Bone root;
        root.name = "root";
        assets::Bone child;
        child.name = "child";
        child.parent = 0;
        child.bindLocal.translation = {0.0f, 1.0f, 0.0f};
        assets::Mat4 inv;
        inv.at(1, 3) = -1.0f; // inverse of translate(0, 1, 0)
        child.inverseBind = inv;
        mesh.skeleton.bones = {root, child};
        mesh.vertexFormat = assets::kVertexNormal | assets::kVertexSkin;
        idle = makeClip("idle", 0.0f, 0.0f);
        walk = makeClip("walk", 0.5f, 2.0f);
        run = makeClip("run", 1.0f, 5.5f);
        clips.idle = &idle;
        clips.walk = &walk;
        clips.run = &run;
        clips.bind(mesh.skeleton);
    }
};

// Angle about +X encoded in the child's skinning matrix.
float childAngle(const world::Animator& a)
{
    const assets::Mat4& m = a.skinMatrices()[1];
    return std::atan2(m.at(2, 1), m.at(1, 1));
}

} // namespace

TEST_CASE("Animation: state selection with hysteresis and running flag", "[client][world][anim]")
{
    CHECK(world::selectAnimState(AnimState::Idle, 0.0f, true) == AnimState::Idle);
    CHECK(world::selectAnimState(AnimState::Idle, 0.3f, false) == AnimState::Idle); // below start
    CHECK(world::selectAnimState(AnimState::Idle, 1.0f, false) == AnimState::Walk);
    CHECK(world::selectAnimState(AnimState::Idle, 1.0f, true) == AnimState::Run);
    CHECK(world::selectAnimState(AnimState::Walk, 0.2f, false) == AnimState::Walk); // above stop
    CHECK(world::selectAnimState(AnimState::Walk, 0.1f, false) == AnimState::Idle);
    CHECK(world::selectAnimState(AnimState::Run, 4.0f, false) == AnimState::Walk);

    Rig rig;
    CHECK(world::playbackRate(rig.walk, 2.0f) == Approx(1.0f));
    CHECK(world::playbackRate(rig.run, 2.75f) == Approx(0.5f));
    CHECK(world::playbackRate(rig.idle, 3.0f) == 1.0f); // in place
    CHECK(world::playbackRate(rig.walk, 100.0f) == 3.0f);
}

TEST_CASE("Animator: cross-fades over ~0.18 s, keeps stride phase walk<->run, scales rate by speed",
          "[client][world][anim]")
{
    Rig rig;
    world::Animator a;
    a.bind(&rig.mesh, &rig.clips);
    a.update(0.1f, 0.0f, false);
    a.evaluate();
    CHECK(a.state() == AnimState::Idle);
    CHECK(childAngle(a) == Approx(0.0f).margin(1e-4f));

    // Start walking at 2 m/s: becomes Walk within a few frames, fading in.
    for (int i = 0; i < 5; ++i)
        a.update(1.0f / 60.0f, 2.0f, false);
    CHECK(a.state() == AnimState::Walk);
    CHECK(a.fading());
    a.evaluate();
    const float mid = childAngle(a);
    CHECK(mid > 0.0f);
    CHECK(mid < 0.5f);
    for (int i = 0; i < 15; ++i)
        a.update(1.0f / 60.0f, 2.0f, false);
    CHECK_FALSE(a.fading());
    a.evaluate();
    CHECK(childAngle(a) == Approx(0.5f).margin(1e-3f));

    // Phase advances at speed / moveSpeed: 0.5 s at 2 m/s on a 1 s, 2 m/s clip = half a cycle.
    const float p0 = a.phase();
    for (int i = 0; i < 30; ++i)
        a.update(1.0f / 60.0f, 2.0f, false);
    float dp = a.phase() - p0;
    dp -= std::floor(dp);
    CHECK(dp == Approx(0.5f).margin(0.02f));

    // Walk -> run keeps the normalised phase.
    const float before = a.phase();
    a.update(1.0f / 60.0f, 5.5f, true);
    CHECK(a.state() == AnimState::Run);
    CHECK(std::fabs(a.phase() - before) < 0.1f);

    // Stop: back to idle after the speed decays.
    for (int i = 0; i < 60; ++i)
        a.update(1.0f / 60.0f, 0.0f, true);
    CHECK(a.state() == AnimState::Idle);
    for (int i = 0; i < 20; ++i)
        a.update(1.0f / 60.0f, 0.0f, true);
    a.evaluate();
    CHECK(childAngle(a) == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("Camera math: look-at, reverse-Z projection, frustum and screen rays", "[client][world][camera]")
{
    const world::Mat4 view = world::lookAtLH({0.0f, 0.0f, -10.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const world::Mat4 proj = world::perspectiveReverseZ(1.0f, 1.5f, 0.1f, 1000.0f);
    const world::Mat4 vp = proj * view;
    // Near plane -> depth 1, far plane -> depth 0 (reverse-Z).
    auto depthAt = [&](float z)
    {
        const world::Vec4 c = world::transform(vp, {0.0f, 0.0f, z});
        return c.z / c.w;
    };
    CHECK(depthAt(-10.0f + 0.1f) == Approx(1.0f).margin(1e-4f));
    CHECK(depthAt(-10.0f + 1000.0f) == Approx(0.0f).margin(1e-4f));
    CHECK(depthAt(0.0f) > depthAt(10.0f)); // nearer = larger

    // +X is to the right on screen (left-handed).
    world::Vec2 px{};
    REQUIRE(world::projectToScreen(vp, {1.0f, 0.0f, 0.0f}, 300.0f, 200.0f, px));
    CHECK(px.x > 150.0f);
    CHECK(px.y == Approx(100.0f));
    REQUIRE(world::projectToScreen(vp, {0.0f, 1.0f, 0.0f}, 300.0f, 200.0f, px));
    CHECK(px.y < 100.0f);
    CHECK_FALSE(world::projectToScreen(vp, {0.0f, 0.0f, -20.0f}, 300.0f, 200.0f, px)); // behind

    world::Frustum f;
    f.setFromViewProj(vp);
    CHECK(f.intersectsSphere({0.0f, 0.0f, 0.0f}, 0.5f));
    CHECK_FALSE(f.intersectsSphere({0.0f, 0.0f, -30.0f}, 1.0f));  // behind
    CHECK_FALSE(f.intersectsSphere({100.0f, 0.0f, 0.0f}, 1.0f));  // far right
    CHECK(f.intersectsAabb({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}));
    CHECK_FALSE(f.intersectsAabb({0.0f, 0.0f, 2000.0f}, {1.0f, 1.0f, 2001.0f})); // beyond far

    // Screen centre ray goes along +Z from the eye.
    assets::Mat4 inv;
    REQUIRE(assets::invert(vp, inv));
    const world::Ray r = world::screenRay(inv, 149.5f, 99.5f, 300.0f, 200.0f);
    CHECK(r.direction.z == Approx(1.0f).margin(1e-3f));
    CHECK(r.origin.z == Approx(-9.9f).margin(1e-2f));

    // makeWorld: yaw maps +Z to forward(yaw).
    const world::Mat4 w = world::makeWorld({1.0f, 2.0f, 3.0f}, world::kPi / 2.0f, 2.0f);
    const Vec3 p = assets::transformPoint(w, {0.0f, 0.0f, 1.0f});
    CHECK(p.x == Approx(3.0f));
    CHECK(p.y == Approx(2.0f));
    CHECK(p.z == Approx(3.0f).margin(1e-5f));

    CHECK(world::wrapAngle(3.0f * world::kPi) == Approx(-world::kPi).margin(1e-5f));
    CHECK(world::angleDelta(3.0f, -3.0f) == Approx(2.0f * world::kPi - 6.0f).margin(1e-5f));
    CHECK(world::approachAngle(0.0f, 1.0f, 0.25f) == Approx(0.25f));
}

TEST_CASE("OrbitCamera: orbit, pitch/zoom clamps, default view behind the character", "[client][world][camera]")
{
    world::OrbitCamera cam;
    cam.setPose(0.0f, 0.4f, 8.0f);
    cam.reset({10.0f, 2.0f, 10.0f}, 0.0f); // character faces +Z
    // Behind = -Z, above.
    CHECK(cam.eye().z < 10.0f);
    CHECK(cam.eye().y > 2.0f);
    CHECK(assets::length(cam.eye() - cam.focus()) == Approx(8.0f).margin(1e-3f));

    cam.rotate(10000.0f, 10000.0f);
    CHECK(cam.pitch() == Approx(cam.params().maxPitch));
    cam.rotate(0.0f, -100000.0f);
    CHECK(cam.pitch() == Approx(cam.params().minPitch));
    for (int i = 0; i < 50; ++i)
        cam.zoom(1.0f);
    CHECK(cam.targetDistance() == Approx(cam.params().minDistance));
    for (int i = 0; i < 50; ++i)
        cam.zoom(-1.0f);
    CHECK(cam.targetDistance() == Approx(cam.params().maxDistance));

    // Rotating by +90 degrees of yaw puts the camera on the -X side looking +X.
    cam.setPose(world::kPi / 2.0f, 0.0f, 5.0f);
    cam.update({10.0f, 2.0f, 10.0f}, nullptr, 1.0f / 60.0f);
    CHECK(cam.eye().x == Approx(5.0f).margin(1e-3f));
    CHECK(cam.eye().z == Approx(10.0f).margin(1e-3f));
}

TEST_CASE("OrbitCamera: terrain collision keeps the eye above the ground", "[client][world][camera]")
{
    // A steep hill rising behind the character (towards -Z).
    const auto terrain = vo::client::test::makeTerrain(
        65, 64.0f, [](float, float z) { return z < 30.0f ? 2.0f + (30.0f - z) * 1.5f : 2.0f; });
    world::OrbitCamera cam;
    cam.setPose(0.0f, 0.2f, 20.0f);
    cam.reset({32.0f, 3.5f, 32.0f}, 0.0f);
    for (int i = 0; i < 120; ++i)
    {
        cam.update({32.0f, 3.5f, 32.0f}, &terrain, 1.0f / 60.0f);
        const Vec3 e = cam.eye();
        REQUIRE(e.y >= terrain.heightAt(e.x, e.z) + cam.params().groundClearance - 1e-3f);
    }
    CHECK(cam.effectiveDistance() < 20.0f); // the boom was shortened

    // Looking up from below the focus on flat ground: the eye is lifted, never underground.
    const auto flat = vo::client::test::makeFlat(5.0f);
    cam.setPose(0.0f, cam.params().minPitch, 10.0f);
    for (int i = 0; i < 60; ++i)
        cam.update({32.0f, 5.5f, 32.0f}, &flat, 1.0f / 60.0f);
    CHECK(cam.eye().y >= 5.0f + cam.params().groundClearance - 1e-3f);

    // Obstruction gone: the boom eases back out to the wanted distance.
    cam.setPose(0.0f, 0.5f, 10.0f);
    for (int i = 0; i < 240; ++i)
        cam.update({32.0f, 7.0f, 32.0f}, &flat, 1.0f / 60.0f);
    CHECK(cam.effectiveDistance() == Approx(10.0f).margin(0.05f));
}

TEST_CASE("Picking: ray vs heightmap", "[client][world][picking]")
{
    // Plane y = 0.25 x + 4 (slope) to make the hit point non-trivial.
    const auto terrain =
        vo::client::test::makeTerrain(65, 64.0f, [](float x, float) { return 4.0f + 0.25f * x; });
    world::Ray ray;
    ray.origin = {10.0f, 40.0f, 20.0f};
    ray.direction = assets::normalize(Vec3{0.3f, -1.0f, 0.1f});
    Vec3 hit{};
    REQUIRE(world::raycastTerrain(terrain, ray, 200.0f, hit));
    CHECK(hit.y == Approx(terrain.heightAt(hit.x, hit.z)).margin(0.02f));
    // Analytic: origin + t d with 40 - t*dy = 4 + 0.25 (10 + t*dx).
    const float dx = ray.direction.x;
    const float dy = -ray.direction.y;
    const float t = (40.0f - 4.0f - 2.5f) / (dy + 0.25f * dx);
    CHECK(hit.x == Approx(10.0f + t * dx).margin(0.02f));

    // Shallow ray over a ridge hits the ridge's front face, not the ground behind it.
    const auto ridge = vo::client::test::makeTerrain(
        65, 64.0f, [](float x, float) { return (x > 30.0f && x < 34.0f) ? 12.0f : 2.0f; });
    world::Ray shallow;
    shallow.origin = {5.0f, 8.0f, 10.0f};
    shallow.direction = assets::normalize(Vec3{1.0f, -0.02f, 0.0f});
    REQUIRE(world::raycastTerrain(ridge, shallow, 200.0f, hit));
    CHECK(hit.x < 31.0f);
    CHECK(hit.x > 29.0f);

    // Pointing at the sky: no hit. Outside the zone: no hit.
    world::Ray up;
    up.origin = {10.0f, 40.0f, 10.0f};
    up.direction = {0.0f, 1.0f, 0.0f};
    CHECK_FALSE(world::raycastTerrain(terrain, up, 200.0f, hit));
    world::Ray away;
    away.origin = {-10.0f, 40.0f, 10.0f};
    away.direction = {-1.0f, 0.0f, 0.0f};
    CHECK_FALSE(world::raycastTerrain(terrain, away, 200.0f, hit));

    // Label occlusion helper.
    CHECK(world::segmentClearOfTerrain(ridge, {5.0f, 15.0f, 10.0f}, {50.0f, 15.0f, 10.0f}, 0.5f));
    CHECK_FALSE(world::segmentClearOfTerrain(ridge, {5.0f, 5.0f, 10.0f}, {50.0f, 5.0f, 10.0f}, 0.5f));
}
