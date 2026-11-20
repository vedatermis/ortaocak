// PostgreSQL-only item tests (fresh schema in vedatonline_test; SKIP without it): the kit is granted
// exactly once when several servers load the same character at once, the 0002 schema constraints,
// purge/delete cascade to the items, the vo_admin helpers, and a real Server + World on PostgreSQL
// where moves are committed before they are acknowledged and survive a new login.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "shared/game/Items.h"
#include "shared/net/Crypto.h"
#include "tests/db/TestDb.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace va = vo::server::auth;
namespace vdb = vo::server::db;
namespace vg = vo::game;
namespace vn = vo::net;
using AR = va::PostgresAccountService::AdminResult;
using namespace std::chrono_literals;

namespace {

struct Node
{
    Node(const vo::tests::TestSchema& schema, std::size_t threads = 2)
        : pool(schema.settings(threads)), svc(pool, {false, threads})
    {
    }
    ~Node() { svc.stop(); }
    vdb::ConnectionPool pool;
    va::PostgresAccountService svc;
};

template <class R, class F>
R await(F&& call)
{
    std::promise<R> p;
    auto f = p.get_future();
    call([&p](R r) { p.set_value(std::move(r)); });
    REQUIRE(f.wait_for(60s) == std::future_status::ready);
    return f.get();
}

std::int64_t scalar(const vo::tests::TestSchema& schema, const std::string& sql)
{
    std::string error;
    auto conn = vdb::openConnection(schema.settings(), schema.url(), error);
    REQUIRE(conn);
    pqxx::nontransaction tx(*conn);
    return tx.query_value<std::int64_t>(sql);
}

const vg::StarterKit& warriorKit()
{
    return *vo::tests::testItemCatalog()->kitFor(1);
}

} // namespace

TEST_CASE("Postgres: concurrent first entries on several servers grant the kit exactly once", "[db][items][concurrency]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 4; ++i)
        nodes.push_back(std::make_unique<Node>(*schema));
    const auto account = nodes[0]->svc.createAccount("kit_race", "password1");
    REQUIRE(account);
    const auto c = await<va::CreateCharacterOutcome>(
        [&](auto cb) { nodes[0]->svc.createCharacter(*account, {"Yarışçı", 1, 1, 0, 0}, cb); });
    REQUIRE(c.result == vn::CharCreateResult::Ok);

    std::mutex m;
    std::vector<va::LoadItemsOutcome> results;
    std::promise<void> all;
    constexpr int kLoads = 8;
    for (int i = 0; i < kLoads; ++i)
        nodes[static_cast<std::size_t>(i) % nodes.size()]->svc.loadCharacterItems(
            *account, c.character.id, warriorKit(), [&](va::LoadItemsOutcome o) {
                const std::scoped_lock lock(m);
                results.push_back(std::move(o));
                if (results.size() == kLoads)
                    all.set_value();
            });
    REQUIRE(all.get_future().wait_for(60s) == std::future_status::ready);
    int granted = 0;
    for (const auto& r : results)
    {
        CHECK(r.ok);
        CHECK(r.items.size() == 3);
        granted += r.kitGranted ? 1 : 0;
    }
    CHECK(granted == 1);
    CHECK(scalar(*schema, "SELECT count(*) FROM character_items") == 3);
    CHECK(scalar(*schema, "SELECT count(*) FROM characters WHERE starter_kit_granted") == 1);
}

TEST_CASE("Postgres: character_items constraints, cascade and admin helpers", "[db][items][schema][admin]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    Node node(*schema);
    auto& svc = node.svc;
    std::string error;
    va::AccountId account = 0;
    REQUIRE(svc.createAccount("item_admin", "password1", account, error) == AR::Ok);
    const auto c = await<va::CreateCharacterOutcome>(
        [&](auto cb) { svc.createCharacter(account, {"Eşyacı", 1, 1, 0, 0}, cb); });
    REQUIRE(c.result == vn::CharCreateResult::Ok);
    const std::string ch = std::to_string(c.character.id);

    auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
    REQUIRE(conn);
    const auto insertFails = [&](const std::string& values) {
        try
        {
            pqxx::work tx(*conn);
            tx.exec("INSERT INTO character_items (char_id, container, slot, template_id, quantity) VALUES " + values);
            tx.commit();
            return false;
        }
        catch (const pqxx::sql_error&)
        {
            return true;
        }
    };
    CHECK_FALSE(insertFails("(" + ch + ", 1, 27, 1, 1)"));
    CHECK(insertFails("(" + ch + ", 1, 27, 2, 1)")); // place taken
    CHECK(insertFails("(" + ch + ", 1, 28, 1, 1)")); // bag slot range
    CHECK(insertFails("(" + ch + ", 0, 8, 1, 1)"));  // equipment slot range
    CHECK(insertFails("(" + ch + ", 2, 0, 1, 1)"));  // container
    CHECK(insertFails("(" + ch + ", 1, 3, 0, 1)"));  // template 0
    CHECK(insertFails("(" + ch + ", 1, 3, 1, 0)"));  // quantity
    CHECK(insertFails("(999999, 1, 3, 1, 1)"));      // no such character
    CHECK_FALSE(insertFails("(" + ch + ", 0, 7, 1, 1)"));

    // vo_admin give-item / list-items: lowest free bag slot, NoFreeSlot when full.
    vg::StoredItem given;
    REQUIRE(svc.giveItem("EŞYACI", 6, given, error) == AR::Ok); // Turkish-aware name lookup
    CHECK(given.place == vg::ItemPlace::bag(0));
    CHECK(given.templateId == 6);
    REQUIRE(svc.giveItem("eşyacı", 1, given, error) == AR::Ok);
    CHECK(given.place == vg::ItemPlace::bag(1));
    CHECK(svc.giveItem("Kimseyok", 1, given, error) == AR::NotFound);
    CHECK(svc.giveItem("1x", 1, given, error) == AR::Invalid);
    CHECK(svc.giveItem("Eşyacı", 0, given, error) == AR::Invalid);
    for (int i = 2; i < 27; ++i)
        REQUIRE(svc.giveItem("Eşyacı", 1, given, error) == AR::Ok);
    CHECK(given.place == vg::ItemPlace::bag(26)); // 27 was taken by the insert above
    CHECK(svc.giveItem("Eşyacı", 1, given, error) == AR::NoFreeSlot);
    va::CharacterInfo info;
    std::vector<vg::StoredItem> items;
    REQUIRE(svc.listItems("Eşyacı", info, items, error) == AR::Ok);
    CHECK(info.id == c.character.id);
    CHECK(items.size() == 29);
    CHECK(items.front().place == vg::ItemPlace::equipment(vg::EquipSlot::Necklace)); // equipment first (slot 7: reserved before M18)
    CHECK(items.back().place == vg::ItemPlace::bag(27));

    // Soft delete keeps the items; purge (and account deletion) removes them.
    REQUIRE(await<va::DeleteCharacterOutcome>([&](auto cb) { svc.deleteCharacter(account, c.character.id, cb); }).result ==
            vn::CharDeleteResult::Ok);
    CHECK(svc.giveItem("Eşyacı", 1, given, error) == AR::NotFound); // only live characters
    CHECK(scalar(*schema, "SELECT count(*) FROM character_items") == 29);
    REQUIRE(svc.purgeCharacter("item_admin", c.character.id, error) == AR::Ok);
    CHECK(scalar(*schema, "SELECT count(*) FROM character_items") == 0);
}

