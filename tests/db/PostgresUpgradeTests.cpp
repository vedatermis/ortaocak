// PostgreSQL and item upgrades (fresh schema in vedatonline_test; SKIP without it; docs/PROTOCOL_UPGRADE.md
// §6, docs/DATABASE.md): 0009 adds the upgrade level to a database from M15 without touching its rows and
// checks its range; an attempt's write (the item's level or its destruction, the stone, the fee) is one
// transaction that changes nothing when any part does not hold; vo_admin set-upgrade; and a real Server
// + World on PostgreSQL where an attempt is stored before its answer and comes back after a relog.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "server/db/Migrations.h"
#include "server/net/ServerContext.h"
#include "shared/game/Npcs.h"
#include "shared/game/Upgrade.h"
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

TEST_CASE("Postgres: 0009 adds the upgrade level and keeps the stored items", "[db][upgrade][migrations]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema, false);
    const auto dir = vdb::locateMigrationsDir();
    REQUIRE(dir);
    std::vector<vdb::Migration> all;
    std::string error;
    REQUIRE(vdb::loadMigrations(*dir, all, error));
    REQUIRE(all.size() >= 9);
    REQUIRE(all[8].version == 9);
    REQUIRE(all[8].name == "item_upgrades");
    {
        // An M15 database (0001-0008) with a character and two items.
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error, 0);
        REQUIRE(conn);
        const std::vector<vdb::Migration> before(all.begin(), all.begin() + 8);
        vdb::MigrationReport report;
        REQUIRE(vdb::runMigrations(*conn, schema->name(), before, report, error));
        CHECK(report.currentVersion == 8);
        pqxx::nontransaction tx(*conn);
        const auto hash = *vn::hashPassword("password1");
        tx.exec("INSERT INTO accounts (username, username_key, password_hash) VALUES ('pg_up_old', 'pg_up_old', $1)",
                pqxx::params{hash});
        tx.exec("INSERT INTO characters (account_id, name, name_key, race, class, face, hair, level, zone_id, x, y, z, "
                "yaw, xp) SELECT id, 'Eskitav', 'eskitav', 1, 1, 0, 0, 12, 1, 100, 0, 100, 0, 0 FROM accounts");
        tx.exec("INSERT INTO character_items (char_id, container, slot, template_id, quantity) SELECT id, 1, 0, 46, 1 "
                "FROM characters");
        tx.exec("INSERT INTO character_items (char_id, container, slot, template_id, quantity) SELECT id, 1, 1, 9, 7 "
                "FROM characters");
    }
    vdb::MigrationReport report;
    REQUIRE(vdb::migrateDatabase(schema->settings(), std::nullopt, report, error));
    CHECK(report.applied == std::vector<int>{9, 10});
    CHECK(text(*schema, "SELECT string_agg(upgrade::text, ',' ORDER BY slot) FROM character_items") == "0,0");
    CHECK(text(*schema, "SELECT string_agg(quantity::text, ',' ORDER BY slot) FROM character_items") == "1,7");
    {
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(conn);
        pqxx::nontransaction tx(*conn);
        tx.exec("UPDATE character_items SET upgrade = 10 WHERE slot = 0");
        CHECK_THROWS_AS(tx.exec("UPDATE character_items SET upgrade = 11 WHERE slot = 0"), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE character_items SET upgrade = -1 WHERE slot = 0"), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE character_items SET upgrade = 1 WHERE slot = 1"), pqxx::check_violation); // a stack
    }
    // Loaded with the level.
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    va::CharacterInfo info;
    std::vector<vg::StoredItem> items;
    REQUIRE(svc.listItems("Eskitav", info, items, error) == AR::Ok);
    REQUIRE(items.size() == 2);
    CHECK(items[0].upgrade == 10);
    CHECK(items[1].upgrade == 0);
    svc.stop();
}

