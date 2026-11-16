// PostgreSQL loot persistence (fresh schema in vedatonline_test; SKIP without it): the 0004 coins
// column (default 0, CHECK 0..1e9), applyItemWrite (quantity updates and deletes with the expected
// quantity, inserts, the coin increment returning the stored balance, all or nothing), coins on
// entry, vo_admin's set-coins and give-items (stacks first) helpers, and a real Server + World on
// PostgreSQL where a looted stack and the balance survive a server restart.
// docs/PROTOCOL_LOOT.md §6, docs/DATABASE.md §3.3.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/net/Crypto.h"
#include "tests/db/TestDb.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace va = vo::server::auth;
namespace vdb = vo::server::db;
namespace vg = vo::game;
namespace vn = vo::net;
using AR = va::PostgresAccountService::AdminResult;
using vg::ItemPlace;
using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t kAsh = 9, kCore = 11, kDagger2 = 12;

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

const vg::StoredItem* at(const std::vector<vg::StoredItem>& items, ItemPlace p)
{
    for (const auto& i : items)
        if (i.place == p)
            return &i;
    return nullptr;
}

} // namespace

TEST_CASE("Postgres: coins column, applyItemWrite all or nothing, set-coins, give-items stacks", "[db][loot]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    va::PostgresAccountService svc(pool, {false, 2});
    const auto account = svc.createAccount("pg_loot", "password1");
    REQUIRE(account);
    const auto created = await<va::CreateCharacterOutcome>(
        [&](auto done) { svc.createCharacter(*account, {"Ganimetçi", 1, 1, 0, 0}, done); });
    REQUIRE(created.result == vn::CharCreateResult::Ok);
    const auto id = created.character.id;
    const auto ch = std::to_string(id);
    CHECK(created.character.coins == 0);
    CHECK(text(*schema, "SELECT coins FROM characters WHERE id = " + ch) == "0");

    // Inserts and coins: ids come back in insert order, the balance from the database.
    va::ItemWrite w;
    w.inserts = {{ItemPlace::bag(3), kAsh, 18}, {ItemPlace::bag(4), kDagger2, 1}};
    w.coinsAdded = 25;
    auto out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, w, done); });
    REQUIRE(out.ok);
    REQUIRE(out.newIds.size() == 2);
    CHECK(out.coins == 25);
    auto items = await<va::LoadItemsOutcome>([&](auto done) { svc.loadCharacterItems(*account, id, std::nullopt, done); });
    REQUIRE(items.ok);
    REQUIRE(at(items.items, ItemPlace::bag(3)));
    CHECK(at(items.items, ItemPlace::bag(3))->id == out.newIds[0]);
    CHECK(at(items.items, ItemPlace::bag(3))->quantity == 18);
    const auto ashId = out.newIds[0];
    const auto daggerId = out.newIds[1];

    // A quantity change that does not match the stored stack fails as a whole: nothing changes.
    va::ItemWrite bad;
    bad.quantities = {{ashId, ItemPlace::bag(3), 17, 20}};
    bad.inserts = {{ItemPlace::bag(5), kCore, 1}};
    bad.coinsAdded = 5;
    CHECK_FALSE(await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, bad, done); }).ok);
    // An insert onto an occupied place fails too.
    va::ItemWrite occupied;
    occupied.inserts = {{ItemPlace::bag(4), kCore, 1}};
    occupied.coinsAdded = 1;
    CHECK_FALSE(await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, occupied, done); }).ok);
    // Another account's character is not written.
    const auto other = svc.createAccount("pg_loot2", "password1");
    REQUIRE(other);
    va::ItemWrite coinsOnly;
    coinsOnly.coinsAdded = 3;
    CHECK_FALSE(await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*other, id, coinsOnly, done); }).ok);
    CHECK(text(*schema, "SELECT coins FROM characters WHERE id = " + ch) == "25");
    CHECK(text(*schema, "SELECT count(*) FROM character_items WHERE char_id = " + ch) == "2");

    // Grow and delete (a merge), with the expected quantities.
    va::ItemWrite merge;
    merge.quantities = {{ashId, ItemPlace::bag(3), 18, 20}, {daggerId, ItemPlace::bag(4), 1, 0}};
    out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, merge, done); });
    REQUIRE(out.ok);
    CHECK(out.coins == 25); // a write without coins still reports the balance
    CHECK(text(*schema, "SELECT quantity FROM character_items WHERE item_id = " + std::to_string(ashId)) == "20");
    CHECK(text(*schema, "SELECT count(*) FROM character_items WHERE item_id = " + std::to_string(daggerId)) == "0");

    // vo_admin set-coins, and a loot gain afterwards adds to the stored value (not a cached one).
    va::CharacterInfo info;
    std::string error;
    REQUIRE(svc.setCoins("ganimetçi", 999'999'990, info, error) == AR::Ok);
    CHECK(info.coins == 999'999'990);
    CHECK(svc.setCoins("Ganimetçi", 1'000'000'001, info, error) == AR::Invalid);
    CHECK(svc.setCoins("Yokbuisim", 1, info, error) == AR::NotFound);
    coinsOnly.coinsAdded = 10;
    out = await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, coinsOnly, done); });
    REQUIRE(out.ok);
    CHECK(out.coins == 1'000'000'000);
    // Past the cap: refused, unchanged.
    coinsOnly.coinsAdded = 1;
    CHECK_FALSE(await<va::ItemWriteOutcome>([&](auto done) { svc.applyItemWrite(*account, id, coinsOnly, done); }).ok);
    auto loaded = await<va::LoadCharacterOutcome>([&](auto done) { svc.loadCharacterForWorld(*account, id, done); });
    REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
    CHECK(loaded.character.coins == 1'000'000'000);

    // The CHECK constraint itself.
    {
        auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
        REQUIRE(conn);
        pqxx::nontransaction tx(*conn);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET coins = -1 WHERE id = " + ch), pqxx::check_violation);
        CHECK_THROWS_AS(tx.exec("UPDATE characters SET coins = 1000000001 WHERE id = " + ch), pqxx::check_violation);
    }

    // vo_admin give-item with a count: the stack in bag 3 is full, so new stacks of 20 and the rest.
    const auto& catalog = *vo::tests::testItemCatalog();
    std::vector<vg::StoredItem> changed;
    REQUIRE(svc.giveItems("Ganimetçi", catalog, kAsh, 25, changed, error) == AR::Ok);
    REQUIRE(changed.size() == 2);
    CHECK(changed[0].place == ItemPlace::bag(0));
    CHECK(changed[0].quantity == 20);
    CHECK(changed[1].place == ItemPlace::bag(1));
    CHECK(changed[1].quantity == 5);
    REQUIRE(svc.giveItems("Ganimetçi", catalog, kAsh, 3, changed, error) == AR::Ok);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].place == ItemPlace::bag(1)); // stacks first
    CHECK(changed[0].quantity == 8);
    CHECK(svc.giveItems("Ganimetçi", catalog, 999, 1, changed, error) == AR::Invalid);
    CHECK(svc.giveItems("Ganimetçi", catalog, kAsh, 0, changed, error) == AR::Invalid);
    CHECK(svc.giveItems("Ganimetçi", catalog, kAsh, 20 * 28, changed, error) == AR::NoFreeSlot);
    CHECK(text(*schema, "SELECT sum(quantity) FROM character_items WHERE char_id = " + ch + " AND template_id = 9") ==
          "48");
    svc.stop();
}

