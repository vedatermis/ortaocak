// Movement validator (server/world/MoveValidator): the §5.4 formula at its boundaries, walk vs run,
// zone bounds, walk mask incl. tunnelling, height tolerance and snapping, the capped distance budget
// (no banked teleports, but lag bursts pass), packet spam, plausibility (NaN, absurd values, flags).

#include "server/world/MoveValidator.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>

namespace vw = vo::server::world;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using V = vw::MoveVerdict;

namespace {

struct Fixture
{
    vo::assets::TerrainData terrain = vo::tests::makeTestTerrain();
    vw::MoveRules rules;
    vw::MoveState state;
    vo::core::MonoTime t0 = vo::core::monoNow();
    std::uint32_t seq = 0;

    Fixture()
    {
        state.x = 200.0f;
        state.z = 200.0f;
        state.y = terrain.heightAt(200.0f, 200.0f);
        state.lastTime = t0;
    }

    // Move to (x, z) at t0 + at with y on the terrain.
    V move(float x, float z, vo::core::MonoClock::duration at, std::uint8_t flags = vg::kMoveMoving | vg::kMoveRunning,
           float yOffset = 0.0f, float yaw = 0.0f)
    {
        vn::MoveReq r;
        r.seq = ++seq;
        r.flags = flags;
        r.x = x;
        r.z = z;
        r.y = terrain.heightAt(x, z) + yOffset;
        r.yaw = yaw;
        return vw::validateMove(terrain, rules, state, r, t0 + at);
    }
};

vn::MoveReq req(float x, float y, float z, float yaw = 0.0f, std::uint8_t flags = 1)
{
    vn::MoveReq r;
    r.seq = 1;
    r.flags = flags;
    r.x = x;
    r.y = y;
    r.z = z;
    r.yaw = yaw;
    return r;
}

} // namespace

TEST_CASE("MoveValidator: distance bound speed x elapsed x 1.25 + 0.5 m", "[server][world][move]")
{
    Fixture f;
    SECTION("run: just inside the bound is accepted")
    {
        const float bound = vg::maxMoveDistance(vg::kRunSpeed, 0.1f); // 1.1875 m
        CHECK(f.move(200.0f + bound - 0.001f, 200.0f, 100ms) == V::Accepted);
        CHECK(f.state.x == 200.0f + bound - 0.001f);
    }
    SECTION("run: just outside the bound is rejected, position unchanged")
    {
        const float bound = vg::maxMoveDistance(vg::kRunSpeed, 0.1f);
        CHECK(f.move(200.0f + bound + 0.002f, 200.0f, 100ms) == V::TooFar);
        CHECK(f.state.x == 200.0f);
        CHECK(f.state.z == 200.0f);
    }
    SECTION("walk flags use the walk speed")
    {
        const float walkBound = vg::maxMoveDistance(vg::kWalkSpeed, 0.5f); // 1.75 m
        CHECK(f.move(200.0f, 200.0f + walkBound + 0.01f, 500ms, vg::kMoveMoving) == V::TooFar);
        Fixture g;
        CHECK(g.move(200.0f, 200.0f + walkBound - 0.01f, 500ms, vg::kMoveMoving) == V::Accepted);
        Fixture h; // the same distance is fine when running
        CHECK(h.move(200.0f, 200.0f + walkBound + 0.01f, 500ms) == V::Accepted);
    }
    SECTION("diagonal distance is horizontal (x/z), not per axis")
    {
        const float bound = vg::maxMoveDistance(vg::kRunSpeed, 0.2f); // 1.875 m
        const float d = (bound + 0.01f) / std::numbers::sqrt2_v<float>;
        CHECK(f.move(200.0f + d, 200.0f + d, 200ms) == V::TooFar);
        Fixture g;
        const float e = (bound - 0.01f) / std::numbers::sqrt2_v<float>;
        CHECK(g.move(200.0f + e, 200.0f + e, 200ms) == V::Accepted);
    }
    SECTION("zero elapsed time still allows the 0.5 m slack")
    {
        CHECK(f.move(200.49f, 200.0f, 0ms) == V::Accepted);
        Fixture g;
        CHECK(g.move(200.51f, 200.0f, 0ms) == V::TooFar);
    }
    SECTION("a steady legal walker is never rejected")
    {
        for (int i = 1; i <= 600; ++i) // 60 s at 10 Hz and exactly run speed
            REQUIRE(f.move(200.0f + 0.55f * static_cast<float>(i) * 0.5f, 200.0f + 0.55f * static_cast<float>(i) * 0.866f,
                           100ms * i) == V::Accepted);
    }
}