TEST_CASE("World + PostgreSQL: item moves are stored before they are acknowledged", "[db][server][items]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    vdb::ConnectionPool pool(schema->settings(2));
    std::string error;
    REQUIRE(pool.warmUp(error));
    va::PostgresAccountService accounts(pool, {true, 2, vo::tests::testTerrainSpawns()});
    vo::tests::TestServer::Options opts;
    opts.accounts = &accounts;
    opts.world.statsInterval = 0s;
    vo::tests::TestServer server({}, opts);

    vo::tests::TestClient a;
    REQUIRE(a.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(a.login("pg_items") == vn::LoginResult::Ok);
    a.send(vn::CharCreateReq{"Kalıcıel", 1, 1, 0, 0});
    const auto created = a.expect<vn::CharCreateRes>();
    REQUIRE(created.result == vn::CharCreateResult::Ok);
    CHECK(created.character.mainHandTemplate == 1); // the kit, before the first entry
    a.enter(created.character.charId);
    REQUIRE(a.lastInventory().items.size() == 3);
    const auto ch = std::to_string(created.character.charId);
    CHECK(scalar(*schema, "SELECT count(*) FROM characters WHERE starter_kit_granted AND id = " + ch) == 1);

    // Axe into MainHand: committed when ITEM_MOVE_RES(Ok) arrives.
    REQUIRE(a.moveItem(1, 1, 0, 0, 0) == vn::ItemMoveResult::Ok);
    CHECK(scalar(*schema, "SELECT template_id FROM character_items WHERE container = 0 AND slot = 0 AND char_id = " +
                              ch) == 6);
    CHECK(scalar(*schema, "SELECT count(*) FROM character_items WHERE container = 0 AND slot = 1 AND char_id = " +
                              ch) == 0);
    CHECK(scalar(*schema, "SELECT template_id FROM character_items WHERE container = 1 AND slot = 1 AND char_id = " +
                              ch) == 2);
    // Pipelined swap back and forth on the same slots, then a refused one.
    a.send(vn::ItemMoveReq{2, 0, 0, 1, 0}); // axe <-> sword
    a.send(vn::ItemMoveReq{3, 1, 0, 0, 0}); // and back
    a.send(vn::ItemMoveReq{4, 1, 1, 0, 1}); // shield next to the axe: SlotBlocked
    std::vector<vn::ItemMoveRes> res;
    while (res.size() < 3)
    {
        const auto r = a.waitFor<vn::ItemMoveRes>([](const vn::ItemMoveRes&) { return true; });
        REQUIRE(r);
        res.push_back(*r);
    }
    CHECK(res[0] == vn::ItemMoveRes{2, vn::ItemMoveResult::Ok});
    CHECK(res[1] == vn::ItemMoveRes{3, vn::ItemMoveResult::Ok});
    CHECK(res[2] == vn::ItemMoveRes{4, vn::ItemMoveResult::SlotBlocked});
    a.close();

    // A new login sees exactly the committed state, in the list and in INVENTORY.
    vo::tests::TestClient b;
    REQUIRE(b.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(b.login("pg_items") == vn::LoginResult::Ok);
    b.send(vn::CharListReq{});
    const auto list = b.expect<vn::CharListRes>();
    REQUIRE(list.characters.size() == 1);
    CHECK(list.characters[0].mainHandTemplate == 6);
    CHECK(list.characters[0].offHandTemplate == 0);
    b.enter(created.character.charId);
    const auto& inv = b.lastInventory();
    REQUIRE(inv.items.size() == 3);
    CHECK(inv.items[0].container == 0);
    CHECK(inv.items[0].templateId == 6);
    CHECK(inv.items[1].container == 1);
    CHECK(inv.items[1].slot == 0);
    CHECK(inv.items[1].templateId == 1);
    CHECK(inv.items[2].slot == 1);
    CHECK(inv.items[2].templateId == 2);
    CHECK(scalar(*schema, "SELECT count(*) FROM character_items") == 3);
}
