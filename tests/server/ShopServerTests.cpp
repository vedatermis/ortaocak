// Merchants and consumables end to end on an in-process server (MemoryAccountService, test terrain,
// real sockets and the real 20 Hz world thread): what the session adds to the world's checks
// (docs/PROTOCOL_SHOP.md §2-§7) - selling and buying against the committed bag and balance in the
// item queue, the order ITEM_UPDATE, COINS, SHOP_*_RES after the write, failed writes changing
// nothing, consumables consumed before the restore, item slots on the bar stored and kept across a
// re-entry, the flood caps, out-of-state and malformed requests.

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Npcs.h"
#include "shared/game/Shop.h"
#include "shared/game/Skills.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace vn = vo::net;
namespace vg = vo::game;
namespace vw = vo::server::world;
using vo::tests::TestClient;
using vo::tests::TestServer;
using namespace std::chrono_literals;
using SR = vn::ShopResult;
using UR = vn::ItemUseResult;

namespace {

// A potion seller 4.2 m east of spawn_aydor (100, 100): in talk range of a player at the spawn.
std::shared_ptr<const vg::NpcCatalog> testNpcs()
{
    auto c = std::make_shared<vg::NpcCatalog>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseNpcCatalog(R"([[npc]]
id = 1
name = "Sürmeli Bacı"
title = "Şerbetçi"
greeting = "Hoş geldin"
model = "npc_potion_test"
zone = 1
x = 104.2
z = 100.0
yaw = -1.57
kind = "merchant"
stock = [16, 18, 1]
)",
                                "npcs.toml", vo::tests::testItemCatalog().get(), *c, error));
    return c;
}

TestServer::Options shopOptions(std::uint32_t vitalsPercent = 10)
{
    TestServer::Options o;
    o.npcs = testNpcs();
    o.world.zone.spawnSpread = 0.0f; // the player enters exactly at the spawn
    o.world.zone.testingEnterVitalsPercent = vitalsPercent;
    return o;
}

std::uint32_t npcOf(TestClient& c)
{
    const auto s = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& e) { return e.entityType == vn::kEntityTypeNpc; });
    REQUIRE(s);
    CHECK(s->templateId == 1);
    CHECK(s->name == "Sürmeli Bacı");
    return s->entityId;
}

struct Answer
{
    std::vector<vn::ItemUpdate> updates;
    std::vector<vn::Coins> coins;
    std::vector<vn::ItemCooldown> cooldowns;
};

// Waits for the answer T with `seq`, collecting ITEM_UPDATE / COINS / ITEM_COOLDOWN before it.
template <class T>
T answer(TestClient& c, std::uint32_t seq, Answer& a)
{
    for (;;)
    {
        auto m = c.next();
        REQUIRE(m);
        if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::ItemUpdate))
        {
            vn::ItemUpdate u;
            REQUIRE(vn::decodeMessage(m->payload, u));
            a.updates.push_back(u);
        }
        else if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::Coins))
        {
            vn::Coins x;
            REQUIRE(vn::decodeMessage(m->payload, x));
            a.coins.push_back(x);
        }
        else if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::ItemCooldown))
        {
            vn::ItemCooldown x;
            REQUIRE(vn::decodeMessage(m->payload, x));
            a.cooldowns.push_back(x);
        }
        else if (m->opcode == static_cast<std::uint16_t>(T::kOpcode))
        {
            T out;
            REQUIRE(vn::decodeMessage(m->payload, out));
            if (out.seq == seq)
                return out;
        }
    }
}

vn::ShopOpenRes open(TestClient& c, std::uint32_t npc)
{
    c.send(vn::ShopOpenReq{npc});
    const auto r = c.waitFor<vn::ShopOpenRes>([&](const vn::ShopOpenRes& x) { return x.npcId == npc; });
    REQUIRE(r);
    return *r;
}

vn::ShopBuyRes buy(TestClient& c, std::uint32_t seq, std::uint32_t npc, std::uint16_t tmpl, std::uint16_t qty, Answer& a)
{
    c.send(vn::ShopBuyReq{seq, npc, tmpl, qty});
    return answer<vn::ShopBuyRes>(c, seq, a);
}

vn::ShopSellRes sell(TestClient& c, std::uint32_t seq, std::uint32_t npc, std::uint8_t slot, std::uint16_t qty, Answer& a)
{
    c.send(vn::ShopSellReq{seq, npc, slot, qty});
    return answer<vn::ShopSellRes>(c, seq, a);
}

