// Local movement prediction, MOVE_REQ scheduling and POSITION_CORRECT handling
// (client/world/LocalMovement.h, WorldModel.h).
#include "client/world/LocalMovement.h"
#include "client/world/WorldModel.h"
#include "tests/client/WorldTestUtil.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using Catch::Approx;
using vo::client::world::LocalMover;
using vo::client::world::MoveIntent;
using vo::client::world::MoveReqData;
using vo::client::world::MoveSender;
using vo::client::world::MovementParams;
using vo::client::world::Prediction;
using vo::client::world::Vec3;
namespace world = vo::client::world;

namespace {

MoveIntent forward(float cameraYaw = 0.0f, bool running = true)
{
    MoveIntent in;
    in.forward = 1.0f;
    in.cameraYaw = cameraYaw;
    in.running = running;
    return in;
}

} // namespace

TEST_CASE("LocalMover: WASD moves relative to the camera at walk/run speed, y follows terrain", "[client][world][movement]")
{
    const auto terrain = vo::client::test::makeTerrain(65, 64.0f, [](float x, float) { return 10.0f + 0.25f * x; });
    MovementParams params;
    params.walkSpeed = 2.0f;
    params.runSpeed = 5.0f;
    LocalMover m;
    m.reset({20.0f, 0.0f, 20.0f}, 0.0f);

    // Camera looks along +X (yaw 90 deg): W moves +X.
    for (int i = 0; i < 60; ++i)
        m.step(forward(world::kPi / 2.0f, true), params, &terrain, 1.0f / 60.0f);
    CHECK(m.position().x == Approx(25.0f).margin(0.02f));
    CHECK(m.position().z == Approx(20.0f).margin(0.02f));
    CHECK(m.position().y == Approx(terrain.heightAt(m.position().x, m.position().z)).margin(1e-4f));
    CHECK(m.moving());
    CHECK(m.speed() == Approx(5.0f).margin(0.05f));
    // Facing turned towards the travel direction.
    CHECK(m.yaw() == Approx(world::kPi / 2.0f).margin(0.01f));

    // Walk: D strafes right relative to the camera (camera yaw 0 -> +X).
    MoveIntent strafe;
    strafe.right = 1.0f;
    strafe.running = false;
    const Vec3 before = m.position();
    for (int i = 0; i < 30; ++i)
        m.step(strafe, params, &terrain, 1.0f / 60.0f);
    CHECK(m.position().x - before.x == Approx(1.0f).margin(0.02f));
    CHECK(m.speed() == Approx(2.0f).margin(0.05f));

    // No input: stops.
    m.step(MoveIntent{}, params, &terrain, 1.0f / 60.0f);
    CHECK_FALSE(m.moving());
    CHECK(m.speed() == 0.0f);
}

TEST_CASE("LocalMover: blocked cells slide along the free axis instead of sticking", "[client][world][movement]")
{
    // A wall of blocked cells at x = 30 (all z).
    const auto terrain = vo::client::test::makeTerrain(65, 64.0f, [](float, float) { return 5.0f; },
                                                       [](int cx, int) { return cx == 30; });
    MovementParams params;
    params.runSpeed = 4.0f;
    LocalMover m;
    m.reset({28.5f, 5.0f, 20.0f}, 0.0f);

    // Diagonal (+X +Z) into the wall: x stops before the wall, z keeps going.
    MoveIntent diag;
    diag.forward = 1.0f;
    diag.cameraYaw = world::kPi / 4.0f;
    for (int i = 0; i < 120; ++i)
        m.step(diag, params, &terrain, 1.0f / 60.0f);
    CHECK(m.position().x < 30.0f);
    CHECK(m.position().x > 29.5f);
    CHECK(m.position().z > 24.0f); // slid ~ 5.7 m along z
    CHECK(terrain.isWalkable(m.position().x, m.position().z));
    CHECK(m.blocked());
    CHECK(m.moving());

    // Straight into the wall: blocked, no movement through it.
    LocalMover h;
    h.reset({29.5f, 5.0f, 20.0f}, 0.0f);
    for (int i = 0; i < 60; ++i)
        h.step(forward(world::kPi / 2.0f), params, &terrain, 1.0f / 60.0f);
    CHECK(h.position().x < 30.0f);
    CHECK_FALSE(h.moving());

    // Zone bounds: cannot leave [0, size).
    LocalMover b;
    b.reset({1.0f, 5.0f, 10.0f}, 0.0f);
    for (int i = 0; i < 120; ++i)
        b.step(forward(-world::kPi / 2.0f), params, &terrain, 1.0f / 60.0f);
    CHECK(b.position().x >= 0.0f);
    CHECK(b.position().x < 1.0f);
}