TEST_CASE("MoveValidator: banked time is capped, lag bursts are accepted", "[server][world][move]")
{
    SECTION("standing still for 60 s does not allow a teleport")
    {
        Fixture f;
        // 60 s of credit would be 412 m by the literal formula; the cap is 1 s: 6.875 + 0.5 m.
        CHECK(f.move(215.0f, 200.0f, 60s) == V::TooFar);
        CHECK(f.state.x == 200.0f);
        Fixture g;
        CHECK(g.move(207.3f, 200.0f, 60s) == V::Accepted); // within the capped budget
        Fixture h;
        CHECK(h.move(207.45f, 200.0f, 60s) == V::TooFar);
    }
    SECTION("the bank cap is configurable")
    {
        Fixture f;
        f.rules.bankSeconds = 2.0f;
        CHECK(f.move(213.0f, 200.0f, 10s) == V::Accepted); // 2 s x 5.5 x 1.25 + 0.5 = 14.25 m
    }
    SECTION("a 1 s stall followed by a burst of the delayed packets is accepted")
    {
        Fixture f;
        // Ten packets 0.55 m apart (10 Hz at run speed) all arrive within 2 ms after a 1 s stall.
        for (int i = 1; i <= 10; ++i)
            REQUIRE(f.move(200.0f + 0.55f * static_cast<float>(i), 200.0f, 1000ms + 200us * i) == V::Accepted);
        CHECK_THAT(f.state.x, WithinAbs(205.5, 1e-4));
    }
    SECTION("a burst is not a loophole: total distance stays bounded")
    {
        Fixture f;
        int accepted = 0;
        for (int i = 1; i <= 30; ++i)
            accepted += f.move(200.0f + 0.55f * static_cast<float>(i), 200.0f, 1000ms) == V::Accepted ? 1 : 0;
        // 1 s of credit (6.875 m) + 0.5 m slack: at most 13 packets of 0.55 m.
        CHECK(accepted == 13);
    }
    SECTION("spamming small moves cannot exceed 1.25 x speed (slack is not per packet)")
    {
        Fixture f;
        // 30 packets per second, each 0.45 m (< 0.5 m slack): 13.5 m/s if the slack were per packet.
        float x = 200.0f;
        for (int i = 1; i <= 300; ++i)
        {
            const float target = x + 0.45f;
            if (f.move(target, 200.0f, std::chrono::microseconds(33'333) * i) == V::Accepted)
                x = target;
        }
        const float travelled = x - 200.0f;
        // 10 s at run speed x 1.25 = 68.75 m, plus 0.5 m slack.
        CHECK(travelled <= 69.25f + 0.01f);
        CHECK(travelled > 60.0f);
    }
}

TEST_CASE("MoveValidator: bounds, walk mask, tunnelling, height", "[server][world][move]")
{
    Fixture f;
    vo::tests::blockCell(f.terrain, 201.5f, 200.5f); // blocked cell (201, 200), right next to the start
    SECTION("outside the zone")
    {
        Fixture g;
        g.state.x = 0.3f;
        g.state.z = 10.0f;
        CHECK(g.move(-0.1f, 10.0f, 1s) == V::OutOfBounds);
        CHECK(g.move(0.3f, -0.01f, 2s) == V::OutOfBounds);
        g.state.x = 511.8f;
        CHECK(g.move(512.0f, 10.0f, 3s) == V::OutOfBounds); // worldSize itself is outside
        CHECK(g.move(511.99f, 10.0f, 4s) == V::Accepted);
    }
    SECTION("into a blocked cell")
    {
        CHECK(f.move(201.5f, 200.5f, 1s) == V::Blocked);
        CHECK(f.state.x == 200.0f);
    }
    SECTION("through a blocked cell (both end points walkable)")
    {
        f.state.x = 200.5f;
        f.state.z = 200.5f;
        CHECK(f.move(202.5f, 200.5f, 1s) == V::PathBlocked);
        CHECK(f.state.x == 200.5f);
        CHECK(f.move(202.5f, 201.5f, 2s) == V::PathBlocked); // still clips the blocked cell's corner row
        CHECK(f.move(201.5f, 202.5f, 3s) == V::Accepted);    // around it
    }
    SECTION("height: within 1 m is accepted and snapped, beyond is rejected")
    {
        auto sloped = vo::tests::makeTestTerrain({.slope = 0.25f});
        vw::MoveState s;
        s.x = 200.0f;
        s.z = 200.0f;
        s.y = sloped.heightAt(200.0f, 200.0f);
        s.lastTime = f.t0;
        const float ground = sloped.heightAt(201.0f, 200.0f);
        CHECK(vw::validateMove(sloped, f.rules, s, req(201.0f, ground + 0.99f, 200.0f), f.t0 + 1s) == V::Accepted);
        CHECK_THAT(s.y, WithinAbs(ground, 1e-5));
        const float ground2 = sloped.heightAt(202.0f, 200.0f);
        CHECK(vw::validateMove(sloped, f.rules, s, req(202.0f, ground2 - 1.01f, 200.0f), f.t0 + 2s) ==
              V::HeightMismatch);
        CHECK(vw::validateMove(sloped, f.rules, s, req(202.0f, ground2 + 1.5f, 200.0f), f.t0 + 3s) ==
              V::HeightMismatch);
        CHECK(s.x == 201.0f);
    }
}

TEST_CASE("MoveValidator: accepted state, yaw, flags", "[server][world][move]")
{
    Fixture f;
    CHECK(f.move(200.5f, 200.5f, 1s, vg::kMoveMoving, 0.0f, 7.0f) == V::Accepted);
    CHECK_THAT(f.state.yaw, WithinAbs(vg::normalizeYaw(7.0f), 1e-6));
    CHECK(f.state.yaw <= std::numbers::pi_v<float>);
    CHECK(f.state.flags == vg::kMoveMoving);
    // A rejected move leaves the player standing at the old position (moving cleared).
    CHECK(f.move(260.0f, 200.5f, 1100ms, vg::kMoveMoving | vg::kMoveRunning) == V::TooFar);
    CHECK(f.state.flags == 0);
    CHECK(f.state.x == 200.5f);
    // Time does not run backwards: an older timestamp counts as no elapsed time.
    CHECK(f.move(200.9f, 200.5f, 500ms) == V::Accepted);
    CHECK(f.state.lastTime == f.t0 + 1100ms);
}

TEST_CASE("isPlausibleMove rejects what no client can send", "[server][world][move]")
{
    std::string_view why;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(vw::isPlausibleMove(req(1.0f, 2.0f, 3.0f, 0.5f, 3), why));
    CHECK(vw::isPlausibleMove(req(-5000.0f, 0.0f, 99999.0f, -9000.0f, 0), why)); // out of the zone, but finite
    CHECK_FALSE(vw::isPlausibleMove(req(nan, 0.0f, 0.0f), why));
    CHECK(why.find("non-finite") != std::string_view::npos);
    CHECK_FALSE(vw::isPlausibleMove(req(0.0f, nan, 0.0f), why));
    CHECK_FALSE(vw::isPlausibleMove(req(0.0f, 0.0f, inf), why));
    CHECK_FALSE(vw::isPlausibleMove(req(0.0f, 0.0f, 0.0f, -inf), why));
    CHECK_FALSE(vw::isPlausibleMove(req(1e6f, 0.0f, 0.0f), why));
    CHECK_FALSE(vw::isPlausibleMove(req(0.0f, -2e5f, 0.0f), why));
    CHECK_FALSE(vw::isPlausibleMove(req(0.0f, 0.0f, 0.0f, 1e5f), why));
    CHECK_FALSE(vw::isPlausibleMove(req(0.0f, 0.0f, 0.0f, 0.0f, 0x04), why));
    CHECK(why.find("flag") != std::string_view::npos);
    CHECK_FALSE(vw::isPlausibleMove(req(0.0f, 0.0f, 0.0f, 0.0f, 0x80), why));
}
