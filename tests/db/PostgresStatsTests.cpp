// PostgreSQL stat persistence (fresh schema in vedatonline_test; SKIP without it): migration 0005 on
// a database with characters from before M11 (all their points unspent), allocateStats (budget, cap,
// another account, the CHECK on the stored level), the constraints, vo_admin reset-stats /
// set-level / show-character helpers, and a real Server + World on PostgreSQL where an allocation is
// stored before its answer and survives a server restart. docs/PROTOCOL_STATS.md §6, docs/DATABASE.md.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "server/db/Migrations.h"
#include "shared/game/Combat.h"
#include "shared/net/Crypto.h"
#include "tests/db/TestDb.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace va = vo::server::auth;
namespace vdb = vo::server::db;
namespace vg = vo::game;
namespace vn = vo::net;
using AR = va::PostgresAccountService::AdminResult;
using namespace std::chrono_literals;

namespace {

template <class R, class F>
R await(F&& call)
{
    std::promise<R> p;
    auto f = p.get_future();
    call([&p](R r) { p.set_value(std::move(r)); });
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

std::vector<vdb::Migration> repoMigrations()
{
    const auto dir = vdb::locateMigrationsDir();
    REQUIRE(dir);
    std::vector<vdb::Migration> m;
    std::string error;
    REQUIRE(vdb::loadMigrations(*dir, m, error));
    return m;
}

va::StatAllocation points(const vg::Attributes& p, std::uint16_t level, std::uint8_t cls = 1)
{
    return {p, vg::pointsForLevel(level), vg::classStartAttributes(cls)};
}

} // namespace

TEST_CASE("Postgres: 0005 gives characters from before M11 every point of their level", "[db][stats][migrations]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema, false);
    const auto all = repoMigrations();
    REQUIRE(all.size() >= 5);
    REQUIRE(all[4].version == 5);
    REQUIRE(all[4].name == "stats");
    std::string error;
    {
        // The database as M10 left it: 0001-0004 and a level-7 character.
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error, 0);
        REQUIRE(conn);
        const std::vector<vdb::Migration> before(all.begin(), all.begin() + 4);
        vdb::MigrationReport report;
        REQUIRE(vdb::runMigrations(*conn, schema->name(), before, report, error));
        CHECK(report.currentVersion == 4);
        pqxx::nontransaction tx(*conn);
        const auto hash = *vn::hashPassword("password1");
        tx.exec("INSERT INTO accounts (username, username_key, password_hash) VALUES ('pg_old', 'pg_old', $1)",
                pqxx::params{hash});
        tx.exec("INSERT INTO characters (account_id, name, name_key, race, class, face, hair, level, zone_id, x, y, z, "
                "yaw, xp, coins) SELECT id, 'Eskici', 'eskici', 1, 3, 0, 0, 7, 1, 100, 0, 100, 0, 55, 12 FROM accounts");
    }
    // vo_server applies 0005 at its next start.
    vdb::MigrationReport report;
    REQUIRE(vdb::migrateDatabase(schema->settings(), std::nullopt, report, error));
    CHECK(report.applied == std::vector<int>{5, 6, 7}); // M12: 0006_skills follows, M14: 0007_bar_items
    CHECK(text(*schema, "SELECT stat_strength + stat_dexterity + stat_vitality + stat_intelligence + stat_spirit "
                        "FROM characters WHERE name_key = 'eskici'") == "0");

    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    va::CharacterInfo info;
    REQUIRE(svc.findCharacterByName("Eskici", info, error) == AR::Ok);
    CHECK(info.level == 7);
    CHECK(info.xp == 55);
    CHECK(info.coins == 12);
    CHECK(info.statPoints == vg::Attributes{});
    CHECK(vg::unspentPoints(info.level, info.statPoints) == 18);
    // And they can spend them.
    va::AccountId account = 0;
    REQUIRE(svc.findAccount("pg_old", account, error) == AR::Ok);
    const auto out = await<va::StatAllocationOutcome>(
        [&](auto done) { svc.allocateStats(account, info.id, points({0, 0, 0, 18, 0}, 7, 3), done); });
    CHECK(out.ok);
    CHECK(out.allocated == vg::Attributes{0, 0, 0, 18, 0});
    svc.stop();
}

