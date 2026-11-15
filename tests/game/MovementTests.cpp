// shared/game movement rules: constants, yaw normalisation, the per-move bound and straight-segment
// walkability on a walk mask (incl. tunnelling and corner squeezing).

#include "shared/game/Movement.h"
#include "shared/game/TerrainWalk.h"
#include "shared/net/Opcodes.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <random>

namespace vg = vo::game;
using Catch::Matchers::WithinAbs;

TEST_CASE("Movement constants match the protocol", "[game][movement]")
{
    STATIC_CHECK(vg::kWalkSpeed == 2.0f);
    STATIC_CHECK(vg::kRunSpeed == 5.5f);
    STATIC_CHECK(vg::kMoveMoving == vo::net::kMoveFlagMoving);
    STATIC_CHECK(vg::kMoveRunning == vo::net::kMoveFlagRunning);
    STATIC_CHECK(vg::kMaxMoveReqPerSecond == static_cast<int>(vo::net::kMaxMoveReqPerSecond));
    STATIC_CHECK(vg::kAoiCellSize == 32.0f);
    STATIC_CHECK(vg::kAoiViewRadiusCells == 2);
    STATIC_CHECK(vg::moveSpeed(0) == vg::kWalkSpeed);
    STATIC_CHECK(vg::moveSpeed(vg::kMoveMoving) == vg::kWalkSpeed);
    STATIC_CHECK(vg::moveSpeed(vg::kMoveRunning) == vg::kRunSpeed);
    STATIC_CHECK(vg::moveSpeed(vg::kMoveMoving | vg::kMoveRunning) == vg::kRunSpeed);
    // speed x elapsed x 1.25 + 0.5 m
    CHECK_THAT(vg::maxMoveDistance(5.5f, 0.1f), WithinAbs(1.1875, 1e-6));
    CHECK_THAT(vg::maxMoveDistance(2.0f, 1.0f), WithinAbs(3.0, 1e-6));
    CHECK_THAT(vg::maxMoveDistance(5.5f, 0.0f), WithinAbs(0.5, 1e-6));
}

TEST_CASE("normalizeYaw maps to (-pi, pi]", "[game][movement]")
{
    constexpr float kPi = std::numbers::pi_v<float>;
    CHECK(vg::normalizeYaw(0.0f) == 0.0f);
    CHECK_THAT(vg::normalizeYaw(1.0f), WithinAbs(1.0, 1e-6));
    CHECK_THAT(vg::normalizeYaw(2.0f * kPi + 0.5f), WithinAbs(0.5, 1e-5));
    CHECK_THAT(vg::normalizeYaw(-2.0f * kPi - 0.5f), WithinAbs(-0.5, 1e-5));
    CHECK_THAT(vg::normalizeYaw(3.0f * kPi), WithinAbs(kPi, 1e-5));
    CHECK_THAT(vg::normalizeYaw(-kPi), WithinAbs(kPi, 1e-6));
    CHECK_THAT(vg::normalizeYaw(1000.25f), WithinAbs(std::remainder(1000.25, 2.0 * std::numbers::pi), 1e-3));
    CHECK(vg::normalizeYaw(std::nanf("")) == 0.0f);
    CHECK(vg::normalizeYaw(INFINITY) == 0.0f);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> any(-500.0f, 500.0f);
    for (int i = 0; i < 1000; ++i)
    {
        const float y = vg::normalizeYaw(any(rng));
        CHECK(y > -kPi);
        CHECK(y <= kPi);
    }
}

TEST_CASE("yawFromDirection: 0 = +Z, clockwise from above", "[game][movement]")
{
    constexpr float kPi = std::numbers::pi_v<float>;
    CHECK_THAT(vg::yawFromDirection(0.0f, 1.0f), WithinAbs(0.0, 1e-6));
    CHECK_THAT(vg::yawFromDirection(1.0f, 0.0f), WithinAbs(kPi / 2.0f, 1e-6)); // +X is a quarter turn clockwise
    CHECK_THAT(vg::yawFromDirection(-1.0f, 0.0f), WithinAbs(-kPi / 2.0f, 1e-6));
}