vn::ItemUseRes use(TestClient& c, std::uint32_t seq, std::uint16_t tmpl, std::uint8_t slot, Answer& a)
{
    c.send(vn::ItemUseReq{seq, tmpl, slot});
    return answer<vn::ItemUseRes>(c, seq, a);
}

} // namespace

TEST_CASE("Shop server: sell, buy and drink against the committed bag and balance", "[server][shop]")
{
    TestServer server({}, shopOptions());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto enter = c.enterNew("sh_flow", "Pazarcı"); // a warrior: the axe in bag 0
    const auto charId = c.lastCharId();
    CHECK(enter.hp == enter.maxHp / 10); // the testing hook: 9 of 90
    CHECK(c.lastCoins().coins == 0);
    const std::uint32_t npc = npcOf(c);
    const auto stock = open(c, npc);
    REQUIRE(stock.result == SR::Ok);
    CHECK(stock.entries == std::vector<vn::ShopEntry>{{16, 8}, {18, 8}, {1, 12}});

    // Nothing to pay with yet.
    Answer a;
    auto b = buy(c, 1, npc, 16, 1, a);
    CHECK(b.result == SR::NotEnoughCoins);
    CHECK(a.updates.empty());
    CHECK(a.coins.empty());

    // Sell the axe (value 20): ITEM_UPDATE (slot emptied), COINS(10, +10), SHOP_SELL_RES(Ok).
    a = {};
    auto s = sell(c, 2, npc, 0, 1, a);
    REQUIRE(s.result == SR::Ok);
    CHECK(s.templateId == 6);
    CHECK(s.gained == 10);
    REQUIRE(a.updates.size() == 1);
    CHECK(a.updates[0].items == std::vector<vn::ItemEntry>{{1, 0, 0, 0, 0}});
    REQUIRE(a.coins.size() == 1);
    CHECK(a.coins[0] == vn::Coins{10, 10});
    CHECK(server.accounts().characterInfo(charId)->coins == 10); // stored before the answer
    a = {};
    CHECK(sell(c, 3, npc, 0, 1, a).result == SR::NoItem); // the slot is empty now
    CHECK(sell(c, 4, npc, 1, 0, a).result == SR::BadQuantity);

    // Buy a potion: ITEM_UPDATE (a new stack in bag 0), COINS(2, 0), SHOP_BUY_RES(Ok, 8).
    a = {};
    b = buy(c, 5, npc, 16, 1, a);
    REQUIRE(b.result == SR::Ok);
    CHECK(b.cost == 8);
    REQUIRE(a.updates.size() == 1);
    REQUIRE(a.updates[0].items.size() == 1);
    CHECK(a.updates[0].items[0].slot == 0);
    CHECK(a.updates[0].items[0].templateId == 16);
    CHECK(a.updates[0].items[0].quantity == 1);
    REQUIRE(a.coins.size() == 1);
    CHECK(a.coins[0] == vn::Coins{2, 0});
    a = {};
    CHECK(buy(c, 6, npc, 18, 1, a).result == SR::NotEnoughCoins);
    CHECK(buy(c, 7, npc, 17, 1, a).result == SR::NotInStock);
    CHECK(buy(c, 8, npc, 16, 21, a).result == SR::BadQuantity);
    CHECK(a.updates.empty());

    // A failed write: nothing changes, ServerError.
    server.accounts().setFailItemMovesForTesting(true);
    a = {};
    CHECK(sell(c, 9, npc, 0, 1, a).result == SR::ServerError);
    CHECK(a.updates.empty());
    server.accounts().setFailItemMovesForTesting(false);

    // Drink it: ITEM_COOLDOWN, ITEM_UPDATE (gone), ITEM_USE_RES(Ok), then ITEM_USED and SELF_STATS.
    a = {};
    CHECK(use(c, 10, 18, vn::kItemUseAnyStack, a).result == UR::NoItem);
    CHECK(use(c, 11, 1, vn::kItemUseAnyStack, a).result == UR::NotUsable);
    CHECK(a.cooldowns.empty());
    auto u = use(c, 12, 16, 0, a);
    REQUIRE(u.result == UR::Ok);
    REQUIRE(a.cooldowns.size() == 1);
    CHECK(a.cooldowns[0] == vn::ItemCooldown{vg::kConsumableCooldownMs, vg::kConsumableCooldownMs});
    REQUIRE(a.updates.size() == 1);
    CHECK(a.updates[0].items == std::vector<vn::ItemEntry>{{1, 0, 0, 0, 0}});
    const auto used = c.waitFor<vn::ItemUsed>([](const vn::ItemUsed&) { return true; });
    REQUIRE(used);
    CHECK(used->entityId == enter.entityId);
    CHECK(used->templateId == 16);
    CHECK(used->hpRestored == 70);
    const auto self = c.waitFor<vn::SelfStats>([&](const vn::SelfStats& x) { return x.hp >= enter.hp + 70; });
    REQUIRE(self);
    a = {};
    CHECK(use(c, 13, 16, vn::kItemUseAnyStack, a).result == UR::NoItem); // none left
    CHECK(server.accounts().characterInfo(charId)->coins == 2);

    // The bar: a potion slot is stored (not owning one is fine); a material is not a consumable.
    c.send(vn::SkillBarSetReq{14, {1, vg::barItemSlot(16), 0, 0, 0, 0, 0, 0}});
    auto bar = c.waitFor<vn::SkillBarSetRes>([](const vn::SkillBarSetRes& r) { return r.seq == 14; });
    REQUIRE(bar);
    CHECK(bar->result == vn::SkillBarResult::Ok);
    c.send(vn::SkillBarSetReq{15, {1, vg::barItemSlot(9), 0, 0, 0, 0, 0, 0}});
    bar = c.waitFor<vn::SkillBarSetRes>([](const vn::SkillBarSetRes& r) { return r.seq == 15; });
    REQUIRE(bar);
    CHECK(bar->result == vn::SkillBarResult::NotLearned);
    CHECK(bar->slots == vn::SkillBarSlots{1, vg::barItemSlot(16), 0, 0, 0, 0, 0, 0});

    // Re-entry within the cooldown: the bar, the balance and the empty bag slot as stored, and the
    // running cooldown right after CHAR_STATS.
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    c.enter(charId);
    CHECK(c.lastSkillBar().slots == vn::SkillBarSlots{1, vg::barItemSlot(16), 0, 0, 0, 0, 0, 0});
    CHECK(c.lastCoins().coins == 2);
    for (const auto& e : c.lastInventory().items)
        CHECK(e.templateId != 16);
    const auto cd = c.waitFor<vn::ItemCooldown>([](const vn::ItemCooldown&) { return true; });
    REQUIRE(cd);
    CHECK(cd->remainingMs < vg::kConsumableCooldownMs);
}

