// Loot end to end on an in-process server (MemoryAccountService, test terrain, real sockets and the
// real 20 Hz world thread): what the session adds to the world's rules (docs/PROTOCOL_LOOT.md) -
// COINS on entry, a kill -> open -> take everything with ITEM_UPDATE / COINS / LOOT_TAKE_RES after
// the write, the stored rows and balance, stacking into existing stacks, a full bag (BagFull, the
// rest stays), merge moves, a disconnect while the write is running (committed, the container
// goes) and before it started (nothing written, the container keeps everything), flood and state
// rules.

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace vn = vo::net;
namespace vg = vo::game;
using vo::tests::TestClient;
using vo::tests::TestServer;
using LR = vn::LootResult;
using namespace std::chrono_literals;

namespace {

constexpr std::uint8_t kEquip = 0;
constexpr std::uint8_t kBag = 1;
constexpr std::uint16_t kSword = 1, kAsh = 9, kDagger2 = 12;

// One-hit thralls (hp 1, harmless) 4 m east of the Aydor spawn (100/100), and one 4 m north.
std::shared_ptr<const vg::MonsterData> thralls()
{
    const std::string toml = R"(
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
x = 104.0
z = 100.0
radius = 0.0

[[spawn]]
zone = 1
monster = 1
x = 100.0
z = 104.0
radius = 0.0
)";
    auto data = std::make_shared<vg::MonsterData>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseMonsterData(toml, "test.toml", *data, error));
    return data;
}

// 11 Akçe, 3 Kül Tozu and a Çömez Bıçağı from every thrall.
std::shared_ptr<const vg::LootTables> tables(const vg::MonsterData& m)
{
    auto t = std::make_shared<vg::LootTables>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseLootTables(R"(
[[table]]
monsters = [1]
coins = [11, 11]
[[table.drop]]
item = 9
chance = 1.0
count = [3, 3]
[[table.drop]]
item = 12
chance = 1.0
)",
                                "loot.toml", *vo::tests::testItemCatalog(), &m, *t, error));
    return t;
}

TestServer::Options withLoot()
{
    TestServer::Options o;
    o.monsters = thralls();
    o.loot = tables(*o.monsters);
    o.world.statsInterval = 0s;
    o.world.zone.spawnSpread = 0.0f; // enter exactly at 100/100
    return o;
}

std::uint32_t create(TestClient& c, const std::string& name)
{
    c.send(vn::CharCreateReq{name, 1, 1, 0, 0}); // Aydor warrior
    const auto res = c.expect<vn::CharCreateRes>();
    REQUIRE(res.result == vn::CharCreateResult::Ok);
    return res.character.charId;
}

// Monster spawns seen by a client (entity id by position), collected once after entering.
struct Thralls
{
    std::uint32_t east = 0;  // at 104/100
    std::uint32_t north = 0; // at 100/104
};
Thralls thrallIds(TestClient& c)
{
    Thralls t;
    for (int i = 0; i < 2; ++i)
    {
        const auto s = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& e) { return e.entityType == vn::kEntityTypeMonster; });
        REQUIRE(s);
        (s->x == 104.0f ? t.east : t.north) = s->entityId;
    }
    REQUIRE(t.east != 0);
    REQUIRE(t.north != 0);
    return t;
}

// Kills the thrall `monster` at (mx, mz) from 1.5 m away and returns the id of its container.
std::uint32_t killThrall(TestClient& c, std::uint32_t& seq, std::uint32_t monsterId, float mx, float mz)
{
    std::this_thread::sleep_for(1100ms); // a full movement allowance
    const float px = mx == 100.0f ? 100.0f : mx - 1.5f;
    const float pz = mz == 100.0f ? 100.0f : mz - 1.5f;
    c.send(vn::MoveReq{++seq, vn::kMoveFlagMoving | vn::kMoveFlagRunning, px, 0.0f, pz, 0.0f});
    c.send(vn::MoveReq{++seq, 0, px, 0.0f, pz, 0.0f});
    c.send(vn::AttackReq{monsterId});
    const auto loot = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& s) { return s.entityType == vn::kEntityTypeLoot; });
    REQUIRE(loot);
    CHECK(loot->state == vn::kEntityStateLootMine);
    CHECK(loot->name == "Kül Çömezi");
    CHECK(loot->templateId == 1);
    return loot->entityId;
}

struct TakeOutcome
{
    std::vector<vn::ItemUpdate> updates;
    std::vector<vn::Coins> coins;
    vn::LootTakeRes res;
};

