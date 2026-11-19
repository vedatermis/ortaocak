// Item upgrades end to end on an in-process server (MemoryAccountService, test terrain, real sockets
// and the real world thread; docs/PROTOCOL_UPGRADE.md): the world's checks (an open smith) and the
// session's (the committed bag and balance) in order, the order ITEM_UPDATE, COINS, UPGRADE_RES after the
// write, the stone / fee / item change stored together, a failed write changing nothing, the forced
// outcomes of world.testing_upgrade_outcome (success, failure that keeps the item, destruction), the
// level kept across a relog and counted in the stats, and the flood cap.

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Combat.h"
#include "shared/game/Npcs.h"
#include "shared/game/Upgrade.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>

namespace vn = vo::net;
namespace vg = vo::game;
namespace vw = vo::server::world;
namespace vsn = vo::server::net;
using vo::tests::TestClient;
using vo::tests::TestServer;
using namespace std::chrono_literals;
using UR = vn::UpgradeResult;

namespace {

constexpr vg::ItemTemplateId kHam = 166, kSaf = 167, kAsh = 9, kAxe = 6, kSword = 1;

// A smith 4.2 m east of spawn_aydor (100, 100) that upgrades, and a potion seller 4.2 m north that
// does not: both in talk range of a player at the spawn.
std::shared_ptr<const vg::NpcCatalog> testNpcs()
{
    auto c = std::make_shared<vg::NpcCatalog>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseNpcCatalog(R"([[npc]]
id = 1
name = "Örsbaşı"
title = "Demirci"
greeting = "Buyur"
model = "npc_smith_test"
zone = 1
x = 104.2
z = 100.0
yaw = -1.57
kind = "merchant"
stock = [166, 167, 168, 1]
upgrades = true

[[npc]]
id = 2
name = "Sürmeli Bacı"
title = "Şerbetçi"
greeting = "Hoş geldin"
model = "npc_potion_test"
zone = 1
x = 100.0
z = 104.2
yaw = 3.1
kind = "merchant"
stock = [16]
)",
                                "npcs.toml", vo::tests::testItemCatalog().get(), *c, error));
    return c;
}

TestServer::Options options()
{
    TestServer::Options o;
    o.npcs = testNpcs();
    o.world.zone.spawnSpread = 0.0f;
    return o;
}

// world.testing_upgrade_outcome: attempts at targets from ailFrom on fail, below succeed.
vsn::ServerSettings settings(std::uint8_t failFrom)
{
    vsn::ServerSettings s;
    s.testingUpgradeFailFrom = failFrom;
    return s;
}

struct Npcs
{
    std::uint32_t smith = 0;
    std::uint32_t potion = 0;
};

Npcs npcsOf(TestClient& c)
{
    Npcs n;
    for (int i = 0; i < 2; ++i)
    {
        const auto s = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn& e) { return e.entityType == vn::kEntityTypeNpc; });
        REQUIRE(s);
        (s->templateId == 1 ? n.smith : n.potion) = s->entityId;
    }
    REQUIRE(n.smith != 0);
    REQUIRE(n.potion != 0);
    return n;
}

struct Answer
{
    std::vector<vn::ItemUpdate> updates;
    std::vector<vn::Coins> coins;
};

// One attempt; paced at 4 a second (the flood cap, PROTOCOL_UPGRADE.md §5).
vn::UpgradeRes upgrade(TestClient& c, const vn::UpgradeReq& req, Answer& a)
{
    c.pumpFor(260ms);
    c.send(req);
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
        else if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::UpgradeRes))
        {
            vn::UpgradeRes out;
            REQUIRE(vn::decodeMessage(m->payload, out));
            if (out.seq == req.seq)
                return out;
        }
    }
}

void openShop(TestClient& c, std::uint32_t npc)
{
    c.send(vn::ShopOpenReq{npc});
    const auto r = c.waitFor<vn::ShopOpenRes>([&](const vn::ShopOpenRes& x) { return x.npcId == npc; });
    REQUIRE(r);
    REQUIRE(r->result == vn::ShopResult::Ok);
}