TEST_CASE("isSegmentWalkable on the walk mask", "[game][movement][terrain]")
{
    auto t = vo::tests::makeTestTerrain(); // 512 m, 1 m mask cells, all walkable
    vo::tests::blockCell(t, 100.5f, 100.5f); // a single blocked cell (100, 100)

    SECTION("open ground")
    {
        CHECK(vg::isSegmentWalkable(t, 10.2f, 10.2f, 10.7f, 10.9f)); // same cell
        CHECK(vg::isSegmentWalkable(t, 10.0f, 10.0f, 60.0f, 35.0f));
        CHECK(vg::isSegmentWalkable(t, 99.5f, 99.5f, 99.5f, 99.5f));
    }
    SECTION("blocked end points and outside the zone")
    {
        CHECK_FALSE(vg::isSegmentWalkable(t, 99.5f, 100.5f, 100.5f, 100.5f)); // into the blocked cell
        CHECK_FALSE(vg::isSegmentWalkable(t, 100.5f, 100.5f, 101.5f, 100.5f)); // out of it
        CHECK_FALSE(vg::isSegmentWalkable(t, 1.0f, 1.0f, -0.1f, 1.0f));
        CHECK_FALSE(vg::isSegmentWalkable(t, 511.5f, 1.0f, 512.0f, 1.0f)); // x = worldSize is outside
    }
    SECTION("tunnelling through a blocked cell is refused")
    {
        CHECK_FALSE(vg::isSegmentWalkable(t, 99.5f, 100.5f, 101.5f, 100.5f));  // along X
        CHECK_FALSE(vg::isSegmentWalkable(t, 100.5f, 99.5f, 100.5f, 101.5f));  // along Z
        CHECK_FALSE(vg::isSegmentWalkable(t, 99.2f, 99.6f, 101.8f, 101.1f));   // oblique
        CHECK_FALSE(vg::isSegmentWalkable(t, 90.0f, 100.5f, 110.0f, 100.5f));  // long
        CHECK(vg::isSegmentWalkable(t, 99.5f, 101.5f, 101.5f, 101.5f));        // one row further: free
    }
    SECTION("passing exactly through a corner needs both side cells")
    {
        // The diagonal through the corner (101, 101) touches cells (100,100) [blocked] and (101,101);
        // its side cells (100,101) and (101,100) are open, but the path also starts in the blocked cell
        // here, so use the corner (100, 100) between (99,99) and (100,100) instead:
        CHECK_FALSE(vg::isSegmentWalkable(t, 99.5f, 99.5f, 100.5f, 100.5f)); // ends in the blocked cell
        // Squeeze between two diagonal blocked cells: (100,100) and (101,101) blocked; going from
        // (101,100) to (100,101) through their shared corner is refused.
        vo::tests::blockCell(t, 101.5f, 101.5f);
        CHECK_FALSE(vg::isSegmentWalkable(t, 101.5f, 100.5f, 100.5f, 101.5f));
        // A diagonal corner with both side cells open is fine.
        CHECK(vg::isSegmentWalkable(t, 50.5f, 50.5f, 51.5f, 51.5f));
    }
    SECTION("every visited cell matches a fine sampling of the segment")
    {
        // Random mask, random segments: whenever a dense sampling hits a blocked cell the exact
        // traversal must refuse the segment too (the traversal may refuse more: corner touches).
        auto r = vo::tests::makeTestTerrain({.worldSize = 64.0f, .maskResolution = 64, .aydorX = 1.5f,
                                             .aydorZ = 1.5f, .karganX = 2.5f, .karganZ = 2.5f});
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> coin(0, 9);
        for (auto& v : r.walkMask)
            v = coin(rng) == 0 ? 0 : 1;
        std::uniform_real_distribution<float> pos(0.0f, 63.99f);
        int refused = 0;
        for (int n = 0; n < 3000; ++n)
        {
            const float x0 = pos(rng), z0 = pos(rng);
            const float x1 = x0 + (pos(rng) - 32.0f) / 4.0f, z1 = z0 + (pos(rng) - 32.0f) / 4.0f;
            bool sampledFree = r.isWalkable(x0, z0) && r.isWalkable(x1, z1);
            for (int k = 0; k <= 400 && sampledFree; ++k)
            {
                const float f = static_cast<float>(k) / 400.0f;
                sampledFree = r.isWalkable(x0 + (x1 - x0) * f, z0 + (z1 - z0) * f);
            }
            const bool exact = vg::isSegmentWalkable(r, x0, z0, x1, z1);
            if (!sampledFree)
            {
                CHECK_FALSE(exact);
                ++refused;
            }
        }
        CHECK(refused > 100);
    }
}
