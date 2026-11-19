// PostgreSQL and quests (M17; fresh schema in vedatonline_test; SKIP without it; docs/PROTOCOL_QUESTS.md §6,
// docs/DATABASE.md): 0010 adds character_quests to a database from M16 without touching its rows and checks
// its constraints; a quest write is one transaction (the quest row, the units taken, the rewards, the Akçe
// and the experience) that changes nothing when any part does not hold; a progress save skips rows that are
// no longer active; vo_admin set-quest / listQuests; and a real Server + World on PostgreSQL where a hand-in
// is stored before its answer and the rows come back after a relog and after a restart of the server.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "server/db/Migrations.h"
#include "server/net/ServerContext.h"
#include "shared/game/Npcs.h"
#include "shared/game/Quests.h"
#include "shared/net/Crypto.h"
#include "tests/db/TestDb.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <future>
#include <string>
#include <vector>

namespace va = vo::server::auth;
namespace vdb = vo::server::db;
namespace vg = vo::game;
namespace vn = vo::net;
using AR = va::PostgresAccountService::AdminResult;
using QS = vn::QuestState;
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

} // namespace

TEST_CASE("Postgres: 0010 adds character_quests and keeps the stored characters", "[db][quests][migrations]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema, false);
    const auto dir = vdb::locateMigrationsDir();
    REQUIRE(dir);
    std::vector<vdb::Migration> all;
    std::string error;
    REQUIRE(vdb::loadMigrations(*dir, all, error));
    REQUIRE(all.size() >= 10);
    REQUIRE(all[9].version == 10);
    REQUIRE(all[9].name == "quests");
    {
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error, 0);
        REQUIRE(conn);
        const std::vector<vdb::Migration> before(all.begin(), all.begin() + 9);
        vdb::MigrationReport report;
        REQUIRE(vdb::runMigrations(*conn, schema->name(), before, report, error));
        CHECK(report.currentVersion == 9);
        pqxx::nontransaction tx(*conn);
        const auto hash = *vn::hashPassword("password1");
        tx.exec("INSERT INTO accounts (username, username_key, password_hash) VALUES ('pg_q_old', 'pg_q_old', $1)",
                pqxx::params{hash});
        tx.exec("INSERT INTO characters (account_id, name, name_key, race, class, face, hair, level, zone_id, x, y, z, "
                "yaw, xp) SELECT id, 'Eskigorev', 'eskigorev', 1, 1, 0, 0, 4, 1, 100, 0, 100, 0, 37 FROM accounts");
    }
    vdb::MigrationReport report;
    REQUIRE(vdb::migrateDatabase(schema->settings(), std::nullopt, report, error));
    CHECK(report.applied == std::vector<int>{10});
    CHECK(text(*schema, "SELECT count(*) FROM character_quests") == "0");
    CHECK(text(*schema, "SELECT level || '/' || xp FROM characters") == "4/37");
    auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
    REQUIRE(conn);
    pqxx::nontransaction tx(*conn);
    const std::string ch = text(*schema, "SELECT id FROM characters");
    tx.exec("INSERT INTO character_quests (char_id, quest_id, state) VALUES (" + ch + ", 1, 1)");
    CHECK_THROWS_AS(tx.exec("INSERT INTO character_quests (char_id, quest_id, state) VALUES (" + ch + ", 1, 1)"),
                    pqxx::unique_violation);
    CHECK_THROWS_AS(tx.exec("INSERT INTO character_quests (char_id, quest_id, state) VALUES (" + ch + ", 0, 1)"),
                    pqxx::check_violation);
    CHECK_THROWS_AS(tx.exec("INSERT INTO character_quests (char_id, quest_id, state) VALUES (" + ch + ", 2, 3)"),
                    pqxx::check_violation);
    // A done row was handed in (completions >= 1 and a time).
    CHECK_THROWS_AS(tx.exec("INSERT INTO character_quests (char_id, quest_id, state) VALUES (" + ch + ", 2, 2)"),
                    pqxx::check_violation);
    CHECK_THROWS_AS(tx.exec("UPDATE character_quests SET progress1 = 1001"), pqxx::check_violation);
    tx.exec("INSERT INTO character_quests (char_id, quest_id, state, completions, completed_at) VALUES (" + ch +
            ", 2, 2, 1, now())");
    // Deleting the character takes its rows.
    tx.exec("DELETE FROM characters");
    CHECK(text(*schema, "SELECT count(*) FROM character_quests") == "0");
}