TEST_CASE("Postgres: an upgrade attempt is one transaction; set-upgrade", "[db][upgrade]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    const auto account = svc.createAccount("pg_upgrade", "password1");
    REQUIRE(account);
    const auto created = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(*account, {"Tavlı", 1, 1, 0, 0}, done); }); // a warrior
    REQUIRE(created.result == vn::CharCreateResult::Ok);
    const auto id = created.character.id;
    const auto ch = std::to_string(id);
    const auto kit = vo::tests::testItemCatalog()->kitFor(1);
    REQUIRE(kit);
    (void)await<va::LoadItemsOutcome>([&](auto done) { svc.loadCharacterItems(*account, id, *kit, done); });
    va::CharacterInfo info;
    std::string error;
    REQUIRE(svc.setCoins("Tavlı", 100, info, error) == AR::Ok);
    std::vector<vg::StoredItem> changed;
    REQUIRE(svc.giveItems("Tavlı", *vo::tests::testItemCatalog(), 166, 3, changed, error) == AR::Ok);
    REQUIRE(changed.size() == 1);
    const vg::StoredItem stones = changed[0];
    std::vector<vg::StoredItem> items;
    REQUIRE(svc.listItems("Tavlı", info, items, error) == AR::Ok);
    vg::ItemInstanceId axe = 0;
    for (const auto& s : items)
        if (s.templateId == 6)
            axe = s.id;
    REQUIRE(axe != 0);
    const auto axeSql = "SELECT upgrade FROM character_items WHERE item_id = " + std::to_string(axe);
    const auto stoneSql = "SELECT quantity FROM character_items WHERE item_id = " + std::to_string(stones.id);
    const auto coinSql = "SELECT coins FROM characters WHERE id = " + ch;

    // The item is not at the level the write expects: nothing at all changes (stone, coins).
    va::ItemWrite w;
    w.quantities = {{stones.id, stones.place, 3, 2}};
    w.coinsRemoved = 6;
    w.upgrades = {{axe, 1, 2, false}};
    auto out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, w, done); });
    CHECK_FALSE(out.ok);
    CHECK(text(*schema, axeSql) == "0");
    CHECK(text(*schema, stoneSql) == "3");
    CHECK(text(*schema, coinSql) == "100");
    // Not enough Akçe for the fee: nothing changes either.
    w.upgrades = {{axe, 0, 1, false}};
    w.coinsRemoved = 101;
    out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, w, done); });
    CHECK_FALSE(out.ok);
    CHECK(text(*schema, axeSql) == "0");
    CHECK(text(*schema, stoneSql) == "3");
    // All parts hold: the level, the stone and the fee together.
    w.coinsRemoved = 6;
    out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, w, done); });
    REQUIRE(out.ok);
    CHECK(out.coins == 94);
    CHECK(text(*schema, axeSql) == "1");
    CHECK(text(*schema, stoneSql) == "2");
    CHECK(text(*schema, coinSql) == "94");
    // A destruction deletes the row (it must still be at its level).
    va::ItemWrite d;
    d.quantities = {{stones.id, stones.place, 2, 1}};
    d.coinsRemoved = 4;
    d.upgrades = {{axe, 1, 0, true}};
    out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, d, done); });
    REQUIRE(out.ok);
    CHECK(text(*schema, "SELECT count(*) FROM character_items WHERE item_id = " + std::to_string(axe)) == "0");
    CHECK(text(*schema, stoneSql) == "1");
    CHECK(text(*schema, coinSql) == "90");

    // vo_admin set-upgrade: a weapon to 0..10; a stack, an unknown item, 11 refused.
    REQUIRE(svc.listItems("Tavlı", info, items, error) == AR::Ok);
    const vg::ItemInstanceId sword = items.front().id; // equipped main hand
    vg::StoredItem s;
    CHECK(svc.setItemUpgrade("Tavlı", sword, 7, s, error) == AR::Ok);
    CHECK(s.upgrade == 7);
    CHECK(s.place == vg::ItemPlace::equipment(vg::EquipSlot::MainHand));
    CHECK(text(*schema, "SELECT upgrade FROM character_items WHERE item_id = " + std::to_string(sword)) == "7");
    REQUIRE(svc.giveItems("Tavlı", *vo::tests::testItemCatalog(), 9, 2, changed, error) == AR::Ok); // Kül Tozu x2
    CHECK(svc.setItemUpgrade("Tavlı", changed.at(0).id, 1, s, error) == AR::Invalid); // a stack
    CHECK(svc.setItemUpgrade("Tavlı", sword, 11, s, error) == AR::Invalid);
    CHECK(svc.setItemUpgrade("Tavlı", 999999999, 1, s, error) == AR::NotFound);
    CHECK(svc.setItemUpgrade("Kimseyok", sword, 1, s, error) == AR::NotFound);
    svc.stop();
}

