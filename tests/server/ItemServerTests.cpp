// Items end to end on an in-process server (MemoryAccountService, test terrain): starter kit granted
// exactly once, INVENTORY ordering, visible equipment in CHAR_LIST / ENTITY_SPAWN / ENTITY_EQUIP
// (in view and not out of view), ITEM_MOVE_REQ results and their order, the 10/s flood cap,
// pipelined moves decided in order, storage failures and hidden (unknown-template) items.
// docs/PROTOCOL_ITEMS.md.

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Items.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vn = vo::net;
namespace vg = vo::game;
using vo::tests::TestClient;
using vo::tests::TestServer;
using R = vn::ItemMoveResult;
using namespace std::chrono_literals;

namespace {

constexpr std::uint8_t kEquip = 0;
constexpr std::uint8_t kBag = 1;
constexpr std::uint16_t kSword = 1, kShield = 2, kDagger = 3, kStaff = 4, kAxe = 6, kSword3 = 7;

std::uint32_t create(TestClient& c, const std::string& name, std::uint8_t race, std::uint8_t cls,
                     vn::CharSummary* summary = nullptr)
{
    c.send(vn::CharCreateReq{name, race, cls, 0, 0});
    const auto res = c.expect<vn::CharCreateRes>();
    REQUIRE(res.result == vn::CharCreateResult::Ok);
    if (summary)
        *summary = res.character;
    return res.character.charId;
}

std::optional<vn::ItemEntry> entryAt(const vn::Inventory& inv, std::uint8_t container, std::uint8_t slot)
{
    for (const auto& e : inv.items)
        if (e.container == container && e.slot == slot)
            return e;
    return std::nullopt;
}

std::uint16_t templateAt(const std::vector<vg::StoredItem>& items, std::uint8_t container, std::uint8_t slot)
{
    for (const auto& i : items)
        if (i.place.container == container && i.place.slot == slot)
            return i.templateId;
    return 0;
}

// Waits until the stored items match (the commit happens before ITEM_MOVE_RES, so this is
// immediate; the loop only guards against reading while a later request runs).
std::vector<vg::StoredItem> stored(TestServer& server, std::uint32_t charId)
{
    return server.accounts().itemsOf(charId);
}

} // namespace

TEST_CASE("Items: character list shows the starter kit before the first entry, then the real equipment",
          "[server][items]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("kit_list") == vn::LoginResult::Ok);
    vn::CharSummary warrior, mage, rogue;
    const auto warriorId = create(c, "Savaşan", 1, 1, &warrior);
    create(c, "Büyücü", 2, 3, &mage);
    create(c, "Haydut", 1, 2, &rogue);
    CHECK(warrior.mainHandTemplate == kSword);
    CHECK(warrior.offHandTemplate == kShield);
    CHECK(mage.mainHandTemplate == kStaff);
    CHECK(mage.offHandTemplate == 0);
    CHECK(rogue.mainHandTemplate == kDagger);
    CHECK(rogue.offHandTemplate == kDagger);
    CHECK(stored(server, warriorId).empty()); // nothing granted before the first entry

    c.send(vn::CharListReq{});
    auto list = c.expect<vn::CharListRes>();
    REQUIRE(list.characters.size() == 3);
    CHECK(list.characters[0].mainHandTemplate == kSword);
    CHECK(list.characters[0].offHandTemplate == kShield);
    CHECK(list.characters[1].mainHandTemplate == kStaff);

    // Enter, unequip the shield, leave: the list now shows the stored equipment.
    c.enter(warriorId);
    REQUIRE(c.moveItem(1, kEquip, 1, kBag, 10) == R::Ok);
    c.send(vn::LeaveWorldReq{});
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    c.send(vn::CharListReq{});
    list = c.expectSkippingWorld<vn::CharListRes>();
    CHECK(list.characters[0].mainHandTemplate == kSword);
    CHECK(list.characters[0].offHandTemplate == 0);
    CHECK(list.characters[1].mainHandTemplate == kStaff); // still the kit (never entered)
}

