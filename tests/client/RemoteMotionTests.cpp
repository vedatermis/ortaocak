// Remote entity snapshot interpolation / extrapolation and the entity message handlers
// (client/world/RemoteMotion.h, WorldModel.h, DebugGhosts.h).
#include "client/world/DebugGhosts.h"
#include "client/world/RemoteMotion.h"
#include "client/world/WorldModel.h"
#include "tests/client/WorldTestUtil.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <vector>

using Catch::Approx;
using vo::client::world::ServerClock;
using vo::client::world::SnapshotBuffer;
using vo::client::world::Vec3;
namespace world = vo::client::world;
namespace vn = vo::net;

TEST_CASE("ServerClock: offset from the least-delayed packet, unwraps u32 stamps", "[client][world][remote]")
{
    ServerClock c;
    c.observe(10'000, 100.0); // server 10 s at local 100 s
    CHECK(c.now(100.0) == Approx(0.0)); // relative to the first stamp
    c.observe(10'150, 100.2);           // 50 ms more delay: offset kept (max)
    CHECK(c.now(100.2) == Approx(0.2).margin(0.003)); // decays at most 10 ms/s towards the slower packet
    c.observe(10'300, 100.25); // less delay: offset jumps forward
    CHECK(c.now(100.25) == Approx(0.3));
    CHECK(c.toSeconds(10'500) == Approx(0.5));
    CHECK(c.toServerMs(0.5) == 10'500u);

    ServerClock w;
    w.observe(0xFFFFFF00u, 0.0);
    CHECK(w.toSeconds(0x00000100u) == Approx(0.512)); // across the wrap
}

TEST_CASE("SnapshotBuffer: interpolates between the bracketing snapshots", "[client][world][remote]")
{
    SnapshotBuffer b;
    b.push(1.0, {0.0f, 0.0f, 0.0f}, 0.0f, 0x01);
    b.push(1.1, {1.0f, 0.0f, 0.0f}, 0.2f, 0x01);
    b.push(1.2, {2.0f, 0.0f, 1.0f}, 0.4f, 0x03);

    auto s = b.sample(1.05);
    CHECK(s.position.x == Approx(0.5f));
    CHECK(s.yaw == Approx(0.1f));
    CHECK(s.velocity.x == Approx(10.0f));
    CHECK_FALSE(s.extrapolating);
    s = b.sample(1.15);
    CHECK(s.position.x == Approx(1.5f));
    CHECK(s.position.z == Approx(0.5f));
    CHECK(s.flags == 0x03);

    // Out of order / duplicate stamps are dropped.
    b.push(1.15, {50.0f, 0.0f, 0.0f}, 0.0f, 0x01);
    b.push(1.2, {50.0f, 0.0f, 0.0f}, 0.0f, 0x01);
    CHECK(b.size() == 3);
    // Before the oldest: hold the oldest.
    CHECK(b.sample(0.5).position.x == Approx(0.0f));
}

TEST_CASE("SnapshotBuffer: brief extrapolation while moving, then hold; stopped entities hold",
          "[client][world][remote]")
{
    SnapshotBuffer b;
    b.push(0.0, {0.0f, 0.0f, 0.0f}, 0.0f, 0x01);
    b.push(0.1, {0.5f, 0.0f, 0.0f}, 0.0f, 0x01); // 5 m/s along +x
    auto s = b.sample(0.2);
    CHECK(s.extrapolating);
    CHECK(s.position.x == Approx(1.0f));
    s = b.sample(0.1 + SnapshotBuffer::kMaxExtrapolation);
    CHECK(s.position.x == Approx(0.5f + 5.0f * static_cast<float>(SnapshotBuffer::kMaxExtrapolation)));
    // Far past the cap: held where extrapolation ended, velocity zero.
    s = b.sample(5.0);
    CHECK(s.holding);
    CHECK_FALSE(s.extrapolating);
    CHECK(s.position.x == Approx(0.5f + 5.0f * static_cast<float>(SnapshotBuffer::kMaxExtrapolation)));
    CHECK(s.velocity.x == 0.0f);

    // A stop packet (moving = 0): no extrapolation at all.
    SnapshotBuffer stop;
    stop.push(0.0, {0.0f, 0.0f, 0.0f}, 0.0f, 0x01);
    stop.push(0.1, {0.5f, 0.0f, 0.0f}, 0.0f, 0x00);
    s = stop.sample(0.3);
    CHECK(s.holding);
    CHECK(s.position.x == Approx(0.5f));

    // A walk starting long after the stop does not crawl from the old snapshot: the entity stands
    // until one interval before the new snapshot.
    stop.push(10.0, {1.5f, 0.0f, 0.0f}, 0.0f, 0x01);
    CHECK(stop.sample(9.0).position.x == Approx(0.5f));
    CHECK(stop.sample(9.95).position.x == Approx(1.0f));

    // Teleport-sized jumps snap instead of sliding across the map.
    SnapshotBuffer tp;
    tp.push(0.0, {0.0f, 0.0f, 0.0f}, 0.0f, 0x01);
    tp.push(0.1, {100.0f, 0.0f, 0.0f}, 0.0f, 0x01);
    CHECK(tp.size() == 1);
    CHECK(tp.sample(0.05).position.x == Approx(100.0f));
}

TEST_CASE("WorldModel: spawn / move / despawn are rendered 100 ms in the past", "[client][world][remote]")
{
    const auto terrain = vo::client::test::makeFlat(5.0f);
    world::WorldModel model;
    vn::EnterWorldRes res;
    res.entityId = 1;
    res.x = 10.0f;
    res.z = 10.0f;
    res.serverTimeMs = 5000;
    model.enter(res, &terrain, 100.0, true);

    vn::EntitySpawn sp;
    sp.entityId = 42;
    sp.name = "Ayla";
    sp.race = 2;
    sp.face = 3;
    sp.hair = 1;
    sp.x = 20.0f;
    sp.y = 0.0f;
    sp.z = 20.0f;
    model.handleSpawn(sp, 100.0);
    REQUIRE(model.activeCount() == 1);
    world::RemoteEntity* e = model.find(42);
    REQUIRE(e != nullptr);
    CHECK(e->name == "Ayla");
    CHECK(e->race == 2);
    CHECK(e->face == 3);
    CHECK(e->hair == 1);
    // Own entity id is not a remote.
    sp.entityId = 1;
    model.handleSpawn(sp, 100.0);
    CHECK(model.activeCount() == 1);

    // Moves at 10 Hz along +x at 5 m/s, delivered on time (server clock == local clock + offset).
    for (int i = 1; i <= 10; ++i)
    {
        vn::EntityMove mv;
        mv.entityId = 42;
        mv.flags = vn::kMoveFlagMoving | vn::kMoveFlagRunning;
        mv.x = 20.0f + 0.5f * static_cast<float>(i);
        mv.z = 20.0f;
        mv.serverTimeMs = 5000u + 100u * static_cast<std::uint32_t>(i);
        model.handleMove(mv, 100.0 + 0.1 * i);
    }
    // At local 101.05 the render time is server 5.95 s -> between the 9th (24.5) and 10th (25.0).
    model.updateRemotes(101.05, 0.016f);
    e = model.find(42);
    REQUIRE(e != nullptr);
    CHECK(e->position.x == Approx(24.75f).margin(0.01f));
    CHECK(e->position.y == Approx(5.0f).margin(0.01f)); // on the terrain
    CHECK(e->speed == Approx(5.0f).margin(0.01f));
    CHECK(e->moving);
    CHECK(e->running);

    // Unknown ids are counted and ignored.
    vn::EntityMove ghost;
    ghost.entityId = 999;
    ghost.serverTimeMs = 6100;
    model.handleMove(ghost, 101.1);
    CHECK(model.stats().unknownEntity == 1);

    vn::EntityDespawn ds;
    ds.entityId = 42;
    model.handleDespawn(ds);
    CHECK(model.activeCount() == 0);
    CHECK(model.find(42) == nullptr);
}

TEST_CASE("DebugGhosts: synthetic packets go through the normal message path", "[client][world][remote]")
{
    const auto terrain = vo::client::test::makeFlat(5.0f);
    world::WorldModel model;
    vn::EnterWorldRes res;
    res.entityId = 1;
    res.x = 32.0f;
    res.z = 32.0f;
    res.serverTimeMs = 1000;
    model.enter(res, &terrain, 0.0, true);

    std::map<std::uint16_t, int> counts;
    bool allDecoded = true;
    double now = 0.0;
    world::DebugGhosts::Sink sink = [&](std::uint16_t op, std::vector<std::uint8_t> payload)
    {
        ++counts[op];
        allDecoded = model.handleMessage(op, payload, now) && allDecoded;
    };
    world::DebugGhosts ghosts;
    ghosts.start(8, model.local().mover().position(), &terrain, model.clock(), now, sink);
    CHECK(counts[static_cast<std::uint16_t>(vn::Opcode::EntitySpawn)] == 8);
    CHECK(model.activeCount() == 8);

    int maxMovesPerGhostSecond = 0;
    int movesThisSecond = 0;
    for (int frame = 0; frame < 60 * 20; ++frame)
    {
        now += 1.0 / 60.0;
        const int before = counts[static_cast<std::uint16_t>(vn::Opcode::EntityMove)];
        ghosts.update(now, 1.0f / 60.0f, model.clock(), model.local().mover().position(), model.params(), sink);
        movesThisSecond += counts[static_cast<std::uint16_t>(vn::Opcode::EntityMove)] - before;
        if (frame % 60 == 59)
        {
            maxMovesPerGhostSecond = std::max(maxMovesPerGhostSecond, movesThisSecond);
            movesThisSecond = 0;
        }
        model.updateRemotes(now, 1.0f / 60.0f);
    }
    CHECK(allDecoded);
    CHECK(counts[static_cast<std::uint16_t>(vn::Opcode::EntityMove)] > 100);
    CHECK(counts[static_cast<std::uint16_t>(vn::Opcode::EntityDespawn)] >= 1); // the leaver left
    CHECK(counts[static_cast<std::uint16_t>(vn::Opcode::EntitySpawn)] >= 9);  // ... and came back
    CHECK(maxMovesPerGhostSecond <= 8 * 12);                                   // ~10 Hz per ghost
    CHECK(model.stats().unknownEntity == 0);
    // Ghosts stay on walkable ground near the player.
    for (const world::RemoteEntity& e : model.entities())
    {
        if (!e.active)
            continue;
        CHECK(terrain.isWalkable(e.position.x, e.position.z));
        CHECK(world::horizontalDistance(e.position, model.local().mover().position()) < 30.0f);
    }

    // A correction produced by the facility is applied through the same path.
    world::DebugGhosts::correction(0, {30.0f, 0.0f, 30.0f}, 0.0f, sink);
    CHECK(model.stats().corrections == 1);
    CHECK(model.local().mover().position().x == Approx(30.0f));
}
