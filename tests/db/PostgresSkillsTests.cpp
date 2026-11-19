// PostgreSQL skill persistence (fresh schema in vedatonline_test; SKIP without it): migration 0006 on a
// database with characters from before M12 (no rows, NULL bar: every point unspent, default bar),
// learnSkill (the shared rules on the stored ranks and the lower of the live and stored level,
// ownership), setSkillBar, the constraints, the vo_admin reset-skills / set-level / show-character
// helpers, and a real Server + World on PostgreSQL where a learn and the bar are stored before their
// answers and survive a server restart. docs/PROTOCOL_SKILLS.md §6, docs/DATABASE.md.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "server/db/Migrations.h"
#include "shared/game/Combat.h"
#include "shared/game/Skills.h"
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
using R = vn::SkillLearnResult;
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

std::vector<vdb::Migration> repoMigrations()
{
    const auto dir = vdb::locateMigrationsDir();
    REQUIRE(dir);
    std::vector<vdb::Migration> m;
    std::string error;
    REQUIRE(vdb::loadMigrations(*dir, m, error));
    return m;
}

} // namespace

TEST_CASE("Postgres: 0006 gives characters from before M12 every skill point and the default bar",
          "[db][skills][migrations]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema, false);
    const auto all = repoMigrations();
    REQUIRE(all.size() >= 6);
    REQUIRE(all[5].version == 6);
    REQUIRE(all[5].name == "skills");
    std::string error;
    {
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error, 0);
        REQUIRE(conn);
        const std::vector<vdb::Migration> before(all.begin(), all.begin() + 5);
        vdb::MigrationReport report;
        REQUIRE(vdb::runMigrations(*conn, schema->name(), before, report, error));
        CHECK(report.currentVersion == 5);
        pqxx::nontransaction tx(*conn);
        const auto hash = *vn::hashPassword("password1");
        tx.exec("INSERT INTO accounts (username, username_key, password_hash) VALUES ('pg_sk_old', 'pg_sk_old', $1)",
                pqxx::params{hash});
        tx.exec("INSERT INTO characters (account_id, name, name_key, race, class, face, hair, level, zone_id, x, y, z, "
                "yaw, xp) SELECT id, 'Eskiyol', 'eskiyol', 1, 2, 0, 0, 6, 1, 100, 0, 100, 0, 5 FROM accounts");
    }
    vdb::MigrationReport report;
    REQUIRE(vdb::migrateDatabase(schema->settings(), std::nullopt, report, error));
    CHECK(report.applied == std::vector<int>{6, 7, 8, 9}); // M14: 0007_bar_items follows, M15: 0008, M16: 0009
    CHECK(text(*schema, "SELECT count(*) FROM character_skills") == "0");
    CHECK(text(*schema, "SELECT skill_bar FROM characters WHERE name_key = 'eskiyol'") == "NULL");

    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    va::AccountId account = 0;
    REQUIRE(svc.findAccount("pg_sk_old", account, error) == AR::Ok);
    va::CharacterInfo info;
    REQUIRE(svc.findCharacterByName("Eskiyol", info, error) == AR::Ok);
    const auto loaded = await<va::LoadCharacterOutcome>([&](auto done) { svc.loadCharacterForWorld(account, info.id, done); });
    REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
    CHECK(loaded.character.skillRanks == vg::SkillRanks{});
    CHECK_FALSE(loaded.character.skillBar);
    CHECK(vg::unspentSkillPoints(vg::skills(), 2, 6, vg::normalizeSkillRanks(vg::skills(), 2, loaded.character.skillRanks)) == 5);
    // They can spend them.
    const auto out = await<va::SkillLearnOutcome>([&](auto done) { svc.learnSkill(account, info.id, {14, 2, 6}, done); });
    CHECK(out.result == R::Ok);
    CHECK(out.ranks[14] == 1);
    CHECK(out.ranks[2] == 1);
    svc.stop();
}

