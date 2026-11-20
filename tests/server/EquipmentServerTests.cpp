// M18 "Kuşanma" end to end on an in-process server (MemoryAccountService, test terrain, real sockets and
// the 20 Hz world thread; docs/PROTOCOL_EQUIPMENT.md): CHAR_STATS follows the main-hand weapon (interval,
// reach, swing factor, damage range), the server swings at the weapon's interval and validates its reach,
// accessories go into their slots, add their bonus lines and stay after a relog, unmet attribute
// requirements are refused.

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Monsters.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vn = vo::net;
namespace vg = vo::game;
using vo::tests::TestClient;
using vo::tests::TestServer;
using namespace std::chrono_literals;

namespace {

// A training dummy (5000 hp, harmless, never aggressive) 4 m east of the Aydor spawn (100/100).
std::shared_ptr<const vg::MonsterData> dummy()
{
    const std::string toml = R"(
[[monster]]
id = 2
name = "Talim Kuklası"
model = "ash_thrall"
level = 1
hp = 5000
attack = 0
defense = 4
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 0.0
leash_radius = 40.0
xp = 1
respawn_ms = 10000

[[spawn]]
zone = 1
monster = 2
x = 104.0
z = 100.0
radius = 0.0
)";
    auto data = std::make_shared<vg::MonsterData>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseMonsterData(toml, "test.toml", *data, error));
    return data;
}

TestServer::Options options()
{
    TestServer::Options o;
    o.monsters = dummy();
    o.world.statsInterval = 0s;
    return o;
}

const vg::ItemTemplate& named(const std::string& name)
{
    const vg::ItemTemplate* found = nullptr;
    for (const auto& t : vo::tests::testItemCatalog()->templates())
        if (t.name == name)
            found = &t;
    INFO("item " << name);
    REQUIRE(found != nullptr);
    return *found;
}

std::uint32_t dummyId(TestClient& c)
{
    const auto s = c.waitFor<vn::EntitySpawn>(
        [](const vn::EntitySpawn& s) { return s.entityType == vn::kEntityTypeMonster && s.templateId == 2; });
    REQUIRE(s);
    return s->entityId;
}

void runTo(TestClient& c, std::uint32_t& seq, float x, float z)
{
    std::this_thread::sleep_for(1100ms);
    c.send(vn::MoveReq{++seq, vn::kMoveFlagMoving | vn::kMoveFlagRunning, x, 0.0f, z, 0.0f});
    c.send(vn::MoveReq{++seq, vn::kMoveFlagRunning, x, 0.0f, z, 0.0f});
}

// The server times of `n` own basic swings at `target` after ATTACK_REQ.
std::vector<std::uint32_t> swings(TestClient& c, std::uint32_t me, std::uint32_t target, int n)
{
    c.send(vn::AttackReq{target});
    std::vector<std::uint32_t> at;
    while (static_cast<int>(at.size()) < n)
    {
        const auto ev = c.waitFor<vn::CombatEvent>(
            [&](const vn::CombatEvent& e) { return e.sourceId == me && e.targetId == target && e.skillId == 0; }, 6s);
        REQUIRE(ev);
        at.push_back(ev->serverTimeMs);
    }
    c.send(vn::AttackStopReq{});
    return at;
}

std::uint8_t bagSlotOf(const vn::Inventory& inv, vg::ItemTemplateId t)
{
    std::uint8_t slot = 255;
    for (const auto& e : inv.items)
        if (e.container == 1 && e.templateId == t)
            slot = e.slot;
    INFO("template " << t);
    REQUIRE(slot != 255);
    return slot;
}

} // namespace

