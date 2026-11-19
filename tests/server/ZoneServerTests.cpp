// Zone changes through a real in-process server (docs/PROTOCOL_ZONES.md §3): three synthetic zones,
// a client walks into gate_aydor and gets ZONE_CHANGE_BEGIN, ZONE_CHANGE and the entry packets in
// order; moves and combat while loading are dropped without a kick; ZONE_LOADED_REQ restarts the
// MOVE_REQ seq; out-of-state / wrong-zone ZONE_LOADED_REQ; the arrival is stored (character list
// after leaving); a Kargan takes its own road (refusals: tests/server/world/ZoneChangeTests.cpp).

#include "tests/server/TestServer.h"
#include "tests/server/world/ZoneTestWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <thread>

namespace vn = vo::net;
using namespace std::chrono_literals;
using vo::tests::TestClient;
using vo::tests::TestServer;

namespace {

TestServer::Options threeZones()
{
    TestServer::Options o;
    o.zones = [](vo::server::world::World& w) { vo::tests::addThreeZones(w, true); };
    return o;
}

// Walks from (x, z) towards (tx, tz) in steps of 2 m every 400 ms (running) until the server sends
// ZONE_CHANGE_BEGIN or the target is reached; returns the BEGIN if one came.
std::optional<vn::ZoneChangeBegin> walk(TestClient& c, std::uint32_t& seq, float& x, float& z, float tx, float tz)
{
    for (int i = 0; i < 40; ++i)
    {
        const float dx = tx - x, dz = tz - z;
        const float d = std::hypot(dx, dz);
        if (d < 0.01f)
            return std::nullopt;
        const float step = std::min(d, 2.0f);
        x += dx / d * step;
        z += dz / d * step;
        vn::MoveReq m;
        m.seq = ++seq;
        m.flags = vn::kMoveFlagMoving | vn::kMoveFlagRunning;
        m.x = x;
        m.y = 0.0f;
        m.z = z;
        c.send(m);
        if (auto b = c.waitFor<vn::ZoneChangeBegin>([](const vn::ZoneChangeBegin&) { return true; }, 400ms))
            return b;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("Zone server: walking into a gate changes zone with the full entry sequence", "[server][zones]")
{
    TestServer server({}, threeZones());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto entered = c.enterNew("zonewalker", "Yolcu", 1);
    REQUIRE(entered.zoneId == 1);
    float x = entered.x, z = entered.z;
    std::uint32_t seq = 0;
    std::this_thread::sleep_for(2100ms); // the gates arm 2 s after appearing in the zone
    const auto begin = walk(c, seq, x, z, 111.0f, 100.0f);
    REQUIRE(begin);
    CHECK(begin->fromZone == 1);
    CHECK(begin->toZone == 2);

    // Sent before the client knew: dropped silently (no kick, no correction).
    vn::MoveReq late;
    late.seq = ++seq;
    late.flags = vn::kMoveFlagMoving;
    late.x = x + 1.0f;
    late.z = z;
    c.send(late);
    c.send(vn::AttackReq{12345});

    // ZONE_CHANGE, then exactly the packets of a fresh entry.
    const auto change = c.expect<vn::ZoneChange>();
    CHECK(change.result == vn::ZoneChangeResult::Ok);
    CHECK(change.fromZone == 1);
    CHECK(change.enter.zoneId == 2);
    CHECK(change.enter.entityId != entered.entityId);
    CHECK(std::hypot(change.enter.x - 118.0f, change.enter.z - 100.0f) <= 2.05f);
    CHECK(change.enter.level == entered.level);
    CHECK(change.enter.hp == entered.hp);
    CHECK_FALSE(change.enter.isDead);
    const auto inv = c.expect<vn::Inventory>();
    CHECK(inv.items.size() == c.lastInventory().items.size());
    (void)c.expect<vn::Coins>();
    (void)c.expect<vn::SkillState>();
    (void)c.expect<vn::SkillBar>();
    (void)c.expect<vn::QuestList>(); // v11
    (void)c.expect<vn::CharStats>();

    // Loaded: a new MOVE_REQ seq sequence starts (seq 1 again is fine).
    c.send(vn::ZoneLoadedReq{2});
    c.pumpFor(200ms);
    vn::MoveReq first;
    first.seq = 1;
    first.flags = vn::kMoveFlagMoving;
    first.x = change.enter.x + 0.5f;
    first.z = change.enter.z;
    c.send(first);
    first.seq = 2;
    first.flags = 0;
    c.send(first);
    c.send(vn::Ping{77});
    const auto pong = c.waitFor<vn::Pong>([](const vn::Pong& p) { return p.clientTimeMs == 77; }, 3s);
    REQUIRE(pong);
    c.pumpFor(300ms);
    CHECK_FALSE(c.closed());

    // A second ZONE_LOADED_REQ is out of state.
    c.send(vn::ZoneLoadedReq{2});
    const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; }, 3s);
    REQUIRE(err);
    CHECK(err->errorCode == vn::ErrorCode::BadState);
    CHECK(c.waitClosed());

    // The arrival was stored: the character is listed in zone 2.
    TestClient again;
    REQUIRE(again.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(again.login("zonewalker") == vn::LoginResult::Ok);
    again.send(vn::CharListReq{});
    const auto list = again.expect<vn::CharListRes>();
    REQUIRE(list.characters.size() == 1);
    CHECK(list.characters[0].zoneId == 2);
    const auto back = again.enter(list.characters[0].charId);
    CHECK(back.zoneId == 2);
    CHECK(std::hypot(back.x - change.enter.x, back.z - change.enter.z) < 1.0f);
}

TEST_CASE("Zone server: ZONE_LOADED_REQ without a change or for the wrong zone; the Kargan road",
          "[server][zones]")
{
    TestServer server({}, threeZones());
    SECTION("without a zone change: BadState")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        (void)c.enterNew("zoneearly", "Erkenci", 1);
        c.send(vn::ZoneLoadedReq{1});
        const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; }, 3s);
        REQUIRE(err);
        CHECK(err->errorCode == vn::ErrorCode::BadState);
    }
    SECTION("for another zone: protocol violation")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        const auto e = c.enterNew("zonewrong", "Yanlis", 1);
        float x = e.x, z = e.z;
        std::uint32_t seq = 0;
        std::this_thread::sleep_for(2100ms);
        REQUIRE(walk(c, seq, x, z, 111.0f, 100.0f));
        const auto change = c.waitFor<vn::ZoneChange>([](const vn::ZoneChange&) { return true; }, 3s);
        REQUIRE(change);
        c.send(vn::ZoneLoadedReq{3});
        const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; }, 3s);
        REQUIRE(err);
        CHECK(err->errorCode == vn::ErrorCode::ProtocolViolation);
    }
    SECTION("a Kargan takes gate_kargan to Demirağız")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        const auto e = c.enterNew("zonekargan", "Dagli", 2); // Kargan: spawn (400, 400) of zone 1
        CHECK(e.zoneId == 1);
        float x = e.x, z = e.z;
        std::uint32_t seq = 0;
        std::this_thread::sleep_for(2100ms);
        const auto begin = walk(c, seq, x, z, 389.0f, 400.0f);
        REQUIRE(begin);
        CHECK(begin->toZone == 3);
        const auto change = c.waitFor<vn::ZoneChange>([](const vn::ZoneChange&) { return true; }, 3s);
        REQUIRE(change);
        CHECK(change->enter.zoneId == 3);
    }
}