TEST_CASE("LocalMover: a corner hit diagonally is rounded, never entered", "[client][world][movement]")
{
    // A 6 x 6 m blocked block (a house footprint) at cells 30..35.
    const auto terrain = vo::client::test::makeTerrain(65, 64.0f, [](float, float) { return 5.0f; },
                                                       [](int cx, int cz) { return cx >= 30 && cx <= 35 && cz >= 30 && cz <= 35; });
    MovementParams params;
    params.runSpeed = 4.0f;
    LocalMover m;
    m.reset({27.0f, 5.0f, 27.0f}, 0.0f);
    MoveIntent diag;
    diag.forward = 1.0f;
    diag.cameraYaw = world::kPi / 4.0f; // straight at the block's corner (30, 30)
    for (int i = 0; i < 300; ++i)
    {
        m.step(diag, params, &terrain, 1.0f / 60.0f);
        REQUIRE(terrain.isWalkable(m.position().x, m.position().z));
    }
    INFO("ended at " << m.position().x << ", " << m.position().z);
    // 20 m of wish: it got past the block along one of its faces.
    CHECK((m.position().x > 36.0f || m.position().z > 36.0f));
    CHECK(m.moving());
}

TEST_CASE("LocalMover: click-to-move walks to the target, WASD cancels it", "[client][world][movement]")
{
    const auto terrain = vo::client::test::makeFlat();
    MovementParams params;
    LocalMover m;
    m.reset({10.0f, 10.0f, 10.0f}, 0.0f);
    m.setDestination(14.0f, 13.0f);
    MoveIntent idle;
    idle.running = false;
    for (int i = 0; i < 400 && m.destination(); ++i)
        m.step(idle, params, &terrain, 1.0f / 60.0f);
    CHECK_FALSE(m.destination().has_value());
    CHECK(m.position().x == Approx(14.0f).margin(0.2f));
    CHECK(m.position().z == Approx(13.0f).margin(0.2f));

    m.setDestination(30.0f, 30.0f);
    m.step(idle, params, &terrain, 1.0f / 60.0f);
    CHECK(m.destination().has_value());
    m.step(forward(), params, &terrain, 1.0f / 60.0f);
    CHECK_FALSE(m.destination().has_value());

    // An unreachable target (inside a blocked block) gives up instead of pushing forever.
    const auto walled = vo::client::test::makeTerrain(65, 64.0f, [](float, float) { return 0.0f; },
                                                      [](int cx, int cz) { return cx >= 40 && cz >= 0; });
    LocalMover w;
    w.reset({35.0f, 0.0f, 10.0f}, 0.0f);
    w.setDestination(50.0f, 10.0f);
    for (int i = 0; i < 600; ++i)
        w.step(idle, params, &walled, 1.0f / 60.0f);
    CHECK_FALSE(w.destination().has_value());
    CHECK(w.position().x < 40.0f);
}