// Sends LOOT_TAKE_REQ and collects everything up to its LOOT_TAKE_RES.
TakeOutcome take(TestClient& c, std::uint32_t seq, std::uint32_t loot, std::uint8_t what)
{
    c.send(vn::LootTakeReq{seq, loot, what});
    TakeOutcome out;
    for (;;)
    {
        auto m = c.next(5s);
        REQUIRE(m.has_value());
        if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::ItemUpdate))
        {
            vn::ItemUpdate u;
            REQUIRE(vn::decodeMessage(m->payload, u));
            out.updates.push_back(u);
        }
        else if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::Coins))
        {
            vn::Coins x;
            REQUIRE(vn::decodeMessage(m->payload, x));
            out.coins.push_back(x);
        }
        else if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::LootTakeRes))
        {
            REQUIRE(vn::decodeMessage(m->payload, out.res));
            if (out.res.seq == seq)
                return out;
        }
    }
}

std::uint32_t storedUnits(const std::vector<vg::StoredItem>& items, std::uint16_t templateId)
{
    std::uint32_t n = 0;
    for (const auto& i : items)
        if (i.templateId == templateId)
            n += i.quantity;
    return n;
}

} // namespace

TEST_CASE("Loot server: kill, open, take everything - written through, answered after the commit, persisted",
          "[server][loot]")
{
    TestServer server({}, withLoot());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("loot_full") == vn::LoginResult::Ok);
    const auto id = create(c, "Yağmacı");
    REQUIRE(server.accounts().setCoins(id, 5));
    c.enter(id);
    const Thralls thralls = thrallIds(c);
    CHECK(c.lastCoins() == vn::Coins{5, 0}); // the stored balance on entry
    std::uint32_t seq = 0;
    const auto loot = killThrall(c, seq, thralls.east, 104.0f, 100.0f);

    c.send(vn::LootOpenReq{loot});
    const auto open = c.waitFor<vn::LootOpenRes>([](const vn::LootOpenRes&) { return true; });
    REQUIRE(open);
    CHECK(*open == vn::LootOpenRes{loot, LR::Ok});
    const auto contents = c.expectSkippingWorld<vn::LootContents>();
    CHECK(contents == vn::LootContents{loot, 11, {{0, kAsh, 3}, {1, kDagger2, 1}}});

    const auto out = take(c, 77, loot, vn::kLootTakeAll);
    CHECK(out.res == vn::LootTakeRes{77, loot, LR::Ok});
    REQUIRE(out.coins.size() == 1);
    CHECK(out.coins[0] == vn::Coins{16, 11});
    // The bag had the kit's axe in slot 0: Kül Tozu lands in 1, the dagger in 2.
    REQUIRE(out.updates.size() == 1);
    REQUIRE(out.updates[0].items.size() == 2);
    CHECK(out.updates[0].items[0].slot == 1);
    CHECK(out.updates[0].items[0].templateId == kAsh);
    CHECK(out.updates[0].items[0].quantity == 3);
    CHECK(out.updates[0].items[1].slot == 2);
    CHECK(out.updates[0].items[1].templateId == kDagger2);
    const auto items = server.accounts().itemsOf(id);
    CHECK(storedUnits(items, kAsh) == 3);
    CHECK(storedUnits(items, kDagger2) == 1);
    CHECK(server.accounts().characterInfo(id)->coins == 16);
    // Emptied: the view closes and the container despawns.
    const auto closed = c.waitFor<vn::LootClosed>([](const vn::LootClosed&) { return true; });
    REQUIRE(closed);
    CHECK(*closed == vn::LootClosed{loot, vn::LootClosedReason::Gone});
    CHECK(c.waitFor<vn::EntityDespawn>([&](const vn::EntityDespawn& d) { return d.entityId == loot; }));

    // The second thrall's 3 Kül Tozu stack onto the first three.
    const auto loot2 = killThrall(c, seq, thralls.north, 100.0f, 104.0f);
    const auto out2 = take(c, 78, loot2, vn::kLootTakeAll);
    CHECK(out2.res.result == LR::Ok);
    REQUIRE_FALSE(out2.updates.empty());
    CHECK(out2.updates[0].items[0].slot == 1);
    CHECK(out2.updates[0].items[0].quantity == 6);
    CHECK(storedUnits(server.accounts().itemsOf(id), kAsh) == 6);

    // Leave and enter again: the same inventory and balance come back.
    c.send(vn::LeaveWorldReq{});
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    c.enter(id);
    CHECK(c.lastCoins() == vn::Coins{27, 0});
    bool sawStack = false;
    for (const auto& e : c.lastInventory().items)
        sawStack = sawStack || (e.container == kBag && e.slot == 1 && e.templateId == kAsh && e.quantity == 6);
    CHECK(sawStack);
}