TEST_CASE("Items: the starter kit is granted exactly once and INVENTORY lists it", "[server][items]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("kit_once", "Birkez");
    const auto charId = c.lastCharId();
    const vn::Inventory first = c.lastInventory();
    CHECK(first.bagSize == 28);
    REQUIRE(first.items.size() == 3);
    // Equipment first, by slot.
    CHECK(first.items[0].container == kEquip);
    CHECK(first.items[0].slot == 0);
    CHECK(first.items[0].templateId == kSword);
    CHECK(first.items[1].container == kEquip);
    CHECK(first.items[1].slot == 1);
    CHECK(first.items[1].templateId == kShield);
    CHECK(first.items[2].container == kBag);
    CHECK(first.items[2].slot == 0);
    CHECK(first.items[2].templateId == kAxe);
    for (const auto& e : first.items)
    {
        CHECK(e.itemId != 0);
        CHECK(e.quantity == 1);
    }
    CHECK(stored(server, charId).size() == 3);

    // Leave and re-enter (and log in again from another connection): same items, nothing new.
    c.send(vn::LeaveWorldReq{});
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    c.enter(charId);
    CHECK(c.lastInventory() == first);
    c.close();
    TestClient again;
    REQUIRE(again.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(again.login("kit_once") == vn::LoginResult::Ok);
    again.enter(charId);
    CHECK(again.lastInventory() == first);
    CHECK(stored(server, charId).size() == 3);
}

TEST_CASE("Items: INVENTORY comes right after ENTER_WORLD_RES; spawns carry the visible equipment",
          "[server][items]")
{
    TestServer server({});
    TestClient a;
    REQUIRE(a.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto ea = a.enterNew("order_a", "Önceki");

    TestClient b;
    REQUIRE(b.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(b.login("order_b") == vn::LoginResult::Ok);
    const auto bId = create(b, "Sonraki", 1, 3); // a mage next to the warrior
    b.send(vn::EnterWorldReq{bId});
    // Exactly: ENTER_WORLD_RES, INVENTORY, COINS (v4), SKILL_STATE + SKILL_BAR (v6), CHAR_STATS (v5), then
    // the world (A's spawn).
    const auto eb = b.expect<vn::EnterWorldRes>();
    REQUIRE(eb.result == vn::EnterWorldResult::Ok);
    const auto inv = b.expect<vn::Inventory>();
    CHECK(inv.items.size() == 2); // staff equipped, frost staff in the bag
    CHECK(entryAt(inv, kEquip, 0)->templateId == kStaff);
    CHECK(entryAt(inv, kBag, 0)->templateId == 8);
    CHECK(b.expect<vn::Coins>() == vn::Coins{0, 0});
    CHECK(b.expect<vn::SkillState>() == vn::SkillState{0, 0, {{3, 1}}}); // v6: the mage's free skill
    CHECK(b.expect<vn::SkillBar>().slots == vn::SkillBarSlots{3, 0, 0, 0, 0, 0, 0, 0});
    CHECK(b.expect<vn::QuestList>().quests.empty()); // v11: then the quests (none yet)
    CHECK(b.expect<vn::CharStats>().attributes[3].base == 13); // v5: CHAR_STATS (a mage: Zekâ 13)
    const auto spawnOfA = b.expect<vn::EntitySpawn>();
    CHECK(spawnOfA.entityId == ea.entityId);
    CHECK(spawnOfA.mainHandTemplate == kSword);
    CHECK(spawnOfA.offHandTemplate == kShield);

    const auto spawnOfB = a.waitFor<vn::EntitySpawn>([&](const vn::EntitySpawn& s) { return s.entityId == eb.entityId; });
    REQUIRE(spawnOfB);
    CHECK(spawnOfB->mainHandTemplate == kStaff);
    CHECK(spawnOfB->offHandTemplate == 0);
}

TEST_CASE("Items: ENTITY_EQUIP reaches the mover and players in view, not players out of view", "[server][items]")
{
    TestServer server({});
    TestClient a;
    REQUIRE(a.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto ea = a.enterNew("equip_a", "Kuşanan");
    TestClient nearby;
    REQUIRE(nearby.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    nearby.enterNew("equip_near", "Yakındaki"); // Aydor: same spawn, in view
    TestClient distant;
    REQUIRE(distant.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    distant.enterNew("equip_far", "Uzaktaki", 2); // Kargan: the other side of the map
    a.pumpFor(200ms);

    // Two-handed axe from the bag: the shield goes to the bag as well (4 changed slots).
    vn::ItemUpdate update;
    REQUIRE(a.moveItem(1, kBag, 0, kEquip, 0, &update) == R::Ok);
    REQUIRE(update.items.size() == 4);
    CHECK(update.items[0] == vn::ItemEntry{kEquip, 0, update.items[0].itemId, kAxe, 1});
    CHECK(update.items[1].container == kBag);
    CHECK(update.items[1].slot == 0);
    CHECK(update.items[1].templateId == kSword);
    CHECK(update.items[2] == vn::ItemEntry{kEquip, 1, 0, 0, 0});
    CHECK(update.items[3].container == kBag);
    CHECK(update.items[3].slot == 1);
    CHECK(update.items[3].templateId == kShield);

    const auto own = a.waitFor<vn::EntityEquip>([](const vn::EntityEquip&) { return true; });
    REQUIRE(own);
    CHECK(*own == vn::EntityEquip{ea.entityId, kAxe, 0});
    const auto seen = nearby.waitFor<vn::EntityEquip>([](const vn::EntityEquip&) { return true; });
    REQUIRE(seen);
    CHECK(*seen == vn::EntityEquip{ea.entityId, kAxe, 0});
    CHECK_FALSE(distant.waitFor<vn::EntityEquip>([](const vn::EntityEquip&) { return true; }, 700ms).has_value());

    // A bag-only move changes nothing visible: no ENTITY_EQUIP.
    REQUIRE(a.moveItem(2, kBag, 1, kBag, 20) == R::Ok);
    CHECK_FALSE(nearby.waitFor<vn::EntityEquip>([](const vn::EntityEquip&) { return true; }, 500ms).has_value());

    // Someone arriving later sees the new equipment in the spawn.
    TestClient late;
    REQUIRE(late.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    late.enterNew("equip_late", "Sonradan");
    const auto spawn = late.waitFor<vn::EntitySpawn>([&](const vn::EntitySpawn& s) { return s.entityId == ea.entityId; });
    REQUIRE(spawn);
    CHECK(spawn->mainHandTemplate == kAxe);
    CHECK(spawn->offHandTemplate == 0);
}

TEST_CASE("Items: move results; failures send only ITEM_MOVE_RES and change nothing", "[server][items]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("move_results") == vn::LoginResult::Ok);
    const auto id = create(c, "Deneyen", 1, 1);
    // Extra items before the first entry: they are in the bag before the kit, which fills around them.
    REQUIRE(server.accounts().giveItem(id, kDagger)); // bag 0
    REQUIRE(server.accounts().giveItem(id, kSword3)); // bag 1
    c.enter(id);
    const auto inv = c.lastInventory();
    REQUIRE(inv.items.size() == 5);
    CHECK(entryAt(inv, kBag, 0)->templateId == kDagger);
    CHECK(entryAt(inv, kBag, 1)->templateId == kSword3);
    CHECK(entryAt(inv, kBag, 2)->templateId == kAxe); // the kit's bag item takes the next free slot
    const auto before = stored(server, id);

    std::uint32_t seq = 10;
    CHECK(c.moveItem(++seq, kBag, 0, kEquip, 0) == R::WrongClass);
    CHECK(c.moveItem(++seq, kBag, 1, kEquip, 0) == R::LevelTooLow);
    CHECK(c.moveItem(++seq, kEquip, 1, kEquip, 0) == R::WrongSlot);
    CHECK(c.moveItem(++seq, kEquip, 0, kBag, 0) == R::WrongClass); // the dagger cannot take MainHand
    CHECK(c.moveItem(++seq, kBag, 5, kBag, 6) == R::NoItem);
    CHECK(c.moveItem(++seq, kBag, 5, kBag, 5) == R::BadSlot);
    CHECK(c.moveItem(++seq, kBag, 0, kBag, 28) == R::BadSlot);
    CHECK(c.moveItem(++seq, 2, 0, kBag, 3) == R::BadSlot);
    CHECK(c.moveItem(++seq, kEquip, 0, kEquip, 2) == R::WrongSlot); // Head takes nothing in M8
    CHECK(stored(server, id) == before);

    // Axe into MainHand, then the shield back into OffHand: SlotBlocked.
    std::this_thread::sleep_for(1100ms); // nine requests so far: stay under 10/s
    REQUIRE(c.moveItem(++seq, kBag, 2, kEquip, 0) == R::Ok);
    const auto afterAxe = stored(server, id);
    CHECK(templateAt(afterAxe, kEquip, 0) == kAxe);
    CHECK(templateAt(afterAxe, kBag, 2) == kSword);
    CHECK(templateAt(afterAxe, kBag, 3) == kShield);
    CHECK(templateAt(afterAxe, kEquip, 1) == 0);
    CHECK(c.moveItem(++seq, kBag, 3, kEquip, 1) == R::SlotBlocked);
    CHECK(stored(server, id) == afterAxe);
}

TEST_CASE("Items: BagFull leaves everything as it was", "[server][items]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("bag_full", "Dolubel");
    const auto id = c.lastCharId();
    // The kit uses bag 0; fill bags 1..27 (given while in the world: visible on the next entry).
    for (int i = 1; i < 28; ++i)
        REQUIRE(server.accounts().giveItem(id, kSword3));
    c.send(vn::LeaveWorldReq{});
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    c.enter(id);
    REQUIRE(c.lastInventory().items.size() == 30);
    const auto before = stored(server, id);
    CHECK(c.moveItem(1, kBag, 0, kEquip, 0) == R::BagFull); // the shield would have nowhere to go
    CHECK(stored(server, id) == before);
    // Unequipping the shield first frees nothing (the bag is full): drop the sword into the axe's slot.
    CHECK(c.moveItem(2, kEquip, 1, kBag, 5) == R::WrongSlot);
}

TEST_CASE("Items: pipelined moves are decided in order against committed state", "[server][items]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("pipeline", "Hızlıel");
    const auto id = c.lastCharId();
    // Same slots, back to back, before the first commit returns.
    c.send(vn::ItemMoveReq{1, kEquip, 0, kBag, 5}); // sword out
    c.send(vn::ItemMoveReq{2, kBag, 5, kEquip, 0}); // and back in
    c.send(vn::ItemMoveReq{3, kBag, 0, kBag, 7});   // axe 0 -> 7
    c.send(vn::ItemMoveReq{4, kBag, 0, kBag, 8});   // bag 0 is empty by now
    c.send(vn::ItemMoveReq{5, kEquip, 0, kBag, 0}); // sword into the freed slot
    std::vector<std::pair<std::uint32_t, R>> results;
    int updates = 0;
    while (results.size() < 5)
    {
        auto m = c.next();
        REQUIRE(m);
        if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::ItemUpdate))
            ++updates;
        if (m->opcode != static_cast<std::uint16_t>(vn::Opcode::ItemMoveRes))
            continue;
        vn::ItemMoveRes res;
        REQUIRE(vn::decodeMessage(m->payload, res));
        results.emplace_back(res.seq, res.result);
    }
    CHECK(results == std::vector<std::pair<std::uint32_t, R>>{{1, R::Ok}, {2, R::Ok}, {3, R::Ok}, {4, R::NoItem},
                                                             {5, R::Ok}});
    CHECK(updates == 4);
    const auto items = stored(server, id);
    CHECK(items.size() == 3);
    CHECK(templateAt(items, kBag, 0) == kSword);
    CHECK(templateAt(items, kBag, 7) == kAxe);
    CHECK(templateAt(items, kEquip, 1) == kShield);
    CHECK(templateAt(items, kEquip, 0) == 0);
}

TEST_CASE("Items: more than 10 ITEM_MOVE_REQ per second is Flood", "[server][items][flood]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("item_flood", "Seli");
    for (std::uint32_t i = 0; i < 11; ++i)
        c.send(vn::ItemMoveReq{i, kBag, 3, kBag, 4}); // NoItem each, but counted
    const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; });
    REQUIRE(err);
    CHECK(err->errorCode == vn::ErrorCode::Flood);
    CHECK(c.waitClosed());
}

TEST_CASE("Items: ten per second are fine; ITEM_MOVE_REQ outside the world is BadState", "[server][items]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("item_rate", "Sakin");
    for (std::uint32_t i = 0; i < 10; ++i)
        CHECK(c.moveItem(i, kBag, 3, kBag, 4) == R::NoItem);
    std::this_thread::sleep_for(1100ms);
    CHECK(c.moveItem(99, kBag, 0, kBag, 1) == R::Ok);
    c.send(vn::LeaveWorldReq{});
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    c.send(vn::ItemMoveReq{100, kBag, 1, kBag, 0});
    CHECK(c.expectSkippingWorld<vn::Error>().errorCode == vn::ErrorCode::BadState);
    CHECK(c.waitClosed());

    TestClient m;
    REQUIRE(m.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(m.login("item_malformed") == vn::LoginResult::Ok);
    create(m, "Bozuk", 1, 1);
    m.send(vn::CharListReq{});
    const auto list = m.expect<vn::CharListRes>();
    m.enter(list.characters[0].charId);
    m.sendRawPayload(vn::Opcode::ItemMoveReq, {1, 2, 3});
    CHECK(m.expectSkippingWorld<vn::Error>().errorCode == vn::ErrorCode::ProtocolViolation);
}

TEST_CASE("Items: a failed write answers ServerError and keeps the old state", "[server][items]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("item_dbfail", "Hatalı");
    const auto id = c.lastCharId();
    const auto before = stored(server, id);
    server.accounts().setFailItemMovesForTesting(true);
    CHECK(c.moveItem(1, kEquip, 1, kBag, 3) == R::ServerError);
    server.accounts().setFailItemMovesForTesting(false);
    CHECK(stored(server, id) == before);
    // The session's view did not change either: the same move now works, and its ITEM_UPDATE shows
    // the shield leaving OffHand.
    vn::ItemUpdate update;
    REQUIRE(c.moveItem(2, kEquip, 1, kBag, 3, &update) == R::Ok);
    REQUIRE(update.items.size() == 2);
    CHECK(update.items[0].templateId == kShield);
    CHECK(update.items[1] == vn::ItemEntry{kEquip, 1, 0, 0, 0});
}

TEST_CASE("Items: items with unknown templates are kept, hidden and never moved", "[server][items]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(c.login("hidden_items") == vn::LoginResult::Ok);
    const auto id = create(c, "Gizli", 1, 1);
    REQUIRE(server.accounts().giveItem(id, 999)); // bag 0, a template this server does not know
    c.enter(id);
    const auto inv = c.lastInventory();
    CHECK(inv.items.size() == 3); // the kit only
    CHECK_FALSE(entryAt(inv, kBag, 0).has_value());
    CHECK(entryAt(inv, kBag, 1)->templateId == kAxe); // the kit went around it
    CHECK(c.moveItem(1, kBag, 0, kBag, 5) == R::NoItem);
    CHECK(c.moveItem(2, kBag, 1, kBag, 0) == R::SlotBlocked);
    const auto items = stored(server, id);
    CHECK(items.size() == 4);
    CHECK(templateAt(items, kBag, 0) == 999);
}