const vg::StoredItem* stored(const std::vector<vg::StoredItem>& items, vg::ItemInstanceId id)
{
    const auto it = std::find_if(items.begin(), items.end(), [&](const vg::StoredItem& s) { return s.id == id; });
    return it == items.end() ? nullptr : &*it;
}

std::uint64_t idAt(const vn::Inventory& inv, std::uint8_t container, std::uint8_t slot)
{
    for (const auto& e : inv.items)
        if (e.container == container && e.slot == slot)
            return e.itemId;
    return 0;
}

void leave(TestClient& c)
{
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
}

} // namespace

TEST_CASE("Upgrade server: refusals in order, a success stored before the answer, kept across a relog",
          "[server][upgrade]")
{
    TestServer server(settings(11), options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("up_flow", "Tavcı"); // a warrior: the axe in bag 0
    const auto charId = c.lastCharId();
    leave(c);
    // Seeded like vo_admin: Akçe, three Ham stones (bag 1), a Saf stone (bag 2), Kül Tozu (bag 3).
    auto& acc = server.accounts();
    REQUIRE(acc.setCoins(charId, 1000));
    REQUIRE(acc.giveItem(charId, kHam, 3));
    REQUIRE(acc.giveItem(charId, kSaf, 1));
    REQUIRE(acc.giveItem(charId, kAsh, 5));
    c.enter(charId);
    const Npcs npc = npcsOf(c);
    const std::uint64_t axe = idAt(c.lastInventory(), 1, 0);
    REQUIRE(axe != 0);
    for (const auto& e : c.lastInventory().items)
        CHECK(e.upgrade == 0);

    Answer a;
    std::uint32_t seq = 0;
    const auto req = [&](std::uint32_t npcId, std::uint8_t slot, std::uint64_t id, std::uint8_t from,
                         std::uint8_t stone = vn::kUpgradeAnyStone) {
        return vn::UpgradeReq{++seq, npcId, slot, id, from, stone};
    };
    // World checks: no shop open; a merchant that does not upgrade.
    CHECK(upgrade(c, req(npc.smith, 0, axe, 0), a).result == UR::NotOpen);
    openShop(c, npc.potion);
    CHECK(upgrade(c, req(npc.potion, 0, axe, 0), a).result == UR::NotSmith);
    CHECK(upgrade(c, req(npc.smith, 0, axe, 0), a).result == UR::NotOpen); // another shop is open
    openShop(c, npc.smith);
    // Session checks against the committed bag and balance.
    CHECK(upgrade(c, req(npc.smith, 7, axe, 0), a).result == UR::NoItem);
    CHECK(upgrade(c, req(npc.smith, 0, axe + 1000, 0), a).result == UR::NoItem);
    CHECK(upgrade(c, req(npc.smith, 0, axe, 2), a).result == UR::Changed);
    const auto ash = upgrade(c, req(npc.smith, 3, idAt(c.lastInventory(), 1, 3), 0), a);
    CHECK(ash.result == UR::NotUpgradable);
    CHECK(ash.templateId == kAsh);
    CHECK(upgrade(c, req(npc.smith, 0, axe, 0, 2), a).result == UR::WrongStone);
    CHECK(upgrade(c, req(npc.smith, 0, axe, 0, 9), a).result == UR::NoStone);
    CHECK(a.updates.empty());
    CHECK(a.coins.empty());
    CHECK(acc.characterInfo(charId)->coins == 1000);

    // A failed write: nothing changes (stone, fee and level stay), ServerError.
    acc.setFailItemMovesForTesting(true);
    CHECK(upgrade(c, req(npc.smith, 0, axe, 0), a).result == UR::ServerError);
    acc.setFailItemMovesForTesting(false);
    CHECK(a.updates.empty());
    CHECK(stored(acc.itemsOf(charId), axe)->upgrade == 0);
    CHECK(acc.characterInfo(charId)->coins == 1000);

    // The attempt (forced success): ITEM_UPDATE (the axe +1, the stones 3 -> 2), COINS, UPGRADE_RES.
    a = {};
    const auto ok = upgrade(c, req(npc.smith, 0, axe, 0), a);
    REQUIRE(ok.result == UR::Success);
    CHECK(ok.level == 1);
    CHECK(ok.templateId == kAxe);
    CHECK(ok.stoneTemplateId == kHam);
    CHECK(ok.fee == vg::upgradeFeeForTier(1, 1));
    REQUIRE(a.updates.size() == 1);
    REQUIRE(a.updates[0].items.size() == 2);
    CHECK(a.updates[0].items[0] == vn::ItemEntry{1, 0, axe, kAxe, 1, 1});
    CHECK(a.updates[0].items[1].slot == 1);
    CHECK(a.updates[0].items[1].quantity == 2);
    REQUIRE(a.coins.size() == 1);
    CHECK(a.coins[0] == vn::Coins{1000 - ok.fee, 0});
    // Stored before the answer.
    CHECK(stored(acc.itemsOf(charId), axe)->upgrade == 1);
    CHECK(acc.characterInfo(charId)->coins == 1000 - ok.fee);
    // A second one with the stone named; the client's old level is refused now.
    CHECK(upgrade(c, req(npc.smith, 0, axe, 0), a).result == UR::Changed);
    a = {};
    const auto ok2 = upgrade(c, req(npc.smith, 0, axe, 1, 1), a);
    REQUIRE(ok2.result == UR::Success);
    CHECK(ok2.level == 2);
    CHECK(acc.characterInfo(charId)->coins == 1000 - ok.fee - ok2.fee);

    // A relog: the level comes back in INVENTORY.
    leave(c);
    c.enter(charId);
    bool seen = false;
    for (const auto& e : c.lastInventory().items)
        if (e.itemId == axe)
        {
            seen = true;
            CHECK(e.upgrade == 2);
        }
    CHECK(seen);
    CHECK(c.lastCoins().coins == 1000 - ok.fee - ok2.fee);
}

TEST_CASE("Upgrade server: a forced failure keeps the item at +4, destroys it from +6; max level; stats",
          "[server][upgrade]")
{
    TestServer server(settings(4), options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto first = c.enterNew("up_fail", "Kırıcı");
    const auto charId = c.lastCharId();
    leave(c);
    auto& acc = server.accounts();
    const auto items = acc.itemsOf(charId);
    vg::ItemInstanceId axe = 0, sword = 0;
    for (const auto& s : items)
    {
        if (s.templateId == kAxe)
            axe = s.id;
        if (s.templateId == kSword)
            sword = s.id; // equipped (main hand)
    }
    REQUIRE(axe != 0);
    REQUIRE(sword != 0);
    REQUIRE(acc.setCoins(charId, 5000));
    REQUIRE(acc.giveItem(charId, kHam, 2)); // bag 1
    const auto axe2 = acc.giveItem(charId, kAxe); // bag 2
    const auto axe10 = acc.giveItem(charId, kAxe); // bag 3
    REQUIRE(axe2);
    REQUIRE(axe10);
    REQUIRE(acc.setItemUpgrade(charId, axe, 3));
    REQUIRE(acc.setItemUpgrade(charId, *axe2, 5));
    REQUIRE(acc.setItemUpgrade(charId, *axe10, 10));
    REQUIRE(acc.setItemUpgrade(charId, sword, 7)); // the equipped sword counts in the stats
    CHECK_FALSE(acc.setItemUpgrade(charId, axe, 11));
    const auto enter = c.enter(charId);
    // ENTER_WORLD_RES: the +7 sword raises the attack by its bonus (attributes unchanged).
    const vg::ItemTemplate& t = *vo::tests::testItemCatalog()->find(kSword);
    const auto plusSeven = vg::deriveStats(1, 1, vg::EquipBonus{t.attack + vg::upgradeBonus(t.attack, 7), 4}).attack -
                           vg::deriveStats(1, 1, vg::EquipBonus{t.attack, 4}).attack;
    CHECK(plusSeven > 0);
    CHECK(enter.attack == first.attack + plusSeven);
    CHECK(enter.defense == first.defense);
    const Npcs npc = npcsOf(c);
    openShop(c, npc.smith);

    std::uint32_t seq = 0;
    Answer a;
    // +3 -> +4 fails: the axe stays +3; the stone and the fee are gone (one ITEM_UPDATE: the stone only).
    const auto kept = upgrade(c, vn::UpgradeReq{++seq, npc.smith, 0, axe, 3, vn::kUpgradeAnyStone}, a);
    REQUIRE(kept.result == UR::Failed);
    CHECK(kept.level == 3);
    CHECK(kept.fee == vg::upgradeFeeForTier(1, 4));
    REQUIRE(a.updates.size() == 1);
    REQUIRE(a.updates[0].items.size() == 1);
    CHECK(a.updates[0].items[0].slot == 1);
    CHECK(a.updates[0].items[0].quantity == 1);
    CHECK(stored(acc.itemsOf(charId), axe)->upgrade == 3);
    CHECK(acc.characterInfo(charId)->coins == 5000 - kept.fee);
    // +5 -> +6 fails: the axe is destroyed (its slot empties; the last stone too).
    a = {};
    const auto gone = upgrade(c, vn::UpgradeReq{++seq, npc.smith, 2, *axe2, 5, vn::kUpgradeAnyStone}, a);
    REQUIRE(gone.result == UR::Destroyed);
    CHECK(gone.level == 5);
    REQUIRE(a.updates.size() == 1);
    REQUIRE(a.updates[0].items.size() == 2);
    CHECK(a.updates[0].items[0] == vn::ItemEntry{1, 2, 0, 0, 0, 0});
    CHECK(a.updates[0].items[1] == vn::ItemEntry{1, 1, 0, 0, 0, 0});
    CHECK(stored(acc.itemsOf(charId), *axe2) == nullptr);
    CHECK(acc.characterInfo(charId)->coins == 5000 - kept.fee - gone.fee);
    // +10: MaxLevel before the (now missing) stone.
    a = {};
    CHECK(upgrade(c, vn::UpgradeReq{++seq, npc.smith, 3, *axe10, 10, vn::kUpgradeAnyStone}, a).result == UR::MaxLevel);
    CHECK(upgrade(c, vn::UpgradeReq{++seq, npc.smith, 0, axe, 3, vn::kUpgradeAnyStone}, a).result == UR::NoStone);
    CHECK(a.updates.empty());
}

TEST_CASE("Upgrade server: flood cap and out-of-state requests", "[server][upgrade]")
{
    TestServer server({}, options());
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("up_state") == vn::LoginResult::Ok);
        c.send(vn::UpgradeReq{1, 1, 0, 1, 0, vn::kUpgradeAnyStone});
        CHECK(c.expectError() == vn::ErrorCode::BadState);
    }
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("up_bad", "Bozuk");
        c.sendRawPayload(vn::Opcode::UpgradeReq, {1, 2, 3});
        CHECK(c.waitFor<vn::Error>([](const vn::Error& e) { return e.errorCode == vn::ErrorCode::ProtocolViolation; }));
    }
    {
        // 4 UPGRADE_REQ per second are answered (NotOpen here), the 5th closes the connection.
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("up_flood", "Selci");
        for (std::uint32_t i = 1; i <= 4; ++i)
            c.send(vn::UpgradeReq{i, 1, 0, 1, 0, vn::kUpgradeAnyStone});
        for (int i = 0; i < 4; ++i)
            CHECK(c.waitFor<vn::UpgradeRes>([](const vn::UpgradeRes& r) { return r.result == UR::NotOpen; }));
        c.send(vn::UpgradeReq{5, 1, 0, 1, 0, vn::kUpgradeAnyStone});
        CHECK(c.waitFor<vn::Error>([](const vn::Error& e) { return e.errorCode == vn::ErrorCode::Flood; }));
        CHECK(c.waitClosed());
    }
}