TEST_CASE("Loot server: a full bag takes the coins and what stacks, the rest stays (BagFull)", "[server][loot]")
{
    TestServer server({}, withLoot());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("loot_bagfull") == vn::LoginResult::Ok);
    const auto id = create(c, "Doluçanta");
    REQUIRE(server.accounts().giveItem(id, kAsh, 18)); // bag 0: room for 2 more
    for (int i = 0; i < 26; ++i)
        REQUIRE(server.accounts().giveItem(id, kSword)); // bag 1..26; the kit's axe takes 27
    c.enter(id);
    std::uint32_t seq = 0;
    const auto loot = killThrall(c, seq, thrallIds(c).east, 104.0f, 100.0f);
    c.send(vn::LootOpenReq{loot});
    REQUIRE(c.waitFor<vn::LootOpenRes>([](const vn::LootOpenRes& r) { return r.result == LR::Ok; }));

    const auto out = take(c, 5, loot, vn::kLootTakeAll);
    CHECK(out.res.result == LR::BagFull);
    REQUIRE(out.coins.size() == 1);
    CHECK(out.coins[0] == vn::Coins{11, 11});
    REQUIRE(out.updates.size() == 1);
    REQUIRE(out.updates[0].items.size() == 1);
    CHECK(out.updates[0].items[0].slot == 0);
    CHECK(out.updates[0].items[0].quantity == 20);
    // One Kül Tozu and the dagger are back in the container under their old indices.
    const auto back = c.waitFor<vn::LootContents>([&](const vn::LootContents& lc) { return !lc.entries.empty(); });
    REQUIRE(back);
    CHECK(*back == vn::LootContents{loot, 0, {{0, kAsh, 1}, {1, kDagger2, 1}}});
    CHECK(storedUnits(server.accounts().itemsOf(id), kAsh) == 20);
    CHECK(storedUnits(server.accounts().itemsOf(id), kDagger2) == 0);
    // Nothing fits at all now: no write, BagFull at once, everything stays.
    const auto again = take(c, 6, loot, 1);
    CHECK(again.res.result == LR::BagFull);
    CHECK(again.updates.empty());
    CHECK(again.coins.empty());
    // A take of the coins that are gone: AlreadyTaken from the world.
    CHECK(take(c, 7, loot, vn::kLootTakeCoins).res.result == LR::AlreadyTaken);
}

TEST_CASE("Loot server: a bag move onto a stack of the same material merges", "[server][loot][items]")
{
    TestServer server({}, withLoot());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("loot_merge") == vn::LoginResult::Ok);
    const auto id = create(c, "Birleştiren");
    REQUIRE(server.accounts().giveItem(id, kAsh, 15)); // bag 0
    REQUIRE(server.accounts().giveItem(id, kAsh, 8));  // bag 1
    REQUIRE(server.accounts().giveItem(id, kAsh, 2));  // bag 2 (the kit's axe goes to bag 3)
    c.enter(id);
    vn::ItemUpdate update;
    REQUIRE(c.moveItem(1, kBag, 1, kBag, 0, &update) == vn::ItemMoveResult::Ok); // 8 onto 15: 20 + 3
    REQUIRE(update.items.size() == 2);
    CHECK(update.items[0].slot == 0);
    CHECK(update.items[0].quantity == 20);
    CHECK(update.items[1].slot == 1);
    CHECK(update.items[1].quantity == 3);
    REQUIRE(c.moveItem(2, kBag, 1, kBag, 0, &update) == vn::ItemMoveResult::Ok); // full target: a swap
    CHECK(update.items[0].quantity == 3);
    CHECK(update.items[1].quantity == 20);
    REQUIRE(c.moveItem(3, kBag, 2, kBag, 0, &update) == vn::ItemMoveResult::Ok); // 2 onto 3: complete
    CHECK(update.items[0].quantity == 5);
    CHECK(update.items[1].itemId == 0); // the source row is gone
    // Materials refuse equipment slots.
    CHECK(c.moveItem(4, kBag, 0, kEquip, 0) == vn::ItemMoveResult::WrongSlot);
    const auto items = server.accounts().itemsOf(id);
    CHECK(storedUnits(items, kAsh) == 25);
    std::size_t rows = 0;
    for (const auto& i : items)
        rows += i.templateId == kAsh ? 1 : 0;
    CHECK(rows == 2);
}