TEST_CASE("Postgres: a real server stores an upgrade before the answer and loads it after a relog", "[db][upgrade]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {true, 2, vo::tests::testTerrainSpawns()});
    auto npcs = std::make_shared<vg::NpcCatalog>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseNpcCatalog("[[npc]]\nid = 1\nname = \"S\"\ntitle = \"T\"\ngreeting = \"G\"\nmodel = \"npc_t\"\n"
                                "zone = 1\nx = 104.2\nz = 100.0\nyaw = 0.0\nkind = \"merchant\"\nstock = [166]\n"
                                "upgrades = true\n",
                                "npcs.toml", vo::tests::testItemCatalog().get(), *npcs, error));
    vo::tests::TestServer::Options o;
    o.accounts = &svc;
    o.npcs = npcs;
    o.world.zone.spawnSpread = 0.0f;
    vo::server::net::ServerSettings settings;
    settings.testingUpgradeFailFrom = 11; // world.testing_upgrade_outcome = "success"
    vo::tests::TestServer server(settings, o);
    vo::tests::TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("pg_upgrader", "Tavcı"); // a warrior: the axe in bag 0
    const auto charId = c.lastCharId();
    const auto ch = std::to_string(charId);
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    va::CharacterInfo info;
    std::vector<vg::StoredItem> changed;
    REQUIRE(svc.setCoins("Tavcı", 200, info, error) == AR::Ok);
    REQUIRE(svc.giveItems("Tavcı", *vo::tests::testItemCatalog(), 166, 2, changed, error) == AR::Ok);
    c.enter(charId);
    std::uint64_t axe = 0;
    for (const auto& e : c.lastInventory().items)
        if (e.templateId == 6)
            axe = e.itemId;
    REQUIRE(axe != 0);
    const auto npc = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& s) { return s.entityType == vn::kEntityTypeNpc; });
    REQUIRE(npc);
    c.send(vn::ShopOpenReq{npc->entityId});
    REQUIRE(c.waitFor<vn::ShopOpenRes>([](const vn::ShopOpenRes& r) { return r.result == vn::ShopResult::Ok; }));

    c.send(vn::UpgradeReq{1, npc->entityId, 0, axe, 0, vn::kUpgradeAnyStone});
    const auto res = c.waitFor<vn::UpgradeRes>([](const vn::UpgradeRes& r) { return r.seq == 1; });
    REQUIRE(res);
    REQUIRE(res->result == vn::UpgradeResult::Success);
    // In the database before the answer arrived.
    CHECK(text(*schema, "SELECT upgrade FROM character_items WHERE item_id = " + std::to_string(axe)) == "1");
    CHECK(text(*schema, "SELECT quantity FROM character_items WHERE template_id = 166 AND char_id = " + ch) == "1");
    CHECK(text(*schema, "SELECT coins FROM characters WHERE id = " + ch) == std::to_string(200 - res->fee));
    // A relog loads it.
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    c.enter(charId);
    bool seen = false;
    for (const auto& e : c.lastInventory().items)
        if (e.itemId == axe)
        {
            seen = true;
            CHECK(e.upgrade == 1);
        }
    CHECK(seen);
    server.stop(5s);
    svc.stop();
}
