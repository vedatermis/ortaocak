#include "tests/db/TestDb.h"

#include "server/db/Db.h"
#include "server/db/Migrations.h"
#include "shared/core/Config.h"
#include "shared/core/Paths.h"

#include <pqxx/pqxx>

#include <charconv>
#include <chrono>
#include <format>
#include <random>
#include <vector>

namespace vo::tests {

namespace vdb = vo::server::db;

namespace {

constexpr std::int64_t kSweepAfterSeconds = 2 * 60 * 60;

std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Drops vo_test_<ts>_* / vo_it_<ts>_* schemas left behind by crashed runs.
void sweepStaleSchemas(pqxx::connection& conn, pqxx::transaction_base& tx)
{
    std::vector<std::string> stale;
    for (auto [name] : tx.query<std::string>("SELECT nspname FROM pg_namespace WHERE nspname ~ '^vo_(test|it)_'"))
    {
        const auto first = name.find('_', 3) + 1; // after "vo_test_" / "vo_it_"
        const auto end = name.find('_', first);
        std::int64_t ts = 0;
        if (first == 0 || end == std::string::npos)
            continue;
        const auto [p, ec] = std::from_chars(name.data() + first, name.data() + end, ts);
        if (ec == std::errc{} && unixNow() - ts > kSweepAfterSeconds)
            stale.push_back(name);
    }
    for (const auto& s : stale)
        tx.exec(std::format("DROP SCHEMA IF EXISTS {} CASCADE", conn.quote_name(s)));
}

} // namespace

std::optional<TestDbUrls> testDbUrls(std::string& why)
{
    TestDbUrls urls;
    if (auto env = core::Config::processEnv("VEDAT_TEST_DB_URL"); env && !env->empty())
    {
        if (*env == "skip")
        {
            why = "PostgreSQL tests skipped: VEDAT_TEST_DB_URL=skip";
            return std::nullopt;
        }
        urls.url = *env;
    }
    if (auto local = core::locateDataFile("config/server.local.toml"))
    {
        core::Config cfg;
        std::string error;
        if (cfg.loadFile(*local, error))
        {
            if (urls.url.empty())
                urls.url = cfg.getString("db.test_connection_string", "");
            urls.adminUrl = cfg.getString("db.admin_connection_string", "");
        }
    }
    if (urls.url.empty())
    {
        why = "PostgreSQL tests skipped: no test database (set VEDAT_TEST_DB_URL or db.test_connection_string in "
              "config/server.local.toml)";
        return std::nullopt;
    }
    return urls;
}

std::unique_ptr<TestSchema> TestSchema::create(std::string& why, bool& configured, bool migrate)
{
    configured = false;
    const auto urls = testDbUrls(why);
    if (!urls)
        return nullptr;
    configured = true;

    const auto dbname = vdb::connectionDbName(urls->url);
    if (!dbname || *dbname != kTestDatabaseName)
    {
        why = std::format("refusing to run DB tests: the test connection string must name database '{}'",
                          kTestDatabaseName);
        return nullptr;
    }

    vdb::DbSettings base;
    base.connectionString = urls->url;
    base.adminConnectionString = urls->adminUrl;
    base.applicationName = "vo_tests";
    bool created = false;
    if (!vdb::ensureDatabaseExists(base, created, why))
        return nullptr;

    auto conn = vdb::openConnection(base, base.connectionString, why);
    if (!conn)
    {
        why = "test database unreachable: " + why;
        return nullptr;
    }
    std::string name;
    try
    {
        pqxx::nontransaction tx(*conn);
        if (tx.query_value<std::string>("SELECT current_database()") != kTestDatabaseName)
        {
            why = "refusing to run DB tests: connected to a database other than vedatonline_test";
            return nullptr;
        }
        sweepStaleSchemas(*conn, tx);
        std::random_device rd;
        name = std::format("vo_test_{}_{:08x}", unixNow(), rd());
        tx.exec(std::format("CREATE SCHEMA {}", conn->quote_name(name)));
    }
    catch (const std::exception& e)
    {
        why = vdb::describeDbError(e);
        return nullptr;
    }

    auto schema = std::make_unique<TestSchema>(urls->url, name);
    if (migrate)
    {
        const auto dir = vdb::locateMigrationsDir();
        std::vector<vdb::Migration> migrations;
        if (!dir || !vdb::loadMigrations(*dir, migrations, why))
        {
            why = "cannot load db/migrations: " + why;
            return nullptr;
        }
        auto mconn = vdb::openConnection(schema->settings(), schema->url(), why, 0);
        vdb::MigrationReport report;
        if (!mconn || !vdb::runMigrations(*mconn, name, migrations, report, why))
        {
            why = "migrating the test schema failed: " + why;
            return nullptr;
        }
    }
    return schema;
}

TestSchema::~TestSchema()
{
    vdb::DbSettings s;
    s.connectionString = m_url;
    s.applicationName = "vo_tests";
    std::string error;
    if (auto conn = vdb::openConnection(s, m_url, error))
    {
        try
        {
            pqxx::nontransaction tx(*conn);
            tx.exec(std::format("DROP SCHEMA IF EXISTS {} CASCADE", conn->quote_name(m_name)));
        }
        catch (const std::exception&) // swept later if this fails
        {
        }
    }
}

server::db::DbSettings TestSchema::settings(std::size_t poolSize) const
{
    vdb::DbSettings s;
    s.connectionString = m_url;
    s.schema = m_name;
    s.poolSize = poolSize;
    s.workerThreads = poolSize;
    s.applicationName = "vo_tests";
    return s;
}

} // namespace vo::tests