TEST_CASE("Equipment server: CHAR_STATS and the swing rate follow the main-hand weapon", "[server][equipment]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("eq_speed", "Hızlıca");
    // Er Kılıcı (5-7, 1600 ms, 2.5 m) and Meşe Kalkan: the class's numbers, a damage range.
    const vn::CharStats& kit = c.lastCharStats();
    CHECK(kit.attackIntervalMs == 1600);
    CHECK(kit.reachCm == 250);
    CHECK(kit.swingPermille == 1000);
    CHECK(kit.attackMin < kit.attack);
    CHECK(kit.attackMax > kit.attack);
    const std::uint32_t target = dummyId(c);
    std::uint32_t seq = 0;
    runTo(c, seq, 102.0f, 100.0f);

    const auto sword = swings(c, e.entityId, target, 4);
    for (std::size_t i = 1; i < sword.size(); ++i)
    {
        INFO("sword gap " << sword[i] - sword[i - 1]);
        CHECK(sword[i] - sword[i - 1] >= 1600);
        CHECK(sword[i] - sword[i - 1] <= 1700); // one 50 ms tick of slack (+ one more for scheduling)
    }

    // The two-handed Ocak Baltası (2200 ms, 2.7 m, x1.375 per swing): the shield goes to the bag.
    REQUIRE(c.moveItem(1, 1, 0, 0, 0) == vn::ItemMoveResult::Ok);
    const auto axe = c.waitFor<vn::CharStats>([](const vn::CharStats& s) { return s.attackIntervalMs == 2200; });
    REQUIRE(axe);
    CHECK(axe->reachCm == 270);
    CHECK(axe->swingPermille == 1375);
    CHECK(axe->attack == 24);
    const auto slow = swings(c, e.entityId, target, 4);
    for (std::size_t i = 1; i < slow.size(); ++i)
    {
        INFO("axe gap " << slow[i] - slow[i - 1]);
        CHECK(slow[i] - slow[i - 1] >= 2200);
        CHECK(slow[i] - slow[i - 1] <= 2300);
    }
}

TEST_CASE("Equipment server: the reach is the weapon's - a halberd hits from 3.5 m, a sword does not",
          "[server][equipment]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("eq_reach", "Uzanan");
    const auto charId = c.lastCharId();
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    REQUIRE(server.accounts().setLevel(charId, 5));
    const vg::ItemTemplate& halberd = named("Sağlam Teber");
    REQUIRE(server.accounts().giveItem(charId, halberd.id));
    const auto e = c.enter(charId);
    const std::uint32_t target = dummyId(c);
    std::uint32_t seq = 0;
    runTo(c, seq, 100.5f, 100.0f); // 3.5 m from the dummy: sword 2.5 + 0.5 tolerance is short
    c.send(vn::AttackReq{target});
    const auto fail = c.waitFor<vn::ActionFail>([](const vn::ActionFail& f) { return f.action == vn::CombatAction::Attack; });
    REQUIRE(fail);
    CHECK(fail->reason == vn::ActionFailReason::OutOfRange);
    c.send(vn::AttackStopReq{});

    REQUIRE(c.moveItem(2, 1, bagSlotOf(c.lastInventory(), halberd.id), 0, 0) == vn::ItemMoveResult::Ok);
    const auto stats = c.waitFor<vn::CharStats>([](const vn::CharStats& s) { return s.reachCm == 330; });
    REQUIRE(stats);
    CHECK(stats->attackIntervalMs == 2300);
    CHECK(stats->attackMin < stats->attackMax);
    const auto hit = swings(c, e.entityId, target, 1);
    CHECK(hit.size() == 1);
}