TEST_CASE("Postgres: learnSkill and setSkillBar - rules on the stored ranks, ownership, constraints, admin",
          "[db][skills]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    const auto account = svc.createAccount("pg_skills", "password1");
    REQUIRE(account);
    const auto created = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(*account, {"Yolcu", 1, 3, 0, 0}, done); }); // a mage
    REQUIRE(created.result == vn::CharCreateResult::Ok);
    const auto id = created.character.id;
    const auto ch = std::to_string(id);
    CHECK(created.character.skillRanks == vg::SkillRanks{});
    CHECK_FALSE(created.character.skillBar);
    const auto learn = [&](std::uint16_t skill, std::uint16_t level, std::uint8_t cls = 3) {
        return await<va::SkillLearnOutcome>([&](auto done) { svc.learnSkill(*account, id, {skill, cls, level}, done); });
    };
    const auto bar = [&](const vg::SkillBar& b, std::uint8_t cls = 3) {
        return await<va::SkillBarOutcome>([&](auto done) { svc.setSkillBar(*account, id, {b, cls}, done); });
    };

    // Stored level 1: no points, whatever the session believes (the lower level counts).
    CHECK(learn(3, 1).result == R::NotEnoughPoints);
    CHECK(learn(3, 5).result == R::NotEnoughPoints);
    REQUIRE(await<bool>([&](auto done) {
        svc.saveCharacterState(*account, id, {1, 100.0f, 0.0f, 100.0f, 0.0f}, {5, 0, 50, 10}, done);
    }));
    // Level 5: 4 points.
    CHECK(learn(17, 5).result == R::TierLocked);
    CHECK(learn(1, 5).result == R::UnknownSkill); // a warrior skill
    CHECK(learn(3, 5, 1).result == R::UnknownSkill); // wrong class claimed
    auto out = learn(3, 5);
    CHECK(out.result == R::Ok);
    CHECK(out.ranks[3] == 2);
    CHECK(text(*schema, "SELECT rank FROM character_skills WHERE skill_id = 3 AND char_id = " + ch) == "2");
    out = learn(3, 5);
    CHECK(out.ranks[3] == 3);
    CHECK(learn(3, 5).result == R::LevelTooLow); // M15: rank 4 needs level 11
    out = learn(17, 5);
    CHECK(out.result == R::Ok);
    CHECK(out.ranks[17] == 1);
    out = learn(20, 5);
    CHECK(out.result == R::Ok);
    CHECK(learn(21, 5).result == R::NotEnoughPoints);
    // Another account cannot write.
    const auto other = svc.createAccount("pg_skills2", "password1");
    REQUIRE(other);
    CHECK(await<va::SkillLearnOutcome>([&](auto done) { svc.learnSkill(*other, id, {18, 3, 10}, done); }).result ==
          R::ServerError);
    CHECK(await<va::SkillBarOutcome>([&](auto done) { svc.setSkillBar(*other, id, {{}, 3}, done); }).result ==
          vn::SkillBarResult::ServerError);

    // The bar.
    auto b = bar({3, 17, 0, 0, 0, 0, 0, 20});
    CHECK(b.result == vn::SkillBarResult::Ok);
    CHECK(text(*schema, "SELECT array_to_string(skill_bar, ',') FROM characters WHERE id = " + ch) == "3,17,0,0,0,0,0,20");
    b = bar({18, 0, 0, 0, 0, 0, 0, 0}); // not learned
    CHECK(b.result == vn::SkillBarResult::NotLearned);
    CHECK(b.bar == vg::SkillBar{3, 17, 0, 0, 0, 0, 0, 20});
    CHECK(bar({3, 0, 0, 0, 0, 0, 0, 0}, 1).result == vn::SkillBarResult::NotLearned); // wrong class claimed
    // Loads carry ranks and bar.
    auto loaded = await<va::LoadCharacterOutcome>([&](auto done) { svc.loadCharacterForWorld(*account, id, done); });
    REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
    CHECK(loaded.character.skillRanks[3] == 3);
    CHECK(loaded.character.skillRanks[17] == 1);
    CHECK(loaded.character.skillRanks[20] == 1);
    CHECK(loaded.character.skillBar == vg::SkillBar{3, 17, 0, 0, 0, 0, 0, 20});

    // Constraints.
    {
        std::string error;
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(conn);
        pqxx::nontransaction tx(*conn);
        CHECK_THROWS_AS(tx.exec("INSERT INTO character_skills VALUES (" + ch + ", 64, 1)"), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("INSERT INTO character_skills VALUES (" + ch + ", 30, 6)"), pqxx::check_violation); // M15 (0008): ranks 1..5
        CHECK_THROWS_AS(tx.exec("INSERT INTO character_skills VALUES (" + ch + ", 30, 0)"), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("INSERT INTO character_skills VALUES (" + ch + ", 3, 1)"), pqxx::unique_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET skill_bar = '{1,2,3}' WHERE id = " + ch), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET skill_bar = '{1,2,3,4,5,6,7,64}' WHERE id = " + ch),
                        pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET skill_bar = '{1,2,3,4,5,6,7,NULL}' WHERE id = " + ch),
                        pqxx::check_violation);
        // M14 (0007): -1..-32767 are item slots now; -32768 is still refused.
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET skill_bar = '{-32768,0,0,0,0,0,0,0}' WHERE id = " + ch),
                        pqxx::check_violation);
    }

    // vo_admin: show-character (loadSkills), set-level keeps what fits / resets the rest, reset-skills.
    va::CharacterInfo info;
    std::string error;
    REQUIRE(svc.loadSkills("Yolcu", info, error) == AR::Ok);
    CHECK(info.skillRanks[3] == 3);
    CHECK(info.skillBar == vg::SkillBar{3, 17, 0, 0, 0, 0, 0, 20});
    bool reset = true;
    REQUIRE(svc.setLevel("Yolcu", 10, info, error, &reset) == AR::Ok);
    CHECK_FALSE(reset); // 4 <= 9: kept
    REQUIRE(svc.setLevel("Yolcu", 4, info, error, &reset) == AR::Ok);
    CHECK(reset); // 4 > 3: reset
    CHECK(text(*schema, "SELECT count(*) FROM character_skills WHERE char_id = " + ch) == "0");
    CHECK(text(*schema, "SELECT skill_bar FROM characters WHERE id = " + ch) == "NULL");
    REQUIRE(svc.setLevel("Yolcu", 10, info, error) == AR::Ok);
    CHECK(learn(20, 10).result == R::Ok);
    CHECK(bar({20, 0, 0, 0, 0, 0, 0, 0}).result == vn::SkillBarResult::Ok);
    REQUIRE(svc.resetSkills("Yolcu", info, error) == AR::Ok);
    CHECK(text(*schema, "SELECT count(*) FROM character_skills WHERE char_id = " + ch) == "0");
    CHECK(text(*schema, "SELECT skill_bar FROM characters WHERE id = " + ch) == "NULL");
    CHECK(svc.resetSkills("Yokbuisim", info, error) == AR::NotFound);
    // Deleting the character row removes its skill rows (ON DELETE CASCADE).
    CHECK(learn(20, 10).result == R::Ok);
    {
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(conn);
        pqxx::nontransaction tx(*conn);
        tx.exec("DELETE FROM characters WHERE id = " + ch);
    }
    CHECK(text(*schema, "SELECT count(*) FROM character_skills") == "0");
    svc.stop();
}

