// client/world/ClientInventory: the client's inventory model (docs/PROTOCOL_ITEMS.md §3, §6):
// optimistic moves through the shared rules, settling by ITEM_UPDATE, rollback on a refused
// ITEM_MOVE_RES, throttling; plus the Turkish result texts.

#include "client/net/NetText.h"
#include "client/world/ClientInventory.h"
#include "client/world/WorldModel.h"
#include "shared/game/Character.h"
#include "shared/game/Items.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace vg = vo::game;
namespace vn = vo::net;
using vo::client::world::ClientInventory;
using R = vg::ItemMoveResult;
using vg::EquipSlot;
using vg::ItemPlace;

namespace {

constexpr vg::ItemTemplateId kSword = 1;
constexpr vg::ItemTemplateId kShield = 2;
constexpr vg::ItemTemplateId kAxe = 6;
constexpr std::uint8_t kWarrior = static_cast<std::uint8_t>(vg::CharClass::Warrior);

const vg::ItemCatalog& catalog()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        const auto path = vg::locateItemsFile();
        REQUIRE(path.has_value());
        std::string error;
        REQUIRE(vg::loadItemCatalog(*path, out, error));
        return out;
    }();
    return c;
}

constexpr ItemPlace mainHand() { return ItemPlace::equipment(EquipSlot::MainHand); }
constexpr ItemPlace offHand() { return ItemPlace::equipment(EquipSlot::OffHand); }
constexpr ItemPlace bag(std::uint8_t s) { return ItemPlace::bag(s); }

vn::ItemEntry entry(ItemPlace p, std::uint64_t id, vg::ItemTemplateId tmpl)
{
    vn::ItemEntry e;
    e.container = p.container;
    e.slot = p.slot;
    e.itemId = id;
    e.templateId = id != 0 ? tmpl : 0;
    e.quantity = id != 0 ? 1 : 0;
    return e;
}

// A warrior after the starter kit: sword (101) + shield (102) equipped, axe (103) in bag slot 0.
ClientInventory warrior()
{
    ClientInventory inv;
    inv.setRules(&catalog(), {kWarrior, 1});
    vn::Inventory msg;
    msg.items = {entry(mainHand(), 101, kSword), entry(offHand(), 102, kShield), entry(bag(0), 103, kAxe)};
    std::string error;
    REQUIRE(inv.applyInventory(msg, error));
    return inv;
}

vg::ItemTemplateId at(const vg::Inventory& inv, ItemPlace p)
{
    return inv.at(p).templateId;
}

// The ITEM_UPDATE the server sends for the axe move (planItemMove's changes).
vn::ItemUpdate axeUpdate()
{
    vn::ItemUpdate u;
    u.items = {entry(mainHand(), 103, kAxe), entry(bag(0), 101, kSword), entry(offHand(), 0, 0),
               entry(bag(1), 102, kShield)};
    return u;
}

vn::ItemMoveRes result(std::uint32_t seq, R r)
{
    vn::ItemMoveRes m;
    m.seq = seq;
    m.result = r;
    return m;
}

} // namespace

TEST_CASE("client inventory: nothing is sent before INVENTORY", "[client][inventory]")
{
    ClientInventory inv;
    inv.setRules(&catalog(), {kWarrior, 1});
    const auto b = inv.beginMove(bag(0), mainHand(), 0.0);
    CHECK(b.status == ClientInventory::BeginStatus::NotReady);
    CHECK_FALSE(b.request);
    CHECK_FALSE(inv.loaded());
}

TEST_CASE("client inventory: INVENTORY is validated", "[client][inventory]")
{
    ClientInventory inv;
    inv.setRules(&catalog(), {kWarrior, 1});
    vn::Inventory msg;
    msg.items = {entry(bag(3), 7, kSword), entry(bag(3), 8, kShield)};
    std::string error;
    CHECK_FALSE(inv.applyInventory(msg, error));
    CHECK_FALSE(inv.loaded());
    msg.items = {entry({1, 40}, 7, kSword)};
    CHECK_FALSE(inv.applyInventory(msg, error));
    msg.items = {entry(bag(27), 7, kSword)};
    CHECK(inv.applyInventory(msg, error));
    CHECK(at(inv.view(), bag(27)) == kSword);
}

