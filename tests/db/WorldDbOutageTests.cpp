// DB outage while players are in the world (M7): a real Server + World backed by the PostgreSQL
// account service on its own vo_test_* schema in vedatonline_test. The pool's connections are
// terminated (pg_terminate_backend, only our own backends) several times while a player is in the
// world and periodic saves are running: the session survives, the world thread never waits for the
// database, saves are retried on fresh connections and the final position is stored.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "tests/db/TestDb.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <cmath>
#include <future>
#include <thread>

namespace vdb = vo::server::db;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vs = vo::server::net;
using namespace std::chrono_literals;

namespace {

// Terminates the given backends of vedatonline_test (our own: application_name vo_tests). Returns
// how many were terminated.
int terminateBackends(const vo::tests::TestSchema& schema, const std::vector<int>& pids)
{
    std::string error;
    auto admin = vdb::openConnection(schema.settings(), schema.url(), error);
    REQUIRE(admin);
    pqxx::nontransaction tx(*admin);
    REQUIRE(tx.query_value<std::string>("SELECT current_database()") == vo::tests::kTestDatabaseName);
    int n = 0;
    for (const int pid : pids)
    {
        if (tx.query_value<bool>("SELECT coalesce(bool_or(pg_terminate_backend(pid)), false) FROM pg_stat_activity "
                                 "WHERE pid = $1 AND datname = current_database() AND application_name = $2",
                                 pqxx::params{pid, std::string("vo_tests")}))
            ++n;
    }
    return n;
}

} // namespace

TEST_CASE("World + PostgreSQL: terminated DB connections while in the world do not hurt the session",
          "[db][server][world][outage]")
{
    VO_TEST_SCHEMA(schema);
    auto dbSettings = schema->settings(2);
    dbSettings.healthCheckIdle = 1h; // use the dead connections as they are: exercises the job retry
    vdb::ConnectionPool pool(dbSettings);
    std::string error;
    REQUIRE(pool.warmUp(error));
    va::PostgresAccountService accounts(pool, {true, 2, vo::tests::testTerrainSpawns()});

    vo::tests::TestServer::Options opts;
    opts.accounts = &accounts;
    opts.world.saveInterval = std::chrono::seconds(1); // periodic saves during the outage
    opts.world.statsInterval = std::chrono::seconds(0);
    vs::ServerSettings st;
    st.idleTimeout = 10s;
    auto server = std::make_unique<vo::tests::TestServer>(st, opts);

    vo::tests::TestClient c;
    REQUIRE(c.connect(server->clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("db_outage_user", "Kesinti");
    const std::uint32_t charId = c.lastCharId();
    const auto statsBefore = pool.stats();

    vn::MoveReq m;
    m.flags = vn::kMoveFlagMoving | vn::kMoveFlagRunning;
    m.z = e.z;
    std::uint32_t seq = 0;
    float x = e.x;
    std::size_t corrections = 0;
    // Three outages, each followed by a second of walking and pinging (periodic saves run meanwhile).
    for (int outage = 0; outage < 3; ++outage)
    {
        const auto pids = pool.idleBackendPids();
        INFO("outage " << outage << ": " << pids.size() << " idle backend(s)");
        CHECK(terminateBackends(*schema, pids) == static_cast<int>(pids.size()));
        for (int step = 0; step < 10; ++step)
        {
            c.pumpFor(100ms);
            x += 0.4f; // 4 m/s while running (5.5 m/s): well inside the allowance
            m.seq = ++seq;
            m.x = x;
            c.send(m);
            if (step % 3 == 0)
            {
                c.send(vn::Ping{seq});
                bool pong = false; // the session answers during the outage
                while (!pong)
                {
                    auto msg = c.next(3s);
                    REQUIRE(msg.has_value());
                    corrections += msg->opcode == static_cast<std::uint16_t>(vn::Opcode::PositionCorrect) ? 1 : 0;
                    pong = msg->opcode == static_cast<std::uint16_t>(vn::Opcode::Pong);
                }
            }
        }
        while (auto msg = c.next(0ms))
            corrections += msg->opcode == static_cast<std::uint16_t>(vn::Opcode::PositionCorrect) ? 1 : 0;
    }
    CHECK(corrections == 0);
    CHECK_FALSE(c.closed());

    // Once more right before leaving: the leave save runs on a dead connection and is retried.
    CHECK(terminateBackends(*schema, pool.idleBackendPids()) >= 0);
    c.send(vn::LeaveWorldReq{});
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    c.send(vn::CharListReq{});
    CHECK(c.expectSkippingWorld<vn::CharListRes>().characters.size() == 1);

    const auto statsAfter = pool.stats();
    CHECK(statsAfter.retries > statsBefore.retries);       // jobs hit a dead connection and retried
    CHECK(statsAfter.connectsOk > statsBefore.connectsOk); // ... on fresh connections

    // The stored position is the last one walked to.
    va::AccountId accountId = 0;
    REQUIRE(accounts.findAccount("db_outage_user", accountId, error) == va::PostgresAccountService::AdminResult::Ok);
    std::promise<va::LoadCharacterOutcome> loaded;
    accounts.loadCharacterForWorld(accountId, charId, [&](va::LoadCharacterOutcome out) { loaded.set_value(std::move(out)); });
    const auto out = loaded.get_future().get();
    REQUIRE(out.result == vn::EnterWorldResult::Ok);
    CHECK(std::abs(out.character.position.x - x) < 0.01f);

    server->stop(5s);
    // The world thread never waited for the database: no tick came anywhere near a DB round trip.
    const auto ticks = server->world().totalStats();
    CHECK(ticks.ticks > 50);
    CHECK(ticks.maxMs < 40.0);
    server.reset();
}
