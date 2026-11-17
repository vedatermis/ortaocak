// PostgreSQL and zones (fresh schema in vedatonline_test; SKIP without it, docs/PROTOCOL_ZONES.md §3.5,
// §6): new characters get their home zone, the vo_admin move-character helper (setPosition), and a
// real Server + World on PostgreSQL with three zones where the arrival of a zone change is in the
// database before the client hears of it (ZONE_CHANGE), and survives a server restart.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "shared/net/Crypto.h"
#include "tests/db/TestDb.h"
#include "tests/server/TestServer.h"
#include "tests/server/world/ZoneTestWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <string>
#include <thread>

namespace va = vo::server::auth;
namespace vdb = vo::server::db;
namespace vn = vo::net;
using AR = va::PostgresAccountService::AdminResult;
using namespace std::chrono_literals;

namespace {

template <class Out, class F>
Out await(F&& call)
{
    std::promise<Out> p;
    auto f = p.get_future();
    call([&p](Out r) { p.set_value(std::move(r)); });
    REQUIRE(f.wait_for(60s) == std::future_status::ready);
    return f.get();
}

std::string text(const vo::tests::TestSchema& schema, const std::string& sql)
{
    std::string error;
    auto conn = vdb::openConnection(schema.settings(), schema.url(), error);
    REQUIRE(conn);
    pqxx::nontransaction tx(*conn);
    const auto r = tx.exec(sql).one_row();
    return r[0].is_null() ? std::string("NULL") : r[0].as<std::string>();
}

// The home zones of PROTOCOL_ZONES.md §4 on the synthetic three-zone world (ZoneTestWorld.h).
va::NewCharacterSpawns homeSpawns()
{
    va::NewCharacterSpawns s;
    s.aydor = {2, 100.0f, 0.0f, 100.0f, 0.0f};
    s.kargan = {3, 400.0f, 0.0f, 400.0f, 1.0f};
    return s;
}

std::optional<vn::ZoneChangeBegin> walk(vo::tests::TestClient& c, std::uint32_t& seq, float& x, float& z, float tx,
                                        float tz)
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
        m.z = z;
        c.send(m);
        if (auto b = c.waitFor<vn::ZoneChangeBegin>([](const vn::ZoneChangeBegin&) { return true; }, 400ms))
            return b;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("Postgres: new characters in their home zone; setPosition (vo_admin move-character)", "[db][zones]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    std::string error;
    REQUIRE(pool.warmUp(error));
    va::PostgresAccountService svc(pool, {true, 2, homeSpawns()});
    va::AccountId account = 0;
    REQUIRE(svc.createAccount("pg_zones", "password1", account, error) == AR::Ok);
    const auto aydor = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(account, {"Tanyerli", 1, 1, 0, 0}, std::move(done)); });
    REQUIRE(aydor.result == vn::CharCreateResult::Ok);
    CHECK(aydor.character.position.zoneId == 2);
    const auto kargan = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(account, {"Demirli", 2, 1, 0, 0}, std::move(done)); });
    REQUIRE(kargan.result == vn::CharCreateResult::Ok);
    CHECK(kargan.character.position.zoneId == 3);
    CHECK(text(*schema, "SELECT zone_id FROM characters WHERE id = " + std::to_string(aydor.character.id)) == "2");
    CHECK(text(*schema, "SELECT zone_id FROM characters WHERE id = " + std::to_string(kargan.character.id)) == "3");

    va::CharacterInfo info;
    CHECK(svc.setPosition("Demirli", {1, 382.0f, 5.0f, 400.0f, 2.0f}, info, error) == AR::Ok);
    CHECK(info.position.zoneId == 1);
    CHECK(text(*schema, "SELECT zone_id || ' ' || x || ' ' || y || ' ' || z FROM characters WHERE id = " +
                            std::to_string(kargan.character.id)) == "1 382 5 400");
    CHECK(svc.setPosition("Yokboyle", {1, 1.0f, 0.0f, 1.0f, 0.0f}, info, error) == AR::NotFound);
    CHECK(svc.setPosition("Demirli", {1, std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 0.0f}, info, error) ==
          AR::Invalid);
    CHECK(svc.setPosition("Demirli", {0, 1.0f, 0.0f, 1.0f, 0.0f}, info, error) == AR::Invalid);
    CHECK(svc.findCharacterByName("demirli", info, error) == AR::Ok);
    CHECK(info.position.zoneId == 1);
    svc.stop();
}

TEST_CASE("World + PostgreSQL: a zone change is stored before ZONE_CHANGE and survives a restart",
          "[db][server][zones]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    std::uint32_t charId = 0;
    vn::ZoneChange change;
    {
        vdb::ConnectionPool pool(schema->settings(2));
        std::string error;
        REQUIRE(pool.warmUp(error));
        va::PostgresAccountService accounts(pool, {true, 2, vo::tests::testTerrainSpawns()});
        vo::tests::TestServer::Options opts;
        opts.accounts = &accounts;
        opts.world.statsInterval = 0s;
        opts.zones = [](vo::server::world::World& w) { vo::tests::addThreeZones(w, true); };
        vo::tests::TestServer server({}, opts);
        vo::tests::TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        const auto e = c.enterNew("pg_gate", "Geçitçi");
        charId = c.lastCharId();
        const std::string zoneSql = "SELECT zone_id FROM characters WHERE id = " + std::to_string(charId);
        CHECK(text(*schema, zoneSql) == "1");
        float x = e.x, z = e.z;
        std::uint32_t seq = 0;
        std::this_thread::sleep_for(2100ms);
        REQUIRE(walk(c, seq, x, z, 111.0f, 100.0f));
        const auto zc = c.waitFor<vn::ZoneChange>([](const vn::ZoneChange&) { return true; }, 5s);
        REQUIRE(zc);
        change = *zc;
        CHECK(change.enter.zoneId == 2);
        // §3.5: when the client hears of the new zone, the database already has it (a crash from
        // here on finds the character at the arrival, never in zone 1 with the client in zone 2).
        CHECK(text(*schema, zoneSql) == "2");
        const float storedX = std::stof(text(*schema, "SELECT x FROM characters WHERE id = " + std::to_string(charId)));
        CHECK(std::abs(storedX - change.enter.x) < 0.01f);
        c.send(vn::ZoneLoadedReq{2});
        c.pumpFor(200ms);
        server.stop(5s); // stays in the world until the server stops
    }
    {
        vdb::ConnectionPool pool(schema->settings(2));
        std::string error;
        REQUIRE(pool.warmUp(error));
        va::PostgresAccountService accounts(pool, {true, 2, vo::tests::testTerrainSpawns()});
        vo::tests::TestServer::Options opts;
        opts.accounts = &accounts;
        opts.world.statsInterval = 0s;
        opts.zones = [](vo::server::world::World& w) { vo::tests::addThreeZones(w, true); };
        vo::tests::TestServer server({}, opts);
        vo::tests::TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("pg_gate") == vn::LoginResult::Ok);
        const auto back = c.enter(charId);
        CHECK(back.zoneId == 2);
        CHECK(std::hypot(back.x - change.enter.x, back.z - change.enter.z) < 0.01f);
        c.send(vn::Logout{});
        REQUIRE(c.waitClosed());
        server.stop(5s);
    }
}