TEST_CASE("Postgres: a quest write is one transaction; progress saves; set-quest", "[db][quests]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    const auto account = svc.createAccount("pg_quests", "password1");
    REQUIRE(account);
    const auto created = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(*account, {"Görevli", 1, 1, 0, 0}, done); });
    REQUIRE(created.result == vn::CharCreateResult::Ok);
    const auto id = created.character.id;
    const auto ch = std::to_string(id);
    const auto kit = vo::tests::testItemCatalog()->kitFor(1);
    REQUIRE(kit);
    (void)await<va::LoadItemsOutcome>([&](auto done) { svc.loadCharacterItems(*account, id, *kit, done); });
    std::string error;
    va::CharacterInfo info;
    std::vector<vg::StoredItem> changed;
    REQUIRE(svc.giveItems("Görevli", *vo::tests::testItemCatalog(), 9, 5, changed, error) == AR::Ok); // Kül Tozu x5
    const vg::StoredItem ash = changed.at(0);
    const auto write = [&](va::QuestWrite w) {
        return await<va::QuestWriteOutcome>([&](auto done) { svc.applyQuestWrite(*account, id, std::move(w), done); });
    };
    const auto rowSql = "SELECT state || '/' || progress1 || '/' || completions FROM character_quests WHERE char_id = " + ch +
                        " AND quest_id = ";

    // Accept: a new row; a second accept of the same quest (a stale one) changes nothing.
    va::QuestWrite accept;
    accept.changes = {{3, QS::None, QS::Active, {}, false, true}};
    REQUIRE(write(accept).ok);
    CHECK(text(*schema, rowSql + "3") == "1/0/0");
    CHECK_FALSE(write(accept).ok);
    // Progress save (not strict): an active row takes the counter, a missing one is skipped.
    va::QuestWrite progress;
    progress.changes = {{3, QS::Active, QS::Active, {2, 0, 0}, false, false}, {4, QS::Active, QS::Active, {9, 0, 0}, false, false}};
    REQUIRE(write(progress).ok);
    CHECK(text(*schema, rowSql + "3") == "1/2/0");
    CHECK(text(*schema, "SELECT count(*) FROM character_quests WHERE quest_id = 4") == "0");

    // A hand-in whose item part does not hold (the stack holds 5, not 6): nothing changes, not even the row.
    va::QuestWrite handIn;
    handIn.changes = {{3, QS::Active, QS::Done, {}, true, true}};
    handIn.items.quantities = {{ash.id, ash.place, 6, 3}};
    handIn.items.inserts = {{vg::ItemPlace::bag(20), 166, 2}};
    handIn.items.coinsAdded = 25;
    handIn.xp = 90;
    CHECK_FALSE(write(handIn).ok);
    CHECK(text(*schema, rowSql + "3") == "1/2/0");
    CHECK(text(*schema, "SELECT quantity FROM character_items WHERE item_id = " + std::to_string(ash.id)) == "5");
    CHECK(text(*schema, "SELECT coins || '/' || level || '/' || xp FROM characters WHERE id = " + ch) == "0/1/0");
    // All parts hold: the row, the units, the stones, the Akçe and the experience (80 to level 2, 10 over).
    handIn.items.quantities = {{ash.id, ash.place, 5, 2}};
    const auto out = write(handIn);
    REQUIRE(out.ok);
    CHECK(out.coins == 25);
    REQUIRE(out.newIds.size() == 1);
    CHECK(out.now > 1'700'000'000);
    CHECK(text(*schema, rowSql + "3") == "2/0/1");
    CHECK(text(*schema, "SELECT completed_at IS NOT NULL FROM character_quests WHERE quest_id = 3") == "t");
    CHECK(text(*schema, "SELECT quantity FROM character_items WHERE item_id = " + std::to_string(ash.id)) == "2");
    CHECK(text(*schema, "SELECT quantity FROM character_items WHERE template_id = 166 AND char_id = " + ch) == "2");
    CHECK(text(*schema, "SELECT coins || '/' || level || '/' || xp FROM characters WHERE id = " + ch) == "25/2/10");
    // Handing in twice: the row is no longer active.
    handIn.items = {};
    handIn.xp = 0;
    CHECK_FALSE(write(handIn).ok);
    // A repeat of a done quest (Done -> Active), then abandoned back to Done: completions and the time stay.
    const std::string doneAt = text(*schema, "SELECT completed_at::text FROM character_quests WHERE quest_id = 3");
    va::QuestWrite repeat;
    repeat.changes = {{3, QS::Done, QS::Active, {}, false, true}};
    REQUIRE(write(repeat).ok);
    CHECK(text(*schema, rowSql + "3") == "1/0/1");
    va::QuestWrite abandon;
    abandon.changes = {{3, QS::Active, QS::Done, {}, false, true}};
    REQUIRE(write(abandon).ok);
    CHECK(text(*schema, rowSql + "3") == "2/0/1");
    CHECK(text(*schema, "SELECT completed_at::text FROM character_quests WHERE quest_id = 3") == doneAt);
    // Abandoning a first try deletes the row.
    accept.changes = {{5, QS::None, QS::Active, {}, false, true}};
    REQUIRE(write(accept).ok);
    abandon.changes = {{5, QS::Active, QS::None, {}, false, true}};
    REQUIRE(write(abandon).ok);
    CHECK(text(*schema, "SELECT count(*) FROM character_quests WHERE quest_id = 5") == "0");

    // Loaded as records (completedAt in Unix seconds).
    const auto loaded = await<va::LoadQuestsOutcome>([&](auto done) { svc.loadQuests(*account, id, done); });
    REQUIRE(loaded.ok);
    REQUIRE(loaded.quests.size() == 1);
    CHECK(loaded.quests[0].quest == 3);
    CHECK(loaded.quests[0].state == QS::Done);
    CHECK(loaded.quests[0].completions == 1);
    CHECK(loaded.quests[0].completedAt >= out.now - 5);
    CHECK_FALSE(await<va::LoadQuestsOutcome>([&](auto done) { svc.loadQuests(*account + 99, id, done); }).ok);

    // vo_admin set-quest / listQuests.
    vg::QuestRecord r;
    r.quest = 7;
    r.progress = {4, 0, 0};
    REQUIRE(svc.setQuest("Görevli", r, QS::Active, 0, info, error) == AR::Ok);
    CHECK(text(*schema, rowSql + "7") == "1/4/0");
    REQUIRE(svc.setQuest("Görevli", r, QS::Done, 3600, info, error) == AR::Ok);
    CHECK(text(*schema, rowSql + "7") == "2/0/1");
    CHECK(text(*schema, "SELECT extract(epoch FROM now() - completed_at)::int / 60 FROM character_quests WHERE quest_id = 7") == "60");
    std::vector<vg::QuestRecord> listed;
    REQUIRE(svc.listQuests("Görevli", info, listed, error) == AR::Ok);
    CHECK(listed.size() == 2);
    REQUIRE(svc.setQuest("Görevli", r, QS::None, 0, info, error) == AR::Ok);
    CHECK(text(*schema, "SELECT count(*) FROM character_quests WHERE quest_id = 7") == "0");
    r.quest = 0;
    CHECK(svc.setQuest("Görevli", r, QS::Active, 0, info, error) == AR::Invalid);
    r.quest = 7;
    CHECK(svc.setQuest("Kimseyok", r, QS::Active, 0, info, error) == AR::NotFound);
    svc.stop();
}