TEST_CASE("Equipment server: accessories go into their slots, add their bonus lines and stay after a relog",
          "[server][equipment]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("eq_acc", "Takılı");
    const auto charId = c.lastCharId();
    const std::uint32_t hpBefore = c.lastCharStats().maxHp;
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    REQUIRE(server.accounts().setLevel(charId, 3));
    const vg::ItemTemplate& ring = named("Eski Kaya Yüzüğü");
    const vg::ItemTemplate& necklace = named("Eski Kurt Dişi Kolye");
    const vg::ItemTemplate& earring = named("Eski Kıvılcım Küpesi");
    REQUIRE(server.accounts().giveItem(charId, ring.id));
    REQUIRE(server.accounts().giveItem(charId, ring.id));
    REQUIRE(server.accounts().giveItem(charId, necklace.id));
    REQUIRE(server.accounts().giveItem(charId, earring.id));
    c.enter(charId);
    const vn::Inventory inv = c.lastInventory();
    std::vector<std::uint8_t> rings;
    for (const auto& it : inv.items)
        if (it.container == 1 && it.templateId == ring.id)
            rings.push_back(it.slot);
    REQUIRE(rings.size() == 2);
    const auto slot = [](vg::EquipSlot s) { return static_cast<std::uint8_t>(s); };
    CHECK(c.moveItem(1, 1, rings[0], 0, slot(vg::EquipSlot::Ring1)) == vn::ItemMoveResult::Ok);
    CHECK(c.moveItem(2, 1, rings[1], 0, slot(vg::EquipSlot::Earring1)) == vn::ItemMoveResult::WrongSlot);
    CHECK(c.moveItem(3, 1, rings[1], 0, slot(vg::EquipSlot::Ring2)) == vn::ItemMoveResult::Ok);
    CHECK(c.moveItem(4, 1, bagSlotOf(inv, necklace.id), 0, slot(vg::EquipSlot::Necklace)) == vn::ItemMoveResult::Ok);
    CHECK(c.moveItem(5, 1, bagSlotOf(inv, earring.id), 0, slot(vg::EquipSlot::Earring2)) == vn::ItemMoveResult::Ok);
    // Slot 13 does not exist.
    CHECK(c.moveItem(6, 0, slot(vg::EquipSlot::Ring1), 0, 13) == vn::ItemMoveResult::BadSlot);
    const auto stats = c.waitFor<vn::CharStats>([&](const vn::CharStats& s) { return s.bonusCritPermille == earring.bonusCritPermille; });
    REQUIRE(stats);
    CHECK(stats->bonusHp == necklace.bonusHp);
    CHECK(stats->attributes[0].bonus == 2); // two rings of Güç +1
    CHECK(stats->maxHp > hpBefore);
    // The stored rows: equipment slots 7, 9, 10 and 12; after a relog INVENTORY lists them there.
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    std::set<std::uint8_t> stored;
    for (const auto& it : server.accounts().itemsOf(charId))
        if (it.place.isEquipment())
            stored.insert(it.place.slot);
    CHECK(stored.contains(7));
    CHECK(stored.contains(9));
    CHECK(stored.contains(10));
    CHECK(stored.contains(12));
    c.enter(charId);
    std::set<std::uint8_t> listed;
    for (const auto& it : c.lastInventory().items)
        if (it.container == 0)
            listed.insert(it.slot);
    CHECK(listed == stored);
    CHECK(c.lastCharStats().bonusHp == necklace.bonusHp);
    CHECK(c.lastCharStats().bonusCritPermille == earring.bonusCritPermille);
}

TEST_CASE("Equipment server: an unmet attribute requirement refuses the move", "[server][equipment]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("eq_req", "Yetersiz");
    const auto charId = c.lastCharId();
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    REQUIRE(server.accounts().setLevel(charId, 19));
    const vg::ItemTemplate& axe = named("Ant Baltası");
    REQUIRE(axe.requirements[0] > vg::classStartAttributes(1)[0]);
    REQUIRE(server.accounts().giveItem(charId, axe.id));
    c.enter(charId);
    CHECK(c.moveItem(1, 1, bagSlotOf(c.lastInventory(), axe.id), 0, 0) == vn::ItemMoveResult::AttributeTooLow);
    // Nothing changed: the sword is still in the main hand.
    bool sword = false;
    for (const auto& it : server.accounts().itemsOf(charId))
        sword = sword || (it.place.isEquipment() && it.place.slot == 0 && it.templateId == 1);
    CHECK(sword);
}