TEST_CASE("Loot server: disconnecting while the write runs commits it and the container goes", "[server][loot]")
{
    TestServer server({}, withLoot());
    TestClient watcher;
    REQUIRE(watcher.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    watcher.enterNew("loot_watch", "Gözcü");
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("loot_dc") == vn::LoginResult::Ok);
    const auto id = create(c, "Kopan");
    c.enter(id);
    std::uint32_t seq = 0;
    const auto loot = killThrall(c, seq, thrallIds(c).east, 104.0f, 100.0f);
    REQUIRE(watcher.waitFor<vn::EntitySpawn>([&](const vn::EntitySpawn& s) { return s.entityId == loot; }));

    server.accounts().setItemWriteDelayForTesting(600ms);
    c.send(vn::LootTakeReq{1, loot, vn::kLootTakeAll});
    std::this_thread::sleep_for(200ms); // the world reserved it and the session started writing
    c.close();
    // The write commits; the reservation is settled as taken; the empty container despawns.
    CHECK(watcher.waitFor<vn::EntityDespawn>([&](const vn::EntityDespawn& d) { return d.entityId == loot; }, 5s));
    server.accounts().setItemWriteDelayForTesting(0ms);
    const auto items = server.accounts().itemsOf(id);
    CHECK(storedUnits(items, kAsh) == 3);
    CHECK(storedUnits(items, kDagger2) == 1);
    CHECK(server.accounts().characterInfo(id)->coins == 11);
}

TEST_CASE("Loot server: disconnecting before the write started returns everything to the container",
          "[server][loot]")
{
    TestServer server({}, withLoot());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("loot_early") == vn::LoginResult::Ok);
    const auto id = create(c, "Erkenci");
    REQUIRE(server.accounts().giveItem(id, kAsh, 2)); // bag 0
    REQUIRE(server.accounts().giveItem(id, kAsh, 2)); // bag 1
    c.enter(id);
    std::uint32_t seq = 0;
    const auto loot = killThrall(c, seq, thrallIds(c).east, 104.0f, 100.0f);

    // A slow merge occupies the session's queue; the take waits behind it and never starts.
    server.accounts().setItemWriteDelayForTesting(800ms);
    c.send(vn::ItemMoveReq{1, kBag, 1, kBag, 0});
    c.send(vn::LootTakeReq{2, loot, vn::kLootTakeAll});
    std::this_thread::sleep_for(300ms);
    c.close();
    std::this_thread::sleep_for(1200ms);
    server.accounts().setItemWriteDelayForTesting(0ms);
    const auto items = server.accounts().itemsOf(id);
    CHECK(storedUnits(items, kAsh) == 4); // the merge committed, nothing from the loot
    CHECK(storedUnits(items, kDagger2) == 0);
    CHECK(server.accounts().characterInfo(id)->coins == 0);

    // Back in the world: the container still holds everything (it is the character's for 30 s).
    TestClient again;
    REQUIRE(again.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(again.login("loot_early") == vn::LoginResult::Ok);
    again.enter(id);
    const auto spawn =
        again.waitFor<vn::EntitySpawn>([&](const vn::EntitySpawn& s) { return s.entityId == loot; });
    REQUIRE(spawn);
    CHECK(spawn->state == vn::kEntityStateLootMine); // ownership follows the character
    std::uint32_t seq2 = 0;
    std::this_thread::sleep_for(1100ms);
    again.send(vn::MoveReq{++seq2, vn::kMoveFlagMoving | vn::kMoveFlagRunning, 102.5f, 0.0f, 100.0f, 0.0f});
    again.send(vn::MoveReq{++seq2, 0, 102.5f, 0.0f, 100.0f, 0.0f});
    again.send(vn::LootOpenReq{loot});
    REQUIRE(again.waitFor<vn::LootOpenRes>([](const vn::LootOpenRes& r) { return r.result == LR::Ok; }));
    const auto contents = again.expectSkippingWorld<vn::LootContents>();
    CHECK(contents == vn::LootContents{loot, 11, {{0, kAsh, 3}, {1, kDagger2, 1}}});
}

TEST_CASE("Loot server: flood cap, malformed requests, requests outside the world", "[server][loot][flood]")
{
    TestServer server({}, withLoot());
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("loot_flood", "Selci");
        for (int i = 0; i < 11; ++i)
            c.send(vn::LootOpenReq{12345});
        const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; });
        REQUIRE(err);
        CHECK(err->errorCode == vn::ErrorCode::Flood);
        CHECK(c.waitClosed());
    }
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("loot_ok", "Ölçülü");
        for (int i = 0; i < 10; ++i)
            c.send(vn::LootOpenReq{12345});
        for (int i = 0; i < 10; ++i)
        {
            const auto r = c.waitFor<vn::LootOpenRes>([](const vn::LootOpenRes&) { return true; });
            REQUIRE(r);
            CHECK(*r == vn::LootOpenRes{12345, LR::NotFound});
        }
        std::this_thread::sleep_for(1100ms); // a fresh flood window
        c.send(vn::LootCloseReq{12345});     // no answer
        c.sendRawPayload(vn::Opcode::LootTakeReq, {1, 2, 3});
        const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; });
        REQUIRE(err);
        CHECK(err->errorCode == vn::ErrorCode::ProtocolViolation);
    }
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("loot_state") == vn::LoginResult::Ok);
        c.send(vn::LootOpenReq{1});
        CHECK(c.expectError() == vn::ErrorCode::BadState);
    }
}