TEST_CASE("World + PostgreSQL: looted stacks and coins are stored and come back after a restart",
          "[db][server][loot]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    auto monsters = std::make_shared<vg::MonsterData>();
    std::string error;
    REQUIRE(vg::parseMonsterData(R"(
[[monster]]
id = 1
name = "Kül Çömezi"
model = "ash_thrall"
level = 1
hp = 1
attack = 0
defense = 0
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 0.0
leash_radius = 40.0
xp = 25
respawn_ms = 600000

[[spawn]]
zone = 1
monster = 1
x = 102.0
z = 100.0
radius = 0.0
)",
                                 "test.toml", *monsters, error));
    auto loot = std::make_shared<vg::LootTables>();
    REQUIRE(vg::parseLootTables("[[table]]\nmonsters = [1]\ncoins = [9, 9]\n[[table.drop]]\nitem = 9\nchance = 1\n"
                                "count = [2, 2]\n",
                                "loot.toml", *vo::tests::testItemCatalog(), monsters.get(), *loot, error));
    std::uint32_t charId = 0;
    {
        vdb::ConnectionPool pool(schema->settings(2));
        REQUIRE(pool.warmUp(error));
        va::PostgresAccountService accounts(pool, {true, 2, vo::tests::testTerrainSpawns()});
        vo::tests::TestServer::Options opts;
        opts.accounts = &accounts;
        opts.world.statsInterval = 0s;
        opts.world.zone.spawnSpread = 0.0f;
        opts.monsters = monsters;
        opts.loot = loot;
        vo::tests::TestServer server({}, opts);
        vo::tests::TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("pg_looter", "Biriktiren");
        charId = c.lastCharId();
        CHECK(c.lastCoins() == vn::Coins{0, 0});
        const auto m = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& s) { return s.entityType == vn::kEntityTypeMonster; });
        REQUIRE(m);
        c.send(vn::AttackReq{m->entityId}); // 2 m away: in range from the spawn
        const auto l = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& s) { return s.entityType == vn::kEntityTypeLoot; });
        REQUIRE(l);
        c.send(vn::LootTakeReq{1, l->entityId, vn::kLootTakeAll});
        const auto res = c.waitFor<vn::LootTakeRes>([](const vn::LootTakeRes&) { return true; });
        REQUIRE(res);
        CHECK(res->result == vn::LootResult::Ok);
        c.send(vn::Logout{});
        REQUIRE(c.waitClosed());
        server.stop(5s);
    }
    const auto ch = std::to_string(charId);
    CHECK(text(*schema, "SELECT coins FROM characters WHERE id = " + ch) == "9");
    CHECK(text(*schema, "SELECT quantity FROM character_items WHERE char_id = " + ch + " AND template_id = 9") == "2");
    // A new server: INVENTORY has the stack, COINS the balance.
    vdb::ConnectionPool pool(schema->settings(2));
    REQUIRE(pool.warmUp(error));
    va::PostgresAccountService accounts(pool, {true, 2, vo::tests::testTerrainSpawns()});
    vo::tests::TestServer::Options opts;
    opts.accounts = &accounts;
    opts.world.statsInterval = 0s;
    vo::tests::TestServer server({}, opts);
    vo::tests::TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("pg_looter") == vn::LoginResult::Ok);
    c.enter(charId);
    CHECK(c.lastCoins() == vn::Coins{9, 0});
    bool found = false;
    for (const auto& e : c.lastInventory().items)
        found = found || (e.templateId == kAsh && e.quantity == 2);
    CHECK(found);
}