namespace {

void installQuests()
{
    auto c = std::make_shared<vg::QuestCatalog>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseQuestCatalog("[[quest]]\nid = 1\nname = \"K\"\ngiver = 1\nturn_in = 2\nstory = \"s\"\ndone_text = \"d\"\n"
                                  "[[quest.objective]]\ntype = \"talk\"\nnpc = 2\n[quest.reward]\nxp = 30\ncoins = 10\n"
                                  "items = [[166, 2]]\n"
                                  "[[quest]]\nid = 2\nname = \"T\"\ngiver = 1\nstory = \"s\"\ndone_text = \"d\"\n"
                                  "[[quest.objective]]\ntype = \"kill\"\nmonsters = [1]\ncount = 5\n[quest.reward]\nxp = 5\n",
                                  "quests.toml", vo::tests::testItemCatalog().get(), nullptr, nullptr, *c, error));
    vg::installQuestCatalog(std::move(c));
}

std::shared_ptr<const vg::NpcCatalog> twoNpcs()
{
    auto npcs = std::make_shared<vg::NpcCatalog>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseNpcCatalog("[[npc]]\nid = 1\nname = \"A\"\ntitle = \"T\"\ngreeting = \"G\"\nmodel = \"npc_a\"\n"
                                "zone = 1\nx = 104.2\nz = 100.0\nyaw = 0.0\nkind = \"quest\"\n"
                                "[[npc]]\nid = 2\nname = \"B\"\ntitle = \"T\"\ngreeting = \"G\"\nmodel = \"npc_b\"\n"
                                "zone = 1\nx = 100.0\nz = 104.2\nyaw = 0.0\nkind = \"quest\"\n",
                                "npcs.toml", vo::tests::testItemCatalog().get(), *npcs, error));
    return npcs;
}