TEST_CASE("World + PostgreSQL: a learn and the bar are stored before their answers and survive a restart",
          "[db][server][skills]")
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
        c.enterNew("pg_learn", "Öğrenen");
        charId = c.lastCharId();
        CHECK(c.lastSkillState() == vn::SkillState{0, 0, {{1, 1}}});
        CHECK(c.lastSkillBar().slots == vn::SkillBarSlots{1, 0, 0, 0, 0, 0, 0, 0});
        c.send(vn::LeaveWorldReq{});
        REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
        va::CharacterInfo info;
        REQUIRE(accounts.setLevel("Öğrenen", 4, info, error) == AR::Ok); // 3 points
        c.enter(charId);
        CHECK(c.lastSkillState().unspent == 3);
        c.send(vn::SkillLearnReq{1, 8});
        const auto res = c.waitFor<vn::SkillLearnRes>([](const vn::SkillLearnRes&) { return true; });
        REQUIRE(res);
        CHECK(*res == vn::SkillLearnRes{1, 8, R::Ok, 1, 2});
        CHECK(text(*schema, "SELECT rank FROM character_skills WHERE skill_id = 8 AND char_id = " + std::to_string(charId)) ==
              "1");
        c.send(vn::SkillBarSetReq{2, {8, 1, 0, 0, 0, 0, 0, 0}});
        const auto barRes = c.waitFor<vn::SkillBarSetRes>([](const vn::SkillBarSetRes&) { return true; });
        REQUIRE(barRes);
        CHECK(barRes->result == vn::SkillBarResult::Ok);
        CHECK(text(*schema, "SELECT array_to_string(skill_bar, ',') FROM characters WHERE id = " + std::to_string(charId)) ==
              "8,1,0,0,0,0,0,0");
        c.send(vn::Logout{});
        REQUIRE(c.waitClosed());
        server.stop(5s);
    }
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
        REQUIRE(c.login("pg_learn") == vn::LoginResult::Ok);
        c.enter(charId);
        CHECK(c.lastSkillState() == vn::SkillState{3, 2, {{1, 1}, {8, 1}}});
        CHECK(c.lastSkillBar().slots == vn::SkillBarSlots{8, 1, 0, 0, 0, 0, 0, 0});
        c.send(vn::Logout{});
        REQUIRE(c.waitClosed());
        server.stop(5s);
    }
}
