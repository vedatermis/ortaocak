// ConnectionPool / WorkerPool tests. The pool tests use vedatonline_test (SKIP without it); the
// unreachable-database and worker tests need no database.

#include "server/db/ConnectionPool.h"
#include "server/db/WorkerPool.h"
#include "tests/db/TestDb.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pqxx/pqxx>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

namespace vdb = vo::server::db;
using Catch::Matchers::ContainsSubstring;
using namespace std::chrono_literals;

namespace {

int backendPid(vdb::ConnectionPool& pool)
{
    int pid = 0;
    std::string error;
    REQUIRE(pool.run([&](pqxx::connection& c) {
        pqxx::nontransaction tx(c);
        pid = tx.query_value<int>("SELECT pg_backend_pid()");
    }, error));
    return pid;
}

} // namespace

TEST_CASE("ConnectionPool runs jobs, reuses connections and configures sessions", "[db][pool]")
{
    VO_TEST_SCHEMA(schema);
    auto settings = schema->settings(2);
    settings.statementTimeout = 1500ms;
    vdb::ConnectionPool pool(settings);
    std::string error;
    REQUIRE(pool.warmUp(error));

    std::string tz;
    std::string timeout;
    std::string searchPath;
    REQUIRE(pool.run([&](pqxx::connection& c) {
        pqxx::nontransaction tx(c);
        tz = tx.query_value<std::string>("SHOW TimeZone");
        timeout = tx.query_value<std::string>("SHOW statement_timeout");
        searchPath = tx.query_value<std::string>("SELECT current_schema()");
    }, error));
    CHECK(tz == "UTC");
    CHECK(timeout == "1500ms");
    CHECK(searchPath == schema->name());

    // Statement timeout is enforced; the connection survives it.
    CHECK_FALSE(pool.run([](pqxx::connection& c) {
        pqxx::nontransaction tx(c);
        tx.exec("SELECT pg_sleep(5)");
    }, error));
    CHECK_THAT(error, ContainsSubstring("57014")); // query_canceled
    CHECK(backendPid(pool) != 0);

    // Parallel jobs never exceed the pool size.
    // (Catch2 assertions are not thread-safe: threads only count, the test thread checks.)
    std::atomic<int> inFlight{0};
    std::atomic<int> peak{0};
    std::atomic<int> succeeded{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i)
    {
        threads.emplace_back([&] {
            std::string e;
            if (pool.run([&](pqxx::connection& c) {
                const int now = ++inFlight;
                int expected = peak.load();
                while (now > expected && !peak.compare_exchange_weak(expected, now))
                {
                }
                pqxx::nontransaction tx(c);
                tx.exec("SELECT pg_sleep(0.1)");
                --inFlight;
            }, e))
                ++succeeded;
        });
    }
    for (auto& t : threads)
        t.join();
    CHECK(succeeded == 6);
    CHECK(peak <= 2);
    CHECK(pool.stats().open <= 2);

    // A job's own exception is reported, not swallowed, and does not cost the connection.
    const auto before = pool.stats().connectsOk;
    CHECK_FALSE(pool.run([](pqxx::connection& c) {
        pqxx::work tx(c);
        tx.exec("SELECT * FROM no_such_table");
    }, error));
    CHECK_THAT(error, ContainsSubstring("42P01"));
    CHECK(backendPid(pool) != 0);
    CHECK(pool.stats().connectsOk == before);

    pool.close();
    CHECK_FALSE(pool.run([](pqxx::connection&) {}, error));
    CHECK_THAT(error, ContainsSubstring("closed"));
}

TEST_CASE("ConnectionPool recovers after its connections are terminated", "[db][pool]")
{
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    std::string error;
    // Open both connections.
    std::atomic<int> succeeded{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i)
        threads.emplace_back([&] {
            std::string e;
            if (pool.run([](pqxx::connection& c) {
                    pqxx::nontransaction tx(c);
                    tx.exec("SELECT pg_sleep(0.2)");
                }, e))
                ++succeeded;
        });
    for (auto& t : threads)
        t.join();
    REQUIRE(succeeded == 2);
    const auto pids = pool.idleBackendPids();
    REQUIRE(pids.size() == 2);

    // Terminate exactly the pool's own backends, from a separate connection to vedatonline_test.
    {
        auto admin = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(admin);
        pqxx::nontransaction tx(*admin);
        REQUIRE(tx.query_value<std::string>("SELECT current_database()") == vo::tests::kTestDatabaseName);
        for (const int pid : pids)
        {
            CHECK(tx.query_value<bool>("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                                       "WHERE pid = $1 AND datname = current_database() AND application_name = $2",
                                       pqxx::params{pid, std::string("vo_tests")}));
        }
    }
    std::this_thread::sleep_for(200ms);

    // The next jobs transparently reconnect (retry once on a fresh connection).
    std::set<int> newPids;
    for (int i = 0; i < 3; ++i)
        newPids.insert(backendPid(pool));
    for (const int pid : pids)
        CHECK_FALSE(newPids.contains(pid));
    CHECK(pool.stats().retries >= 1);
    CHECK(pool.stats().connectsOk >= 3);

    // Writes work again too.
    REQUIRE(pool.run([](pqxx::connection& c) {
        pqxx::work tx(c);
        tx.exec("INSERT INTO accounts (username, username_key, password_hash) VALUES "
                "('pool_user', 'pool_user', '$argon2id$v=19$m=1,t=1,p=1$x$y')");
        tx.commit();
    }, error));
}

