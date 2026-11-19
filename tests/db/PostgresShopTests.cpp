// PostgreSQL and merchants (fresh schema in vedatonline_test; SKIP without it, docs/PROTOCOL_SHOP.md §7,
// docs/DATABASE.md): 0007 widens the skill bar CHECK on a database from M13 without touching its
// rows, item slots are stored as -template and loaded back as 0x8000 | template, a purchase cannot
// take the balance below 0 (vo_admin set-coins meanwhile), and a real Server + World on PostgreSQL
// where a sale, a purchase and a consumed potion are in the database before their answers.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "server/db/Migrations.h"
#include "shared/game/Npcs.h"
#include "shared/game/Skills.h"
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

TEST_CASE("Postgres: 0007 widens the skill bar CHECK for item slots and keeps the stored bars",
          "[db][shop][migrations]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema, false);
    const auto dir = vdb::locateMigrationsDir();
    REQUIRE(dir);
    std::vector<vdb::Migration> all;
    std::string error;
    REQUIRE(vdb::loadMigrations(*dir, all, error));
    REQUIRE(all.size() >= 7);
    REQUIRE(all[6].version == 7);
    REQUIRE(all[6].name == "bar_items");
    {
        // An M13 database (0001-0006) with a stored bar and a default (NULL) one.
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error, 0);
        REQUIRE(conn);
        const std::vector<vdb::Migration> before(all.begin(), all.begin() + 6);
        vdb::MigrationReport report;
        REQUIRE(vdb::runMigrations(*conn, schema->name(), before, report, error));
        CHECK(report.currentVersion == 6);
        pqxx::nontransaction tx(*conn);
        const auto hash = *vn::hashPassword("password1");
        tx.exec("INSERT INTO accounts (username, username_key, password_hash) VALUES ('pg_sh_old', 'pg_sh_old', $1)",
                pqxx::params{hash});
        tx.exec("INSERT INTO characters (account_id, name, name_key, race, class, face, hair, level, zone_id, x, y, z, "
                "yaw, xp, skill_bar) SELECT id, 'Eskibar', 'eskibar', 1, 3, 0, 0, 3, 1, 100, 0, 100, 0, 0, "
                "'{3,0,0,0,0,0,0,0}' FROM accounts");
        tx.exec("INSERT INTO characters (account_id, name, name_key, race, class, face, hair, level, zone_id, x, y, z, "
                "yaw, xp) SELECT id, 'Bosbar', 'bosbar', 1, 3, 0, 0, 1, 1, 100, 0, 100, 0, 0 FROM accounts");
        // Before 0007 an item slot is refused.
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET skill_bar = '{3,-16,0,0,0,0,0,0}' WHERE name_key = 'eskibar'"),
                        pqxx::check_violation);
    }
    vdb::MigrationReport report;
    REQUIRE(vdb::migrateDatabase(schema->settings(), std::nullopt, report, error));
    CHECK(report.applied == std::vector<int>{7, 8, 9, 10}); // M15: 0008_skill_ranks_5 follows, M16: 0009_item_upgrades
    CHECK(text(*schema, "SELECT array_to_string(skill_bar, ',') FROM characters WHERE name_key = 'eskibar'") ==
          "3,0,0,0,0,0,0,0");
    CHECK(text(*schema, "SELECT skill_bar FROM characters WHERE name_key = 'bosbar'") == "NULL");
    {
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(conn);
        pqxx::nontransaction tx(*conn);
        tx.exec("UPDATE characters SET skill_bar = '{3,-16,-32767,0,0,0,0,63}' WHERE name_key = 'eskibar'");
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET skill_bar = '{3,-32768,0,0,0,0,0,0}' WHERE name_key = 'eskibar'"),
                        pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET skill_bar = '{3,64,0,0,0,0,0,0}' WHERE name_key = 'eskibar'"),
                        pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET skill_bar = '{3,-16}' WHERE name_key = 'eskibar'"),
                        pqxx::check_violation);
    }
    // Loaded as the wire's 0x8000 | template.
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    va::CharacterInfo info;
    REQUIRE(svc.loadSkills("Eskibar", info, error) == AR::Ok);
    REQUIRE(info.skillBar);
    CHECK(*info.skillBar == vg::SkillBar{3, vg::barItemSlot(16), vg::barItemSlot(0x7FFF), 0, 0, 0, 0, 63});
    svc.stop();
}