TEST_CASE("Postgres: allocateStats - budget, cap, ownership, the stored-level CHECK, constraints, admin helpers",
          "[db][stats]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    const auto account = svc.createAccount("pg_stats", "password1");
    REQUIRE(account);
    const auto created = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(*account, {"Puanlı", 1, 1, 0, 0}, done); });
    REQUIRE(created.result == vn::CharCreateResult::Ok);
    const auto id = created.character.id;
    const auto ch = std::to_string(id);
    CHECK(created.character.statPoints == vg::Attributes{});
    const auto alloc = [&](const vg::Attributes& p, std::uint16_t level) {
        return await<va::StatAllocationOutcome>([&](auto done) { svc.allocateStats(*account, id, points(p, level), done); });
    };

    // Level 1: no points; the stored level is the last word (the CHECK) even with a larger budget.
    CHECK_FALSE(alloc({1, 0, 0, 0, 0}, 1).ok);
    CHECK_FALSE(alloc({1, 0, 0, 0, 0}, 5).ok); // stored level 1: characters_stat_budget
    REQUIRE(await<bool>([&](auto done) {
        svc.saveCharacterState(*account, id, {1, 100.0f, 0.0f, 100.0f, 0.0f}, {4, 0, 50, 10}, done);
    }));
    // Level 4: 9 points, in several writes; each answer carries the stored allocation.
    auto out = alloc({2, 0, 1, 0, 0}, 4);
    CHECK(out.ok);
    CHECK(out.allocated == vg::Attributes{2, 0, 1, 0, 0});
    out = alloc({0, 3, 0, 0, 3}, 4);
    CHECK(out.ok);
    CHECK(out.allocated == vg::Attributes{2, 3, 1, 0, 3});
    CHECK_FALSE(alloc({0, 0, 0, 1, 0}, 4).ok); // 10 > 9: nothing changes
    CHECK(text(*schema, "SELECT stat_intelligence FROM characters WHERE id = " + ch) == "0");
    // Loads carry it (world entry, admin lookup).
    auto loaded = await<va::LoadCharacterOutcome>([&](auto done) { svc.loadCharacterForWorld(*account, id, done); });
    REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
    CHECK(loaded.character.statPoints == vg::Attributes{2, 3, 1, 0, 3});
    // Another account's character is not written.
    const auto other = svc.createAccount("pg_stats2", "password1");
    REQUIRE(other);
    CHECK_FALSE(await<va::StatAllocationOutcome>([&](auto done) {
                    svc.allocateStats(*other, id, points({0, 0, 0, 0, 0}, 10), done);
                }).ok);
    // The cap: class start + allocated <= 255 (a budget no level gives, to reach it).
    {
        std::string error;
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(conn);
        pqxx::nontransaction tx(*conn);
        tx.exec("ALTER TABLE characters DROP CONSTRAINT characters_stat_budget");
        tx.exec("UPDATE characters SET stat_strength = 243 WHERE id = " + ch); // 12 + 243 = 255
    }
    CHECK_FALSE(await<va::StatAllocationOutcome>([&](auto done) {
                    svc.allocateStats(*account, id, {{1, 0, 0, 0, 0}, 100000, vg::classStartAttributes(1)}, done);
                }).ok);
    // Constraints.
    {
        std::string error;
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(conn);
        pqxx::nontransaction tx(*conn);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET stat_spirit = -1 WHERE id = " + ch), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET stat_vitality = 256 WHERE id = " + ch), pqxx::check_violation);
        tx.exec("UPDATE characters SET stat_strength = 2 WHERE id = " + ch);
        tx.exec("ALTER TABLE characters ADD CONSTRAINT characters_stat_budget CHECK (stat_strength + stat_dexterity + "
                "stat_vitality + stat_intelligence + stat_spirit <= 3 * (level - 1))");
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET stat_dexterity = 30 WHERE id = " + ch), pqxx::check_violation);
    }

    // vo_admin: show-character (findCharacterByName), set-level keeps what fits / resets the rest,
    // reset-stats.
    va::CharacterInfo info;
    std::string error;
    REQUIRE(svc.findCharacterByName("Puanlı", info, error) == AR::Ok);
    CHECK(info.statPoints == vg::Attributes{2, 3, 1, 0, 3});
    REQUIRE(svc.setLevel("Puanlı", 10, info, error) == AR::Ok);
    CHECK(info.statPoints == vg::Attributes{2, 3, 1, 0, 3}); // 9 <= 27: kept
    REQUIRE(svc.setLevel("Puanlı", 3, info, error) == AR::Ok);
    CHECK(info.statPoints == vg::Attributes{}); // 9 > 6: reset
    CHECK(text(*schema, "SELECT stat_dexterity FROM characters WHERE id = " + ch) == "0");
    REQUIRE(svc.setLevel("Puanlı", 10, info, error) == AR::Ok);
    CHECK(alloc({5, 0, 5, 0, 0}, 10).ok);
    REQUIRE(svc.resetStats("Puanlı", info, error) == AR::Ok);
    CHECK(info.statPoints == vg::Attributes{});
    CHECK(text(*schema, "SELECT stat_strength + stat_vitality FROM characters WHERE id = " + ch) == "0");
    CHECK(svc.resetStats("Yokbuisim", info, error) == AR::NotFound);
    CHECK(svc.resetStats("x", info, error) == AR::Invalid);
    svc.stop();
}