TEST_CASE("ConnectionPool fails fast and backs off when the database is unreachable", "[pool]")
{
    vdb::DbSettings s;
    // Port 1 on loopback: refused immediately, no network needed.
    s.connectionString = "host=127.0.0.1 port=1 dbname=vedatonline_test user=nobody connect_timeout=2";
    s.poolSize = 1;
    s.acquireTimeout = 500ms;
    vdb::ConnectionPool pool(s);
    std::string error;
    CHECK_FALSE(pool.warmUp(error));
    CHECK_THAT(error, ContainsSubstring("cannot connect"));
    CHECK(error.find("password") == std::string::npos);
    // Within the backoff window the next attempt does not even try.
    CHECK_FALSE(pool.run([](pqxx::connection&) {}, error));
    CHECK_THAT(error, ContainsSubstring("next reconnect attempt"));
    CHECK(pool.stats().connectsFailed == 1);
    CHECK(pool.stats().open == 0);
    std::this_thread::sleep_for(250ms);
    CHECK_FALSE(pool.run([](pqxx::connection&) {}, error));
    CHECK(pool.stats().connectsFailed == 2);
}

TEST_CASE("Database helpers never expose the password", "[pool]")
{
    CHECK(vdb::describeConnectionString("postgresql://alice:s3cr3t@db.example:6543/game?sslmode=require") ==
          "alice@db.example:6543/game sslmode=require");
    CHECK(vdb::describeConnectionString("host=h port=5 dbname=d user=u password=s3cr3t") == "u@h:5/d");
    CHECK(vdb::describeConnectionString("this is = not valid '") == "<unparseable connection string>");
    CHECK(vdb::connectionDbName("postgresql://u:p@h/vedatonline_test") == "vedatonline_test");
    CHECK(vdb::isSafeIdentifier("vedatonline"));
    CHECK_FALSE(vdb::isSafeIdentifier("Vedat"));
    CHECK_FALSE(vdb::isSafeIdentifier("x; DROP"));
    CHECK_FALSE(vdb::isSafeIdentifier("1abc"));
}

TEST_CASE("WorkerPool runs jobs off the calling thread and drains on stop", "[pool]")
{
    vdb::WorkerPool workers(3, "test-workers");
    std::atomic<int> done{0};
    std::atomic<bool> offThread{true};
    const auto self = std::this_thread::get_id();
    for (int i = 0; i < 50; ++i)
    {
        REQUIRE(workers.submit([&] {
            if (std::this_thread::get_id() == self)
                offThread = false;
            std::this_thread::sleep_for(1ms);
            ++done;
        }));
    }
    workers.stop(); // must run all 50 queued jobs first
    CHECK(done == 50);
    CHECK(offThread);
    CHECK_FALSE(workers.submit([&] { ++done; }));
    workers.stop();
    CHECK(done == 50);
}

TEST_CASE("DB access from a thread marked markThreadNoDb() is refused", "[pool]")
{
#if !defined(VO_DEBUG)
    bool connected = true;
    bool ran = true;
    std::string connectError;
    std::string runError;
    std::thread t([&] {
        vdb::markThreadNoDb();
        vdb::DbSettings s;
        s.connectionString = "host=127.0.0.1 port=1 dbname=x";
        connected = vdb::openConnection(s, s.connectionString, connectError) != nullptr;
        vdb::ConnectionPool pool(s);
        ran = pool.run([](pqxx::connection&) {}, runError);
    });
    t.join();
    CHECK_FALSE(connected);
    CHECK_THAT(connectError, ContainsSubstring("network thread"));
    CHECK_FALSE(ran);
    CHECK_THAT(runError, ContainsSubstring("network thread"));
#else
    SUCCEED("Debug builds assert instead (VO_ASSERT); checked in Release");
#endif
    CHECK_FALSE(vdb::isThreadNoDb());
}
