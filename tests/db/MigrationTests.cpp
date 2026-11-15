// Migration runner tests against vedatonline_test (each on its own schema; SKIP without a test DB),
// plus file loading/checksum tests that need no database.

#include "server/db/Db.h"
#include "server/db/Migrations.h"
#include "shared/core/Paths.h"
#include "tests/db/TestDb.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pqxx/pqxx>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

namespace vdb = vo::server::db;
using Catch::Matchers::ContainsSubstring;

namespace {

std::filesystem::path tempDir(const char* tag)
{
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / std::format("vo_{}_{:08x}", tag, rd());
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& p, std::string_view text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::int64_t countRows(pqxx::connection& c, const std::string& sql)
{
    pqxx::nontransaction tx(c);
    return tx.query_value<std::int64_t>(sql);
}

std::vector<vdb::Migration> repoMigrations()
{
    std::vector<vdb::Migration> m;
    std::string error;
    const auto dir = vdb::locateMigrationsDir();
    REQUIRE(dir.has_value());
    REQUIRE(vdb::loadMigrations(*dir, m, error));
    return m;
}

} // namespace

TEST_CASE("Migration files: naming, ordering and line-ending independent checksums", "[migrations]")
{
    const auto repo = repoMigrations();
    REQUIRE_FALSE(repo.empty());
    CHECK(repo.front().version == 1);
    CHECK(repo.front().name == "init");
    CHECK(repo.front().checksum.size() == 64);
    for (std::size_t i = 1; i < repo.size(); ++i)
        CHECK(repo[i].version > repo[i - 1].version);

    const auto lf = vdb::makeMigration(7, "x", "CREATE TABLE a (x int);\nSELECT 1;\n");
    const auto crlf = vdb::makeMigration(7, "x", "CREATE TABLE a (x int);\r\nSELECT 1;\r\n");
    const auto bom = vdb::makeMigration(7, "x", "\xEF\xBB\xBF" "CREATE TABLE a (x int);\nSELECT 1;\n");
    CHECK(lf.checksum == crlf.checksum);
    CHECK(lf.checksum == bom.checksum);
    CHECK(crlf.sql == lf.sql);
    CHECK(vdb::makeMigration(7, "x", "CREATE TABLE b (x int);").checksum != lf.checksum);

    const auto dir = tempDir("migfiles");
    writeFile(dir / "0002_second.sql", "SELECT 2;");
    writeFile(dir / "0001_first.sql", "SELECT 1;");
    writeFile(dir / "README.md", "ignored");
    std::vector<vdb::Migration> loaded;
    std::string error;
    REQUIRE(vdb::loadMigrations(dir, loaded, error));
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].version == 1);
    CHECK(loaded[1].name == "second");

    writeFile(dir / "0002_duplicate.sql", "SELECT 2;");
    CHECK_FALSE(vdb::loadMigrations(dir, loaded, error));
    CHECK_THAT(error, ContainsSubstring("duplicate"));
    std::filesystem::remove(dir / "0002_duplicate.sql");
    writeFile(dir / "3_bad-name.sql", "SELECT 3;");
    CHECK_FALSE(vdb::loadMigrations(dir, loaded, error));
    CHECK_THAT(error, ContainsSubstring("NNNN"));
    std::filesystem::remove_all(dir);
    CHECK_FALSE(vdb::loadMigrations(dir, loaded, error));
}

TEST_CASE("Migrations apply to a fresh schema and are idempotent", "[db][migrations]")
{
    VO_TEST_SCHEMA(schema, false);
    const auto migrations = repoMigrations();
    std::string error;
    auto conn = vdb::openConnection(schema->settings(), schema->url(), error, 0);
    REQUIRE(conn);

    vdb::MigrationStatus st;
    REQUIRE(vdb::readMigrationStatus(*conn, migrations, st, error));
    CHECK(st.applied.empty());
    CHECK(st.currentVersion == 0);
    CHECK(st.pending.size() == migrations.size());

    vdb::MigrationReport report;
    REQUIRE(vdb::runMigrations(*conn, schema->name(), migrations, report, error));
    CHECK(report.applied.size() == migrations.size());
    CHECK(report.currentVersion == migrations.back().version);
    CHECK(countRows(*conn, "SELECT count(*) FROM schema_migrations") == static_cast<std::int64_t>(migrations.size()));
    CHECK(countRows(*conn, "SELECT count(*) FROM accounts") == 0);
    CHECK(countRows(*conn, "SELECT count(*) FROM characters") == 0);
    {
        // Tables were created in the test schema, not in public.
        pqxx::nontransaction tx(*conn);
        CHECK(tx.query_value<std::string>("SELECT table_schema FROM information_schema.tables WHERE table_name = "
                                          "'accounts' AND table_schema = current_schema()") == schema->name());
    }

    vdb::MigrationReport again;
    REQUIRE(vdb::runMigrations(*conn, schema->name(), migrations, again, error));
    CHECK(again.applied.empty());
    CHECK(again.currentVersion == migrations.back().version);
    CHECK(countRows(*conn, "SELECT count(*) FROM schema_migrations") == static_cast<std::int64_t>(migrations.size()));

    REQUIRE(vdb::readMigrationStatus(*conn, migrations, st, error));
    CHECK(st.pending.empty());
    CHECK(st.problems.empty());
    REQUIRE(st.applied.size() == migrations.size());
    CHECK(st.applied[0].checksum == migrations[0].checksum);
    CHECK(st.applied[0].appliedAt.ends_with("Z"));

    // migrateDatabase (the vo_server/vo_admin entry point) agrees.
    vdb::MigrationReport viaHelper;
    REQUIRE(vdb::migrateDatabase(schema->settings(), std::nullopt, viaHelper, error));
    CHECK(viaHelper.applied.empty());
}