TEST_CASE("Postgres: item slots on the bar, purchases against the stored balance", "[db][shop]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    const auto account = svc.createAccount("pg_shop", "password1");
    REQUIRE(account);
    const auto created = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(*account, {"Alıcı", 1, 3, 0, 0}, done); }); // a mage
    REQUIRE(created.result == vn::CharCreateResult::Ok);
    const auto id = created.character.id;
    const auto ch = std::to_string(id);

    // The bar: stored as -template, checked syntactically by the service (the session knows the items).
    const vg::SkillBar bar{3, vg::barItemSlot(16), vg::barItemSlot(18), 0, 0, 0, 0, vg::barItemSlot(16)};
    auto b = await<va::SkillBarOutcome>([&](auto done) { svc.setSkillBar(*account, id, {bar, 3}, done); });
    CHECK(b.result == vn::SkillBarResult::Ok);
    CHECK(b.bar == bar);
    CHECK(text(*schema, "SELECT array_to_string(skill_bar, ',') FROM characters WHERE id = " + ch) ==
          "3,-16,-18,0,0,0,0,-16");
    b = await<va::SkillBarOutcome>(
        [&](auto done) { svc.setSkillBar(*account, id, {{3, vg::kBarItemFlag, 0, 0, 0, 0, 0, 0}, 3}, done); });
    CHECK(b.result == vn::SkillBarResult::NotLearned); // template 0
    CHECK(b.bar == bar);
    auto loaded = await<va::LoadCharacterOutcome>([&](auto done) { svc.loadCharacterForWorld(*account, id, done); });
    REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
    CHECK(loaded.character.skillBar == bar);

    // A purchase after vo_admin set-coins lowered the balance: refused, nothing written.
    va::CharacterInfo info;
    std::string error;
    REQUIRE(svc.setCoins("Alıcı", 20, info, error) == AR::Ok);
    (void)await<va::LoadItemsOutcome>([&](auto done) { svc.loadCharacterItems(*account, id, std::nullopt, done); });
    va::ItemWrite buy;
    buy.inserts = {{vg::ItemPlace::bag(5), 16, 2}};
    buy.coinsRemoved = 16;
    REQUIRE(svc.setCoins("Alıcı", 10, info, error) == AR::Ok);
    auto out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, buy, done); });
    CHECK_FALSE(out.ok);
    CHECK(text(*schema, "SELECT count(*) FROM character_items WHERE template_id = 16 AND char_id = " + ch) == "0");
    CHECK(text(*schema, "SELECT coins FROM characters WHERE id = " + ch) == "10");
    REQUIRE(svc.setCoins("Alıcı", 16, info, error) == AR::Ok);
    out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, buy, done); });
    REQUIRE(out.ok);
    CHECK(out.coins == 0);
    CHECK(text(*schema, "SELECT quantity FROM character_items WHERE template_id = 16 AND char_id = " + ch) == "2");
    svc.stop();
}

TEST_CASE("Postgres: a real server stores a sale, a purchase and a consumed potion before the answers",
          "[db][shop]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {true, 2, vo::tests::testTerrainSpawns()});
    auto npcs = std::make_shared<vg::NpcCatalog>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseNpcCatalog("[[npc]]\nid = 1\nname = \"S\"\ntitle = \"T\"\ngreeting = \"G\"\nmodel = \"npc_t\"\n"
                                "zone = 1\nx = 104.2\nz = 100.0\nyaw = 0.0\nkind = \"merchant\"\nstock = [16]\n",
                                "npcs.toml", vo::tests::testItemCatalog().get(), *npcs, error));
    vo::tests::TestServer::Options o;
    o.accounts = &svc;
    o.npcs = npcs;
    o.world.zone.spawnSpread = 0.0f;
    o.world.zone.testingEnterVitalsPercent = 10;
    vo::tests::TestServer server({}, o);
    vo::tests::TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("pg_shopper", "Satıcı"); // a warrior: the axe in bag 0
    const auto ch = std::to_string(c.lastCharId());
    const auto npc = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& s) { return s.entityType == vn::kEntityTypeNpc; });
    REQUIRE(npc);
    c.send(vn::ShopOpenReq{npc->entityId});
    REQUIRE(c.waitFor<vn::ShopOpenRes>([](const vn::ShopOpenRes& r) { return r.result == vn::ShopResult::Ok; }));

    c.send(vn::ShopSellReq{1, npc->entityId, 0, 1});
    const auto sold = c.waitFor<vn::ShopSellRes>([](const vn::ShopSellRes& r) { return r.seq == 1; });
    REQUIRE(sold);
    REQUIRE(sold->result == vn::ShopResult::Ok);
    CHECK(text(*schema, "SELECT coins FROM characters WHERE id = " + ch) == "10");
    CHECK(text(*schema, "SELECT count(*) FROM character_items WHERE template_id = 6 AND char_id = " + ch) == "0");

    c.send(vn::ShopBuyReq{2, npc->entityId, 16, 1});
    const auto bought = c.waitFor<vn::ShopBuyRes>([](const vn::ShopBuyRes& r) { return r.seq == 2; });
    REQUIRE(bought);
    REQUIRE(bought->result == vn::ShopResult::Ok);
    CHECK(text(*schema, "SELECT coins FROM characters WHERE id = " + ch) == "2");
    CHECK(text(*schema, "SELECT quantity FROM character_items WHERE template_id = 16 AND char_id = " + ch) == "1");

    c.send(vn::ItemUseReq{3, 16, vn::kItemUseAnyStack});
    const auto used = c.waitFor<vn::ItemUseRes>([](const vn::ItemUseRes& r) { return r.seq == 3; });
    REQUIRE(used);
    REQUIRE(used->result == vn::ItemUseResult::Ok);
    CHECK(text(*schema, "SELECT count(*) FROM character_items WHERE template_id = 16 AND char_id = " + ch) == "0");
    CHECK(c.waitFor<vn::ItemUsed>([](const vn::ItemUsed& u) { return u.hpRestored == 70; }));
    server.stop(5s);
    svc.stop();
}