TEST_CASE("MoveSender: immediate start/stop/toggle, 10 Hz while moving, seq increments", "[client][world][movement]")
{
    MoveSender s;
    const Vec3 p{1.0f, 2.0f, 3.0f};
    double t = 0.0;
    constexpr double kDt = 1.0 / 60.0;
    // Standing still: nothing to send.
    for (int i = 0; i < 30; ++i, t += kDt)
        CHECK_FALSE(s.update(t, false, true, p, 0.0f).has_value());

    // Start: immediate packet with moving|running.
    auto start = s.update(t, true, true, p, 0.5f);
    REQUIRE(start);
    CHECK(start->seq == 1);
    CHECK(start->flags == 0x03);
    CHECK(start->yaw == 0.5f);

    // One second of moving at 60 fps: exactly 10 periodic packets, seq strictly increasing.
    std::vector<MoveReqData> sent;
    for (int i = 0; i < 60; ++i)
    {
        t += kDt;
        if (auto r = s.update(t, true, true, p, 0.0f))
            sent.push_back(*r);
    }
    CHECK(sent.size() == 10);
    for (std::size_t i = 0; i < sent.size(); ++i)
    {
        CHECK(sent[i].seq == 2 + i);
        CHECK(sent[i].flags == 0x03);
    }

    // "Immediate" = the first frame the 40 ms rate guard allows (here: within 3 frames at 60 fps).
    auto sendWithin = [&](int frames, bool moving, bool running) -> std::optional<MoveReqData>
    {
        for (int i = 0; i < frames; ++i)
        {
            t += kDt;
            if (auto r = s.update(t, moving, running, p, 0.0f))
                return r;
        }
        return std::nullopt;
    };

    // Run -> walk toggle while moving: immediate, flags moving only.
    auto toggle = sendWithin(3, true, false);
    REQUIRE(toggle);
    CHECK(toggle->flags == 0x01);
    CHECK(toggle->seq == 12);

    // Stop: immediate, moving = 0.
    auto stop = sendWithin(3, false, false);
    REQUIRE(stop);
    CHECK(stop->flags == 0x00);
    CHECK(stop->seq == 13);
    // Standing again: silence.
    for (int i = 0; i < 120; ++i, t += kDt)
        CHECK_FALSE(s.update(t, false, false, p, 0.0f).has_value());
    // Toggle while standing is sent too (as specified: "immediately on ... run/walk toggle").
    auto standToggle = s.update(t, false, true, p, 0.0f);
    REQUIRE(standToggle);
    CHECK(standToggle->flags == 0x02);
}

TEST_CASE("MoveSender: rate stays below the server flood limit at any frame rate", "[client][world][movement]")
{
    MoveSender s;
    const Vec3 p{};
    // 1000 fps with the player toggling run every frame for a second: still <= 25 packets.
    double t = 0.0;
    int count = 0;
    for (int i = 0; i < 1000; ++i, t += 0.001)
    {
        if (s.update(t, true, (i % 2) == 0, p, 0.0f))
            ++count;
    }
    CHECK(count <= 26);
    CHECK(count >= 20);

    // 20 fps moving: one packet per 100 ms frame (10 Hz), no bursts.
    MoveSender slow;
    t = 0.0;
    count = 0;
    for (int i = 0; i < 40; ++i, t += 0.05)
    {
        if (slow.update(t, true, true, p, 0.0f))
            ++count;
    }
    CHECK(count >= 19);
    CHECK(count <= 21);
}