TEST_CASE("Migration runner refuses changed, missing and out-of-order migrations", "[db][migrations]")
{
    VO_TEST_SCHEMA(schema, false);
    std::string error;
    auto conn = vdb::openConnection(schema->settings(), schema->url(), error, 0);
    REQUIRE(conn);
    std::vector<vdb::Migration> v1 = {vdb::makeMigration(1, "one", "CREATE TABLE m_one (x int);"),
                                      vdb::makeMigration(3, "three", "CREATE TABLE m_three (x int);")};
    vdb::MigrationReport report;
    REQUIRE(vdb::runMigrations(*conn, schema->name(), v1, report, error));
    CHECK(report.applied == std::vector<int>{1, 3});

    SECTION("checksum mismatch of an applied migration is a hard error")
    {
        auto changed = v1;
        changed[0] = vdb::makeMigration(1, "one", "CREATE TABLE m_one (x bigint);");
        changed.push_back(vdb::makeMigration(4, "four", "CREATE TABLE m_four (x int);"));
        CHECK_FALSE(vdb::runMigrations(*conn, schema->name(), changed, report, error));
        CHECK_THAT(error, ContainsSubstring("checksum mismatch"));
        CHECK_THAT(error, ContainsSubstring("0001_one"));
        CHECK(countRows(*conn, "SELECT count(*) FROM schema_migrations") == 2); // 0004 was not applied
        vdb::MigrationStatus st;
        REQUIRE(vdb::readMigrationStatus(*conn, changed, st, error));
        CHECK(st.problems.size() == 1);
    }
    SECTION("an applied migration without a file is a hard error")
    {
        const std::vector<vdb::Migration> missing = {v1[0]};
        CHECK_FALSE(vdb::runMigrations(*conn, schema->name(), missing, report, error));
        CHECK_THAT(error, ContainsSubstring("0003_three"));
    }
    SECTION("a new migration older than the applied version is a hard error")
    {
        auto withTwo = v1;
        withTwo.insert(withTwo.begin() + 1, vdb::makeMigration(2, "two", "CREATE TABLE m_two (x int);"));
        CHECK_FALSE(vdb::runMigrations(*conn, schema->name(), withTwo, report, error));
        CHECK_THAT(error, ContainsSubstring("out of order"));
        CHECK(countRows(*conn, "SELECT count(*) FROM pg_tables WHERE tablename = 'm_two'") == 0);
    }
    SECTION("a failing migration is rolled back completely and stops the run")
    {
        auto bad = v1;
        bad.push_back(vdb::makeMigration(4, "four", "CREATE TABLE m_four (x int); SELECT no_such_function();"));
        bad.push_back(vdb::makeMigration(5, "five", "CREATE TABLE m_five (x int);"));
        CHECK_FALSE(vdb::runMigrations(*conn, schema->name(), bad, report, error));
        CHECK_THAT(error, ContainsSubstring("0004_four"));
        CHECK(countRows(*conn, "SELECT count(*) FROM pg_tables WHERE schemaname = current_schema() AND tablename "
                               "IN ('m_four', 'm_five')") == 0);
        CHECK(countRows(*conn, "SELECT count(*) FROM schema_migrations") == 2);
        // The lock was released: a corrected run succeeds.
        bad[2] = vdb::makeMigration(4, "four", "CREATE TABLE m_four (x int);");
        REQUIRE(vdb::runMigrations(*conn, schema->name(), bad, report, error));
        CHECK(report.applied == std::vector<int>{4, 5});
    }
}

TEST_CASE("Concurrent migration runners are serialised by the advisory lock", "[db][migrations]")
{
    VO_TEST_SCHEMA(schema, false);
    // Slow migrations: without the lock both runners would try to create the same tables.
    const std::vector<vdb::Migration> migrations = {
        vdb::makeMigration(1, "slow_one", "CREATE TABLE c_one (x int); SELECT pg_sleep(0.4);"),
        vdb::makeMigration(2, "slow_two", "CREATE TABLE c_two (x int); SELECT pg_sleep(0.2);"),
    };
    constexpr int kRunners = 3;
    std::atomic<int> ok{0};
    std::atomic<int> appliedTotal{0};
    std::vector<std::string> errors(kRunners);
    std::vector<std::thread> threads;
    for (int i = 0; i < kRunners; ++i)
    {
        threads.emplace_back([&, i] {
            std::string error;
            auto conn = vdb::openConnection(schema->settings(), schema->url(), error, 0);
            vdb::MigrationReport report;
            if (conn && vdb::runMigrations(*conn, schema->name(), migrations, report, error))
            {
                ++ok;
                appliedTotal += static_cast<int>(report.applied.size());
            }
            else
            {
                errors[static_cast<std::size_t>(i)] = error;
            }
        });
    }
    for (auto& t : threads)
        t.join();
    for (const auto& e : errors)
        CHECK(e.empty());
    CHECK(ok == kRunners);
    CHECK(appliedTotal == 2); // each migration applied exactly once overall
    std::string error;
    auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
    REQUIRE(conn);
    CHECK(countRows(*conn, "SELECT count(*) FROM schema_migrations") == 2);
}