TEST_CASE("client inventory: the two-handed axe is shown at once and settled by the server", "[client][inventory]")
{
    ClientInventory inv = warrior();
    CHECK(inv.visible() == vg::VisibleEquipment{kSword, kShield});
    const auto b = inv.beginMove(bag(0), mainHand(), 1.0);
    REQUIRE(b.status == ClientInventory::BeginStatus::Sent);
    REQUIRE(b.request);
    CHECK(b.request->fromContainer == 1);
    CHECK(b.request->fromSlot == 0);
    CHECK(b.request->toContainer == 0);
    CHECK(b.request->toSlot == 0);
    // Optimistic: axe in hand, sword where the axe was, shield in the first free bag slot.
    CHECK(at(inv.view(), mainHand()) == kAxe);
    CHECK(at(inv.view(), offHand()) == 0);
    CHECK(at(inv.view(), bag(0)) == kSword);
    CHECK(at(inv.view(), bag(1)) == kShield);
    CHECK(inv.visible() == vg::VisibleEquipment{kAxe, 0});
    // The server has not changed yet.
    CHECK(at(inv.confirmed(), mainHand()) == kSword);

    // ITEM_UPDATE first: the move is settled (not replayed on top of the new state).
    REQUIRE(inv.applyUpdate(axeUpdate()));
    CHECK(inv.pendingCount() == 1);
    CHECK(at(inv.view(), mainHand()) == kAxe);
    CHECK(at(inv.view(), bag(0)) == kSword);
    CHECK(at(inv.view(), bag(1)) == kShield);
    // Then ITEM_MOVE_RES(Ok).
    const auto a = inv.applyResult(result(b.request->seq, R::Ok));
    REQUIRE(a);
    CHECK(a->result == R::Ok);
    CHECK(inv.pendingCount() == 0);
    CHECK(inv.view() == inv.confirmed());
    CHECK(at(inv.view(), mainHand()) == kAxe);
}

TEST_CASE("client inventory: a refused move is rolled back", "[client][inventory]")
{
    ClientInventory inv = warrior();
    const vg::Inventory before = inv.view();
    const auto b = inv.beginMove(bag(0), mainHand(), 1.0);
    REQUIRE(b.request);
    CHECK(at(inv.view(), mainHand()) == kAxe);
    const auto a = inv.applyResult(result(b.request->seq, R::ServerError));
    REQUIRE(a);
    CHECK(a->result == R::ServerError);
    CHECK(a->from == bag(0));
    CHECK(a->to == mainHand());
    CHECK(inv.view() == before);
    CHECK(inv.pendingCount() == 0);
    // An answer for an unknown seq changes nothing.
    CHECK_FALSE(inv.applyResult(result(999, R::Ok)));
    CHECK(inv.view() == before);
}

TEST_CASE("client inventory: several moves in flight", "[client][inventory]")
{
    ClientInventory inv = warrior();
    // 1: axe into the hand; 2: the (now bagged) sword from bag 0 to bag 5.
    const auto b1 = inv.beginMove(bag(0), mainHand(), 1.0);
    const auto b2 = inv.beginMove(bag(0), bag(5), 1.0);
    REQUIRE(b1.request);
    REQUIRE(b2.request);
    CHECK(b2.request->seq != b1.request->seq);
    CHECK(at(inv.view(), bag(5)) == kSword);
    CHECK(at(inv.view(), bag(0)) == 0);
    // The server settles move 1 ...
    REQUIRE(inv.applyUpdate(axeUpdate()));
    REQUIRE(inv.applyResult(result(b1.request->seq, R::Ok)));
    CHECK(at(inv.view(), mainHand()) == kAxe);
    CHECK(at(inv.view(), bag(5)) == kSword); // move 2 still shown on top
    // ... and refuses move 2: the sword goes back to bag 0 (the confirmed state).
    const auto a2 = inv.applyResult(result(b2.request->seq, R::BadSlot));
    REQUIRE(a2);
    CHECK(at(inv.view(), bag(0)) == kSword);
    CHECK(at(inv.view(), bag(5)) == 0);
}

TEST_CASE("client inventory: moves the shared rules refuse are not sent", "[client][inventory]")
{
    ClientInventory inv = warrior();
    // Shield into the main hand: WrongSlot.
    auto b = inv.beginMove(offHand(), mainHand(), 1.0);
    CHECK(b.status == ClientInventory::BeginStatus::Refused);
    CHECK(b.result == R::WrongSlot);
    CHECK_FALSE(b.request);
    CHECK(inv.pendingCount() == 0);
    // Empty source, same place.
    CHECK(inv.beginMove(bag(9), bag(10), 1.0).result == R::NoItem);
    CHECK(inv.beginMove(bag(0), bag(0), 1.0).result == R::BadSlot);
    CHECK(inv.check(bag(0), mainHand()) == R::Ok);
    CHECK(inv.check(bag(0), offHand()) == R::WrongSlot);
    // Forced (automation): sent, but not shown optimistically.
    b = inv.beginMove(offHand(), mainHand(), 1.0, true);
    REQUIRE(b.status == ClientInventory::BeginStatus::Sent);
    REQUIRE(b.request);
    CHECK(at(inv.view(), mainHand()) == kSword);
    const auto a = inv.applyResult(result(b.request->seq, R::WrongSlot));
    REQUIRE(a);
    CHECK(a->result == R::WrongSlot);
}