TEST_CASE("World + PostgreSQL: an allocation is stored before its answer and survives a restart", "[db][server][stats]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    std::uint32_t charId = 0;
    {
        vdb::ConnectionPool pool(schema->settings(2));
        std::string error;
        REQUIRE(pool.warmUp(error));
        va::PostgresAccountService accounts(pool, {true, 2, vo::tests::testTerrainSpawns()});
        vo::tests::TestServer::Options opts;
        opts.accounts = &accounts;
        opts.world.statsInterval = 0s;
        vo::tests::TestServer server({}, opts);
        vo::tests::TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("pg_alloc", "Dağıtan");
        charId = c.lastCharId();
        c.send(vn::LeaveWorldReq{});
        REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
        va::CharacterInfo info;
        REQUIRE(accounts.setLevel("Dağıtan", 6, info, error) == AR::Ok); // 15 points
        c.enter(charId);
        CHECK(c.lastCharStats().unspent == 15);
        c.send(vn::StatAllocateReq{1, {10, 0, 5, 0, 0}});
        const auto res = c.waitFor<vn::StatAllocateRes>([](const vn::StatAllocateRes&) { return true; });
        REQUIRE(res);
        CHECK(res->result == vn::StatAllocateResult::Ok);
        CHECK(res->unspent == 0);
        CHECK(text(*schema, "SELECT stat_strength || '/' || stat_vitality FROM characters WHERE id = " +
                                std::to_string(charId)) == "10/5");
        const auto stats = c.waitFor<vn::CharStats>([](const vn::CharStats& s) { return s.attributes[0].base == 22; });
        REQUIRE(stats);
        CHECK(stats->attack == vg::deriveStats(1, 6, {10, 0, 5, 0, 0}, {6, 4}).attack);
        c.send(vn::Logout{});
        REQUIRE(c.waitClosed());
        server.stop(5s);
    }
    // A new server: the same allocation on entry.
    vdb::ConnectionPool pool(schema->settings(2));
    std::string error;
    REQUIRE(pool.warmUp(error));
    va::PostgresAccountService accounts(pool, {true, 2, vo::tests::testTerrainSpawns()});
    vo::tests::TestServer::Options opts;
    opts.accounts = &accounts;
    opts.world.statsInterval = 0s;
    vo::tests::TestServer server({}, opts);
    vo::tests::TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("pg_alloc") == vn::LoginResult::Ok);
    const auto e = c.enter(charId);
    CHECK(c.lastCharStats().attributes[0].base == 22);
    CHECK(c.lastCharStats().attributes[2].base == 16);
    CHECK(c.lastCharStats().unspent == 0);
    CHECK(e.maxHp == vg::deriveStats(1, 6, {10, 0, 5, 0, 0}, {6, 4}).maxHp);
}