TEST_CASE("Shop server: flood caps, out-of-state and malformed requests", "[server][shop]")
{
    TestServer server({}, shopOptions(0));
    {
        // In character select: ERROR(BadState).
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("sh_state") == vn::LoginResult::Ok);
        c.send(vn::ShopOpenReq{1});
        CHECK(c.expectError() == vn::ErrorCode::BadState);
    }
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("sh_bad", "Bozukçu");
        c.sendRawPayload(vn::Opcode::ShopBuyReq, {1, 2, 3});
        CHECK(c.waitFor<vn::Error>([](const vn::Error& e) { return e.errorCode == vn::ErrorCode::ProtocolViolation; }));
    }
    {
        // 10 shop requests per second are answered, the 11th closes the connection.
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("sh_flood", "Selci");
        for (int i = 0; i < 11; ++i)
            c.send(vn::ShopCloseReq{1});
        CHECK(c.waitFor<vn::Error>([](const vn::Error& e) { return e.errorCode == vn::ErrorCode::Flood; }));
        CHECK(c.waitClosed());
    }
    {
        // ITEM_USE_REQ has its own cap of 10 per second (each answered NoItem here).
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("sh_useflood", "Içici");
        for (int i = 0; i < 10; ++i)
            c.send(vn::ItemUseReq{static_cast<std::uint32_t>(i + 1), 16, vn::kItemUseAnyStack});
        for (int i = 0; i < 10; ++i)
            CHECK(c.waitFor<vn::ItemUseRes>([](const vn::ItemUseRes& r) { return r.result == UR::NoItem; }));
        c.send(vn::ItemUseReq{99, 16, vn::kItemUseAnyStack});
        CHECK(c.waitFor<vn::Error>([](const vn::Error& e) { return e.errorCode == vn::ErrorCode::Flood; }));
    }
}