TEST_CASE("Prediction: POSITION_CORRECT snaps, blends visually, drops the click target, ignores stale seqs",
          "[client][world][movement]")
{
    const auto terrain = vo::client::test::makeFlat(3.0f);
    MovementParams params;
    Prediction pred;
    pred.reset({10.0f, 3.0f, 10.0f}, 0.0f, true);
    double t = 0.0;
    std::uint32_t lastSeq = 0;
    for (int i = 0; i < 30; ++i, t += 1.0 / 60.0)
    {
        if (auto r = pred.update(t, forward(), params, &terrain, 1.0f / 60.0f))
            lastSeq = r->seq;
    }
    REQUIRE(lastSeq >= 2);
    pred.mover().setDestination(40.0f, 40.0f);
    const Vec3 visualBefore = pred.visualPosition();

    // The server rejected seq 1 and puts us back at (10, y, 10). y comes from the terrain.
    REQUIRE(pred.applyCorrection(1, {10.0f, 99.0f, 10.0f}, 0.25f, &terrain));
    CHECK(pred.mover().position().x == Approx(10.0f));
    CHECK(pred.mover().position().z == Approx(10.0f));
    CHECK(pred.mover().position().y == Approx(3.0f).margin(0.01f));
    CHECK(pred.mover().yaw() == Approx(0.25f));
    CHECK_FALSE(pred.mover().destination().has_value());
    // Visual position starts where the character was drawn, then converges.
    CHECK(pred.visualPosition().z == Approx(visualBefore.z).margin(1e-3f));
    for (int i = 0; i < 30; ++i, t += 1.0 / 60.0)
        (void)pred.update(t, MoveIntent{}, params, &terrain, 1.0f / 60.0f);
    CHECK(world::horizontalDistance(pred.visualPosition(), pred.mover().position()) < 0.01f);

    // Corrections answering packets sent before the snap are stale.
    CHECK_FALSE(pred.applyCorrection(lastSeq, {0.0f, 0.0f, 0.0f}, 0.0f, &terrain));
    CHECK(pred.correctionsIgnored() == 1);
    CHECK(pred.mover().position().x == Approx(10.0f));

    // A correction for a packet sent after the snap applies again.
    std::uint32_t newSeq = 0;
    for (int i = 0; i < 30; ++i, t += 1.0 / 60.0)
    {
        if (auto r = pred.update(t, forward(), params, &terrain, 1.0f / 60.0f))
            newSeq = r->seq;
    }
    REQUIRE(newSeq > lastSeq);
    CHECK(pred.applyCorrection(newSeq, {12.0f, 0.0f, 12.0f}, 0.0f, &terrain));
    CHECK(pred.correctionsApplied() == 2);
    CHECK(pred.mover().position().x == Approx(12.0f));
}

TEST_CASE("WorldModel: enter snaps y to the terrain and MOVE_REQ carries the prediction",
          "[client][world][movement]")
{
    const auto terrain = vo::client::test::makeTerrain(65, 64.0f, [](float x, float) { return 4.0f + 0.5f * x; });
    world::WorldModel model;
    vo::net::EnterWorldRes res;
    res.entityId = 7;
    res.zoneId = 1;
    res.x = 20.0f;
    res.y = 0.0f; // the server's y is ignored
    res.z = 20.0f;
    res.walkSpeed = 2.5f;
    res.runSpeed = 6.0f;
    res.serverTimeMs = 1000;
    model.enter(res, &terrain, 0.0, true);
    CHECK(model.local().mover().position().y == Approx(14.0f).margin(0.01f));
    CHECK(model.params().walkSpeed == 2.5f);
    CHECK(model.params().runSpeed == 6.0f);

    auto req = model.updateLocal(0.016, forward(), 0.016f);
    REQUIRE(req);
    CHECK(req->seq == 1);
    CHECK(req->flags == (vo::net::kMoveFlagMoving | vo::net::kMoveFlagRunning));
    CHECK(req->y == Approx(terrain.heightAt(req->x, req->z)).margin(1e-3f));

    // POSITION_CORRECT through the message path.
    vo::net::PositionCorrect pc;
    pc.seq = 1;
    pc.x = 20.0f;
    pc.y = 0.0f;
    pc.z = 20.0f;
    auto payload = vo::net::encodeMessage(pc);
    REQUIRE(payload);
    CHECK(model.handleMessage(static_cast<std::uint16_t>(vo::net::Opcode::PositionCorrect), *payload, 0.02));
    CHECK(model.stats().corrections == 1);
    CHECK(model.local().mover().position().z == Approx(20.0f));
    // Malformed payload is reported, not applied.
    std::vector<std::uint8_t> bad{1, 2, 3};
    CHECK_FALSE(model.handleMessage(static_cast<std::uint16_t>(vo::net::Opcode::PositionCorrect), bad, 0.03));
    CHECK(model.stats().decodeErrors == 1);
}