TEST_CASE("client inventory: at most kMaxMovesPerSecond requests per second", "[client][inventory]")
{
    ClientInventory inv = warrior();
    int sent = 0;
    std::uint8_t slot = 0; // where the axe is (in view())
    for (int i = 0; i < ClientInventory::kMaxMovesPerSecond + 3; ++i)
    {
        // Shuffle the axe between two bag slots.
        const auto to = static_cast<std::uint8_t>(slot == 0 ? 1 : 0);
        const auto b = inv.beginMove(bag(slot), bag(to), 5.0 + 0.01 * i);
        if (b.status == ClientInventory::BeginStatus::Sent)
        {
            ++sent;
            slot = to;
        }
        else
        {
            CHECK(b.status == ClientInventory::BeginStatus::Throttled);
        }
    }
    CHECK(sent == ClientInventory::kMaxMovesPerSecond);
    // A second later there is room again.
    CHECK(inv.beginMove(bag(0), bag(2), 6.5).status != ClientInventory::BeginStatus::Throttled);
}

TEST_CASE("client inventory: quick move targets", "[client][inventory]")
{
    ClientInventory inv = warrior();
    CHECK(inv.quickTarget(bag(0)) == mainHand());
    CHECK(inv.quickTarget(mainHand()) == bag(1));
    CHECK(inv.quickTarget(offHand()) == bag(1));
    CHECK_FALSE(inv.quickTarget(bag(4)));
    // INVENTORY again drops whatever was pending.
    REQUIRE(inv.beginMove(bag(0), mainHand(), 1.0).request);
    vn::Inventory msg;
    msg.items = {entry(mainHand(), 101, kSword)};
    std::string error;
    REQUIRE(inv.applyInventory(msg, error));
    CHECK(inv.pendingCount() == 0);
    CHECK(inv.visible() == vg::VisibleEquipment{kSword, 0});
}

TEST_CASE("world model: remote weapons from ENTITY_SPAWN and ENTITY_EQUIP", "[client][inventory]")
{
    vo::client::world::WorldModel model;
    vn::EnterWorldRes enter;
    enter.entityId = 7;
    model.enter(enter, nullptr, 0.0, true);
    vn::EntitySpawn spawn;
    spawn.entityId = 9;
    spawn.name = "Gozcu";
    spawn.mainHandTemplate = 3;
    spawn.offHandTemplate = 3;
    REQUIRE(model.handleMessage(static_cast<std::uint16_t>(vn::Opcode::EntitySpawn), *vn::encodeMessage(spawn), 0.0));
    const auto* e = model.find(9);
    REQUIRE(e != nullptr);
    CHECK(e->mainHand == 3);
    CHECK(e->offHand == 3);
    vn::EntityEquip equip;
    equip.entityId = 9;
    equip.mainHandTemplate = kAxe;
    REQUIRE(model.handleMessage(static_cast<std::uint16_t>(vn::Opcode::EntityEquip), *vn::encodeMessage(equip), 0.0));
    CHECK(e->mainHand == kAxe);
    CHECK(e->offHand == 0);
    CHECK(e->equipChanges == 1);
    // Our own entity: remembered, no remote created; unknown ids are counted, not created.
    equip.entityId = 7;
    REQUIRE(model.handleMessage(static_cast<std::uint16_t>(vn::Opcode::EntityEquip), *vn::encodeMessage(equip), 0.0));
    REQUIRE(model.localEquip());
    CHECK(model.localEquip()->mainHandTemplate == kAxe);
    equip.entityId = 99;
    REQUIRE(model.handleMessage(static_cast<std::uint16_t>(vn::Opcode::EntityEquip), *vn::encodeMessage(equip), 0.0));
    CHECK(model.activeCount() == 1);
    // A truncated ENTITY_EQUIP is a decode error.
    CHECK_FALSE(model.handleMessage(static_cast<std::uint16_t>(vn::Opcode::EntityEquip), std::vector<std::uint8_t>{1, 2},
                                    0.0));
}

TEST_CASE("client inventory: every move result has a Turkish text", "[client][inventory]")
{
    CHECK(vo::client::net::itemMoveResultMessage(R::Ok).empty());
    for (std::uint8_t r = 1; r <= vn::kItemMoveResultMax; ++r)
    {
        const std::string text = vo::client::net::itemMoveResultMessage(static_cast<R>(r));
        INFO(static_cast<int>(r));
        CHECK_FALSE(text.empty());
    }
    CHECK(vo::client::net::itemMoveResultMessage(static_cast<R>(77)).find("77") != std::string::npos);
}
