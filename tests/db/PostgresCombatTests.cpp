// PostgreSQL combat persistence (fresh schema in vedatonline_test; SKIP without it): the 0003 columns
// (xp, hp, mp; NULL = full for new and old characters), saveCharacterState round trips, the
// constraints, vo_admin's set-level helper, and a real Server + World on PostgreSQL where a character
// that logs out dead is stored with hp 0 and enters again (on a new server) at its race spawn with
// 50 % hp/mp. docs/PROTOCOL_COMBAT.md §3.5, docs/DATABASE.md §3.2.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "shared/game/Combat.h"
#include "shared/game/Monsters.h"
#include "shared/net/Crypto.h"
#include "tests/db/TestDb.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <string>

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

std::shared_ptr<const vg::MonsterData> killerNextToTheSpawn()
{
    auto data = std::make_shared<vg::MonsterData>();
    std::string error;
    REQUIRE(vg::parseMonsterData(R"(
[[monster]]
id = 5
name = "Kor Bekçisi"
model = "ember_warden"
level = 6
hp = 900
attack = 60
defense = 14
attack_range = 2.6
attack_interval_ms = 1000
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 12.0
leash_radius = 40.0
xp = 300
respawn_ms = 90000

[[spawn]]
zone = 1
monster = 5
x = 108.0
z = 100.0
radius = 0.0
)",
                                 "test.toml", *data, error));
    return data;
}

} // namespace

TEST_CASE("Postgres: xp/hp/mp columns, saveCharacterState round trip, constraints, set-level",
          "[db][combat]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    const auto account = svc.createAccount("pg_combat", "password1");
    REQUIRE(account);
    const auto created = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(*account, {"Kalıcı", 1, 1, 0, 0}, done); });
    REQUIRE(created.result == vn::CharCreateResult::Ok);
    const auto id = created.character.id;
    const auto ch = std::to_string(id);
    // New characters: xp 0, hp/mp NULL (= full).
    CHECK(created.character.xp == 0);
    CHECK_FALSE(created.character.hp.has_value());
    CHECK(text(*schema, "SELECT hp FROM characters WHERE id = " + ch) == "NULL");
    auto loaded = await<va::LoadCharacterOutcome>([&](auto done) { svc.loadCharacterForWorld(*account, id, done); });
    REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
    CHECK_FALSE(loaded.character.mp.has_value());

    // Position and progress in one write.
    REQUIRE(await<bool>([&](auto done) {
        svc.saveCharacterState(*account, id, {1, 120.5f, 3.0f, 99.25f, 0.5f}, {4, 321, 0, 17}, done);
    }));
    loaded = await<va::LoadCharacterOutcome>([&](auto done) { svc.loadCharacterForWorld(*account, id, done); });
    CHECK(loaded.character.level == 4);
    CHECK(loaded.character.xp == 321);
    CHECK(loaded.character.hp == std::optional<std::uint32_t>(0)); // left dead
    CHECK(loaded.character.mp == std::optional<std::uint32_t>(17));
    CHECK(loaded.character.position.x == 120.5f);
    // Another account's character is not written.
    const auto other = svc.createAccount("pg_combat2", "password1");
    REQUIRE(other);
    CHECK_FALSE(await<bool>([&](auto done) { svc.saveCharacterState(*other, id, {}, {9, 9, 9, 9}, done); }));
    CHECK(text(*schema, "SELECT level FROM characters WHERE id = " + ch) == "4");

    // Constraints.
    {
        std::string error;
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(conn);
        pqxx::nontransaction tx(*conn);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET hp = -1 WHERE id = " + ch), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET mp = -5 WHERE id = " + ch), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET xp = -1 WHERE id = " + ch), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET xp = 4294967296 WHERE id = " + ch), pqxx::check_violation);
    }

    // vo_admin set-level: clamped to the cap, xp 0, hp/mp back to NULL (full on the next entry).
    va::CharacterInfo info;
    std::string error;
    REQUIRE(svc.setLevel("Kalıcı", 99, info, error) == AR::Ok);
    CHECK(info.level == vg::kLevelCap);
    CHECK(text(*schema, "SELECT level FROM characters WHERE id = " + ch) == "20"); // M15: the cap is 20

    CHECK(text(*schema, "SELECT xp FROM characters WHERE id = " + ch) == "0");
    CHECK(text(*schema, "SELECT hp FROM characters WHERE id = " + ch) == "NULL");
    CHECK(text(*schema, "SELECT mp FROM characters WHERE id = " + ch) == "NULL");
    REQUIRE(svc.setLevel("kalici", 0, info, error) == AR::Ok); // Turkish-aware name key; 0 -> 1
    CHECK(info.level == 1);
    CHECK(svc.setLevel("Yokbuisim", 3, info, error) == AR::NotFound);
    CHECK(svc.setLevel("x", 3, info, error) == AR::Invalid);
    svc.stop();
}

TEST_CASE("World + PostgreSQL: logging out dead stores hp 0; the next entry is at the race spawn with 50 %",
          "[db][server][combat]")
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
        opts.monsters = killerNextToTheSpawn();
        vo::tests::TestServer server({}, opts);
        vo::tests::TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        const auto e = c.enterNew("pg_dead", "Düşen");
        charId = c.lastCharId();
        REQUIRE(c.waitFor<vn::EntityDeath>([&](const vn::EntityDeath& d) { return d.entityId == e.entityId; }, 10s));
        c.send(vn::Logout{});
        REQUIRE(c.waitClosed());
        server.stop(5s); // drains the save
    }
    const auto ch = std::to_string(charId);
    CHECK(text(*schema, "SELECT hp FROM characters WHERE id = " + ch) == "0");
    CHECK(text(*schema, "SELECT mp FROM characters WHERE id = " + ch) == "34");
    // A new server (no in-memory state left): alive at the Aydor spawn with 50 % hp/mp.
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
    REQUIRE(c.login("pg_dead") == vn::LoginResult::Ok);
    const auto e = c.enter(charId);
    CHECK_FALSE(e.isDead);
    CHECK(e.hp == 45);
    CHECK(e.mp == 17);
    CHECK(std::hypot(e.x - 100.0f, e.z - 100.0f) <= 2.1f);
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    const auto hp = std::stoi(text(*schema, "SELECT hp FROM characters WHERE id = " + ch));
    CHECK((hp >= 45 && hp <= 90));
}