struct Ids
{
    std::uint32_t a = 0, b = 0;
};

Ids npcIds(vo::tests::TestClient& c)
{
    Ids ids;
    for (int i = 0; i < 2; ++i)
    {
        const auto s = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& e) { return e.entityType == vn::kEntityTypeNpc; });
        REQUIRE(s);
        (s->templateId == 1 ? ids.a : ids.b) = s->entityId;
    }
    REQUIRE(ids.a != 0);
    REQUIRE(ids.b != 0);
    return ids;
}

vn::QuestRes questRes(vo::tests::TestClient& c, std::uint32_t seq)
{
    const auto r = c.waitFor<vn::QuestRes>([&](const vn::QuestRes& x) { return x.seq == seq; });
    REQUIRE(r);
    return *r;
}

} // namespace

TEST_CASE("Postgres: a real server stores a hand-in before the answer; quests survive a relog and a restart",
          "[db][quests]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    installQuests();
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {true, 2, vo::tests::testTerrainSpawns()});
    vo::tests::TestServer::Options o;
    o.accounts = &svc;
    o.npcs = twoNpcs();
    o.world.zone.spawnSpread = 0.0f;
    std::uint32_t charId = 0;
    std::string ch;
    {
        vo::tests::TestServer server({}, o);
        vo::tests::TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("pg_quester", "Haberci");
        charId = c.lastCharId();
        ch = std::to_string(charId);
        const Ids npc = npcIds(c);
        c.send(vn::QuestAcceptReq{1, npc.a, 1});
        REQUIRE(questRes(c, 1).result == vn::QuestResult::Ok);
        CHECK(text(*schema, "SELECT state FROM character_quests WHERE quest_id = 1 AND char_id = " + ch) == "1");
        c.pumpFor(250ms);
        c.send(vn::QuestHandInReq{2, npc.b, 1, vn::kQuestNoChoice});
        const auto res = questRes(c, 2);
        REQUIRE(res.result == vn::QuestResult::Ok);
        // Stored before the answer: the row, the stones, the Akçe and the experience.
        CHECK(text(*schema, "SELECT state || '/' || completions FROM character_quests WHERE quest_id = 1 AND char_id = " + ch) ==
              "2/1");
        CHECK(text(*schema, "SELECT quantity FROM character_items WHERE template_id = 166 AND char_id = " + ch) == "2");
        CHECK(text(*schema, "SELECT coins || '/' || xp FROM characters WHERE id = " + ch) == "10/30");
        c.pumpFor(250ms);
        c.send(vn::QuestAcceptReq{3, npc.a, 2});
        REQUIRE(questRes(c, 3).result == vn::QuestResult::Ok);
        // Relog: the list comes back from the database.
        c.send(vn::LeaveWorldReq{});
        REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
        c.enter(charId);
        const auto& list = c.lastQuestList().quests;
        REQUIRE(list.size() == 2);
        CHECK(list[0] == vn::QuestEntry{1, QS::Done, {}, 0});
        CHECK(list[1] == vn::QuestEntry{2, QS::Active, {0, 0, 0}, 0});
        // As if two kills had been counted (the world's counters are stored on leave: the in-process test
        // terrain has no monsters, so vo_admin set-quest stands in), then the server stops.
    }
    va::CharacterInfo info;
    std::string error;
    vg::QuestRecord r;
    r.quest = 2;
    r.progress = {2, 0, 0};
    REQUIRE(svc.setQuest("Haberci", r, QS::Active, 0, info, error) == AR::Ok);
    {
        // A new server process on the same database (its own account service: the first one was stopped with its server).
        vdb::ConnectionPool pool2(schema->settings(2));
        va::PostgresAccountService svc2(pool2, {true, 2, vo::tests::testTerrainSpawns()});
        o.accounts = &svc2;
        vo::tests::TestServer server({}, o);
        vo::tests::TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("pg_quester") == vn::LoginResult::Ok);
        c.enter(charId);
        const auto& list = c.lastQuestList().quests;
        REQUIRE(list.size() == 2);
        CHECK(list[1] == vn::QuestEntry{2, QS::Active, {2, 0, 0}, 0});
        const Ids npc = npcIds(c);
        c.send(vn::QuestAcceptReq{1, npc.a, 1});
        CHECK(questRes(c, 1).result == vn::QuestResult::AlreadyDone);
    }
    svc.stop();
}
