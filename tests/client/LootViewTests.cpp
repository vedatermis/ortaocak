// M10 "Ganimet" client logic (client/world/LootView.h, ClientInventory's loot takes, WorldModel's
// loot containers): label state selection and styles, number formatting, Turkish result texts, the
// loot window model, pickup feedback, item tooltips, the Esc order, the interact key's bag choice and
// the loot automation command parser (including its error messages).
#include "client/world/ClientInventory.h"
#include "client/world/LootView.h"
#include "client/world/WorldModel.h"
#include "shared/game/Character.h"
#include "shared/game/Items.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

namespace world = vo::client::world;
namespace vg = vo::game;
namespace vn = vo::net;
using world::LootLabel;
using world::Vec3;

namespace {

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

constexpr vg::ItemTemplateId kSword = 1;
constexpr vg::ItemTemplateId kShield = 2;
constexpr vg::ItemTemplateId kAshDust = 9;   // Kül Tozu, material, stack 20, value 2
constexpr vg::ItemTemplateId kEmberCore = 11; // Kor Çekirdeği, rare
constexpr vg::ItemTemplateId kWardenShield = 15;
constexpr std::uint8_t kWarrior = static_cast<std::uint8_t>(vg::CharClass::Warrior);
constexpr std::uint8_t kMage = static_cast<std::uint8_t>(vg::CharClass::Mage);

vn::ItemEntry entry(vg::ItemPlace p, std::uint64_t id, vg::ItemTemplateId tmpl, std::uint16_t qty = 1)
{
    vn::ItemEntry e;
    e.container = p.container;
    e.slot = p.slot;
    e.itemId = id;
    e.templateId = id != 0 ? tmpl : 0;
    e.quantity = id != 0 ? qty : 0;
    return e;
}

vn::LootContents contents(std::uint32_t id, std::uint32_t coins, std::vector<vn::LootEntry> entries)
{
    vn::LootContents m;
    m.lootId = id;
    m.coins = coins;
    m.entries = std::move(entries);
    return m;
}

bool hasLine(const std::vector<world::TipLine>& lines, const std::string& text)
{
    for (const world::TipLine& l : lines)
        if (l.text.find(text) != std::string::npos)
            return true;
    return false;
}

const world::TipLine* line(const std::vector<world::TipLine>& lines, const std::string& text)
{
    for (const world::TipLine& l : lines)
        if (l.text.find(text) != std::string::npos)
            return &l;
    return nullptr;
}

} // namespace

TEST_CASE("Loot view: label state from the entity state bits", "[client][loot]")
{
    CHECK(world::lootLabel(0) == LootLabel::Free);
    CHECK(world::lootLabel(vn::kEntityStateLootMine) == LootLabel::Mine);
    CHECK(world::lootLabel(vn::kEntityStateLootLocked) == LootLabel::Locked);
    // Other bits do not matter; "mine" wins if both were ever set.
    CHECK(world::lootLabel(vn::kEntityStateInCombat) == LootLabel::Free);
    CHECK(world::lootLabel(vn::kEntityStateLootMine | vn::kEntityStateLootLocked) == LootLabel::Mine);
    CHECK(std::string(world::lootLabelName(LootLabel::Mine)) == "mine");
    CHECK(std::string(world::lootLabelName(LootLabel::Locked)) == "locked");
    CHECK(std::string(world::lootLabelName(LootLabel::Free)) == "free");

    const auto mine = world::lootLabelStyle(LootLabel::Mine);
    const auto locked = world::lootLabelStyle(LootLabel::Locked);
    const auto free = world::lootLabelStyle(LootLabel::Free);
    // Three distinct label colours; only ours pulses; the locked bag is dimmed, the others are not.
    CHECK(std::set<std::uint32_t>{mine.labelRgb, locked.labelRgb, free.labelRgb}.size() == 3);
    CHECK(mine.ringPulse);
    CHECK_FALSE(locked.ringPulse);
    CHECK_FALSE(free.ringPulse);
    CHECK(locked.meshTint < 0.8f);
    CHECK(mine.meshTint == 1.0f);
    CHECK(free.meshTint == 1.0f);
    CHECK(locked.labelAlpha < mine.labelAlpha);
    CHECK(locked.ringAlpha < free.ringAlpha);
    CHECK(free.ringAlpha < mine.ringAlpha);
    // Gold for ours: red channel highest, blue lowest.
    CHECK(((mine.labelRgb >> 16) & 0xFF) > (mine.labelRgb & 0xFF));
}

TEST_CASE("Loot view: thousands separators", "[client][loot]")
{
    CHECK(world::formatThousands(0) == "0");
    CHECK(world::formatThousands(7) == "7");
    CHECK(world::formatThousands(999) == "999");
    CHECK(world::formatThousands(1000) == "1.000");
    CHECK(world::formatThousands(12345) == "12.345");
    CHECK(world::formatThousands(123456) == "123.456");
    CHECK(world::formatThousands(1234567) == "1.234.567");
    CHECK(world::formatThousands(vn::kMaxCoins) == "1.000.000.000");
    CHECK(world::coinPickupText(7) == "+7 Akçe");
    CHECK(world::coinPickupText(1500) == "+1.500 Akçe");
}

TEST_CASE("Loot view: every result and close reason has a Turkish text", "[client][loot]")
{
    CHECK(world::lootResultText(vn::LootResult::Ok).empty());
    std::set<std::string_view> texts;
    for (std::uint8_t v = 1; v <= vn::kLootResultMax; ++v)
    {
        const std::string_view t = world::lootResultText(static_cast<vn::LootResult>(v));
        CHECK_FALSE(t.empty());
        texts.insert(t);
    }
    CHECK(texts.size() == vn::kLootResultMax); // all different
    // The agreed texts (docs/PROTOCOL_LOOT.md §9).
    CHECK(world::lootResultText(vn::LootResult::NotFound) == "Ganimet bulunamadı");
    CHECK(world::lootResultText(vn::LootResult::YouAreDead) == "Ölüyken ganimet alınmaz");
    CHECK(world::lootResultText(vn::LootResult::OutOfRange) == "Çok uzaktasın");
    CHECK(world::lootResultText(vn::LootResult::NotYours) == "Bu ganimet henüz başkasının");
    CHECK(world::lootResultText(vn::LootResult::AlreadyTaken) == "Bu çoktan alındı");
    CHECK(world::lootResultText(vn::LootResult::BagFull) == "Çantan dolu");
    CHECK(world::lootResultText(vn::LootResult::PurseFull) == "Kesen dolu");
    CHECK(world::lootResultText(vn::LootResult::ServerError) == "Ganimet alınamadı, tekrar dene");
    CHECK(world::lootClosedText(vn::LootClosedReason::Gone).empty());
    CHECK(world::lootClosedText(vn::LootClosedReason::YouDied).empty());
    CHECK_FALSE(world::lootClosedText(vn::LootClosedReason::OutOfRange).empty());
    // Rarity colours differ.
    CHECK(std::set<std::uint32_t>{world::rarityRgb(vg::Rarity::Common), world::rarityRgb(vg::Rarity::Uncommon),
                                  world::rarityRgb(vg::Rarity::Rare)}
              .size() == 3);
}

TEST_CASE("Loot view: the loot window model", "[client][loot]")
{
    world::LootWindowModel m;
    CHECK_FALSE(m.isOpen());
    CHECK(m.rows().empty());
    // Contents for a bag that is not open are ignored.
    CHECK_FALSE(m.applyContents(contents(40, 7, {})));
    m.open(40);
    CHECK(m.isOpen());
    CHECK(m.lootId() == 40);
    CHECK_FALSE(m.hasContents());
    CHECK_FALSE(m.applyContents(contents(41, 7, {})));
    REQUIRE(m.applyContents(contents(40, 7, {{0, kAshDust, 2}, {2, kSword, 1}})));
    CHECK(m.hasContents());
    // The Akçe row first, then the entries by index.
    auto rows = m.rows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].coins);
    CHECK(rows[0].quantity == 7);
    CHECK(rows[0].what() == vn::kLootTakeCoins);
    CHECK_FALSE(rows[1].coins);
    CHECK(rows[1].index == 0);
    CHECK(rows[1].templateId == kAshDust);
    CHECK(rows[1].quantity == 2);
    CHECK(rows[1].what() == 0);
    CHECK(rows[2].what() == 2);
    CHECK(m.row(vn::kLootTakeCoins));
    CHECK(m.row(2));
    CHECK_FALSE(m.row(1));
    CHECK_FALSE(m.empty());
    // Coins taken: no Akçe row any more; indices stay as the server keeps them.
    REQUIRE(m.applyContents(contents(40, 0, {{2, kSword, 1}})));
    rows = m.rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].index == 2);
    CHECK_FALSE(m.row(vn::kLootTakeCoins));
    REQUIRE(m.applyContents(contents(40, 0, {})));
    CHECK(m.empty());
    CHECK(m.rows().empty());

    // Takes: distinct non-zero seqs, answered once.
    const std::uint32_t s1 = m.beginTake(vn::kLootTakeAll);
    const std::uint32_t s2 = m.beginTake(0);
    CHECK(s1 != 0);
    CHECK(s2 != s1);
    CHECK(m.takesInFlight() == 2);
    const auto t1 = m.finishTake(s1);
    REQUIRE(t1);
    CHECK(t1->lootId == 40);
    CHECK(t1->what == vn::kLootTakeAll);
    CHECK_FALSE(m.finishTake(s1));
    CHECK_FALSE(m.finishTake(999));
    // Closing keeps the takes in flight (their answers still come) but forgets the bag.
    m.close();
    CHECK_FALSE(m.isOpen());
    CHECK(m.takesInFlight() == 1);
    CHECK(m.finishTake(s2));
    // Reopening another bag starts without contents.
    m.open(41);
    CHECK_FALSE(m.hasContents());
    CHECK(m.rows().empty());
}

TEST_CASE("Loot view: pickup feedback from inventory changes", "[client][loot]")
{
    std::string error;
    vg::Inventory before;
    REQUIRE(vg::Inventory::fromItems({{101, vg::ItemPlace::equipment(vg::EquipSlot::MainHand), kSword, 1},
                                      {102, vg::ItemPlace::bag(0), kAshDust, 3}},
                                     before, error));
    // A take: the dust stack grows by 2, a rare core lands in a new slot.
    vg::Inventory after;
    REQUIRE(vg::Inventory::fromItems({{101, vg::ItemPlace::equipment(vg::EquipSlot::MainHand), kSword, 1},
                                      {102, vg::ItemPlace::bag(0), kAshDust, 5},
                                      {103, vg::ItemPlace::bag(1), kEmberCore, 1}},
                                     after, error));
    const auto gains = world::inventoryGains(before, after);
    REQUIRE(gains.size() == 2);
    CHECK(gains[0] == world::ItemGain{kAshDust, 2});
    CHECK(gains[1] == world::ItemGain{kEmberCore, 1});
    // A move or a merge changes nothing.
    vg::Inventory moved;
    REQUIRE(vg::Inventory::fromItems({{101, vg::ItemPlace::bag(4), kSword, 1}, {102, vg::ItemPlace::bag(7), kAshDust, 3}},
                                     moved, error));
    CHECK(world::inventoryGains(before, moved).empty());
    CHECK(world::inventoryGains(after, before).empty()); // losses are not pickups

    CHECK(world::pickupText(catalog().find(kAshDust), kAshDust, 2) == "Kül Tozu x2 alındı");
    CHECK(world::pickupText(catalog().find(kEmberCore), kEmberCore, 1) == "Kor Çekirdeği alındı");
    CHECK(world::pickupText(nullptr, 77, 1) == "Eşya #77 alındı");
}

TEST_CASE("Loot view: item tooltips", "[client][loot]")
{
    const vg::ItemUser warrior{kWarrior, 1};
    // A material stack: kind and rarity, count / max stack, value per unit and for the stack, no
    // class or level line.
    const vg::ItemTemplate* dust = catalog().find(kAshDust);
    REQUIRE(dust != nullptr);
    auto lines = world::itemTipLines(*dust, warrior, 7);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines[0].text == "Kül Tozu");
    CHECK(lines[0].tone == world::TipTone::Title);
    CHECK(lines[0].rgb == world::rarityRgb(vg::Rarity::Common));
    CHECK(hasLine(lines, "Malzeme · Sıradan"));
    CHECK(hasLine(lines, "Adet: 7 / 20"));
    CHECK(hasLine(lines, "Değer: 2 Akçe (yığın 14 Akçe)"));
    CHECK_FALSE(hasLine(lines, "Sınıf"));
    CHECK_FALSE(hasLine(lines, "Gereken seviye"));
    // A single unit: no stack value.
    lines = world::itemTipLines(*dust, warrior, 1);
    CHECK(hasLine(lines, "Adet: 1 / 20"));
    CHECK(hasLine(lines, "Değer: 2 Akçe"));
    CHECK_FALSE(hasLine(lines, "yığın"));
    // A rare material: the name in the rare colour.
    const vg::ItemTemplate* core = catalog().find(kEmberCore);
    REQUIRE(core != nullptr);
    lines = world::itemTipLines(*core, warrior, 2);
    CHECK(lines[0].rgb == world::rarityRgb(vg::Rarity::Rare));
    CHECK(hasLine(lines, "Nadir"));
    // A weapon: slot, attack, class and level; unmet requirements are Bad.
    const vg::ItemTemplate* sword = catalog().find(kSword);
    REQUIRE(sword != nullptr);
    lines = world::itemTipLines(*sword, warrior, 1);
    CHECK(hasLine(lines, "Silah · "));
    CHECK_FALSE(hasLine(lines, "Adet")); // not stackable
    const world::TipLine* cls = line(lines, "Sınıf:");
    REQUIRE(cls != nullptr);
    CHECK(cls->tone == world::TipTone::Normal);
    lines = world::itemTipLines(*sword, {kMage, 1}, 1);
    cls = line(lines, "Sınıf:");
    REQUIRE(cls != nullptr);
    CHECK(cls->tone == world::TipTone::Bad);
    // The warden's shield needs level 5 and is uncommon.
    const vg::ItemTemplate* shield = catalog().find(kWardenShield);
    REQUIRE(shield != nullptr);
    lines = world::itemTipLines(*shield, warrior, 1);
    CHECK(lines[0].rgb == world::rarityRgb(vg::Rarity::Uncommon));
    const world::TipLine* lvl = line(lines, "Gereken seviye: 5");
    REQUIRE(lvl != nullptr);
    CHECK(lvl->tone == world::TipTone::Bad);
    CHECK(hasLine(lines, "Savunma +"));
    CHECK(hasLine(lines, "Değer: 70 Akçe"));
}

TEST_CASE("Loot view: Esc closes the loot window, then the inventory, then the target, then opens the menu",
          "[client][loot]")
{
    using A = world::EscapeAction;
    CHECK(world::escapeAction(true, true, true, false) == A::CloseLoot);
    CHECK(world::escapeAction(true, false, false, false) == A::CloseLoot);
    CHECK(world::escapeAction(false, true, true, false) == A::CloseInventory);
    CHECK(world::escapeAction(false, false, true, false) == A::ClearTarget);
    CHECK(world::escapeAction(false, false, false, false) == A::OpenMenu);
    CHECK(world::escapeAction(true, true, true, true) == A::None); // the menu handles its own Esc
}

TEST_CASE("Loot view: the interact key picks the nearest bag we may take from", "[client][loot]")
{
    const Vec3 me{100.0f, 0.0f, 100.0f};
    std::vector<world::LootCandidate> c = {
        {10, {102.0f, 0.0f, 100.0f}, vn::kEntityStateLootLocked}, // nearest, someone else's
        {11, {104.0f, 5.0f, 100.0f}, vn::kEntityStateLootMine},   // 4 m (height ignored)
        {12, {100.0f, 0.0f, 96.0f}, 0},                           // 4 m, free, higher id
        {13, {130.0f, 0.0f, 100.0f}, 0},                          // too far
    };
    CHECK(world::nearestLoot(c, me, 12.0f, false) == 11); // tie at 4 m: the lower id
    CHECK(world::nearestLoot(c, me, 12.0f, true) == 10);
    CHECK(world::nearestLoot(c, me, 3.0f, false) == 0);
    CHECK(world::nearestLoot(c, me, 3.0f, true) == 10);
    CHECK(world::nearestLoot({}, me, 12.0f, true) == 0);
}

TEST_CASE("Loot view: loot automation commands", "[client][loot]")
{
    std::string error;
    using V = world::LootVerb;
    using Op = world::CompareOp;
    // Not a loot verb: nullopt without an error (the scene tries its other commands).
    CHECK_FALSE(world::parseLootCommand("camera 180 25 8", error));
    CHECK(error.empty());
    CHECK_FALSE(world::parseLootCommand("", error));
    CHECK(error.empty());

    auto c = world::parseLootCommand("lootnearest", error);
    REQUIRE(c);
    CHECK(c->verb == V::Nearest);
    CHECK(c->mode.empty());
    c = world::parseLootCommand("lootnearest any", error);
    REQUIRE(c);
    CHECK(c->mode == "any");
    c = world::parseLootCommand("walktoloot", error);
    REQUIRE(c);
    CHECK(c->verb == V::WalkTo);
    CHECK(c->mode == "any");
    c = world::parseLootCommand("walktoloot locked", error);
    REQUIRE(c);
    CHECK(c->mode == "locked");
    c = world::parseLootCommand("takeall", error);
    REQUIRE(c);
    CHECK(c->verb == V::TakeAll);
    c = world::parseLootCommand("take 3", error);
    REQUIRE(c);
    CHECK(c->verb == V::Take);
    CHECK(c->a == 3);
    c = world::parseLootCommand("take coins", error);
    REQUIRE(c);
    CHECK(c->a == vn::kLootTakeCoins);
    c = world::parseLootCommand("lootclose", error);
    REQUIRE(c);
    CHECK(c->verb == V::Close);
    c = world::parseLootCommand("lootlog", error);
    REQUIRE(c);
    CHECK(c->verb == V::Log);
    c = world::parseLootCommand("expectcoins >= 3", error);
    REQUIRE(c);
    CHECK(c->verb == V::ExpectCoins);
    CHECK(c->op == Op::Ge);
    CHECK(c->a == 3);
    c = world::parseLootCommand("expectcoins 12", error);
    REQUIRE(c);
    CHECK(c->op == Op::Eq);
    CHECK(c->a == 12);
    c = world::parseLootCommand("expectitem 9 2", error);
    REQUIRE(c);
    CHECK(c->verb == V::ExpectItem);
    CHECK(c->a == 9);
    CHECK(c->b == 2);
    CHECK(c->op == Op::Eq);
    c = world::parseLootCommand("expectitem 9 > 0", error);
    REQUIRE(c);
    CHECK(c->op == Op::Gt);
    CHECK(c->b == 0);
    c = world::parseLootCommand("expectstack 3 9 4", error);
    REQUIRE(c);
    CHECK(c->verb == V::ExpectStack);
    CHECK(c->a == 3);
    CHECK(c->b == 9);
    CHECK(c->c == 4);
    c = world::parseLootCommand("expectlootrows 2", error);
    REQUIRE(c);
    CHECK(c->verb == V::ExpectRows);
    c = world::parseLootCommand("expectlootmessage henüz başkasının", error);
    REQUIRE(c);
    CHECK(c->verb == V::ExpectMessage);
    CHECK(c->text == "henüz başkasının");

    c = world::parseLootCommand("lootfarm Kül Çömezi 9 2", error);
    REQUIRE(c);
    CHECK(c->verb == V::Farm);
    CHECK(c->text == "Kül Çömezi");
    CHECK(c->a == 9);
    CHECK(c->b == 2);
    c = world::parseLootCommand("lootfarm Kül Çömezi rows 2", error);
    REQUIRE(c);
    CHECK(c->mode == "rows");
    CHECK(c->text == "Kül Çömezi");
    CHECK(c->b == 2);
    CHECK_FALSE(world::parseLootCommand("lootfarm Kül Çömezi rows 0", error));
    CHECK(error.find("rows <1..9>") != std::string::npos);
    c = world::parseLootCommand("lootfarm stop", error);
    REQUIRE(c);
    CHECK(c->mode == "stop");
    c = world::parseLootCommand("movestack 9 6", error);
    REQUIRE(c);
    CHECK(c->verb == V::MoveStack);
    CHECK(c->a == 9);
    CHECK(c->b == 6);

    // Malformed loot commands name the usage.
    const auto bad = [&](const char* line, const char* usage) {
        INFO(line);
        CHECK_FALSE(world::parseLootCommand(line, error));
        CHECK(error.find(usage) != std::string::npos);
    };
    bad("lootnearest mine", "lootnearest [any]");
    bad("walktoloot other", "walktoloot [mine|locked|free|any]");
    bad("takeall now", "takeall");
    bad("take", "take <entry index 0..7>|coins");
    bad("take 8", "take <entry index 0..7>|coins");
    bad("take x", "take <entry index 0..7>|coins");
    bad("expectcoins", "expectcoins");
    bad("expectcoins ~ 3", "expectcoins");
    bad("expectcoins >= x", "expectcoins");
    bad("expectitem 9", "expectitem <templateId>");
    bad("expectitem 0 1", "expectitem <templateId>");
    bad("expectitem 9 != 1", "expectitem <templateId>");
    bad("expectstack 28 9 1", "expectstack <bag slot 0..27>");
    bad("expectstack 3 9 0", "expectstack <bag slot 0..27>");
    bad("expectlootrows", "expectlootrows <n>");
    bad("expectlootmessage", "expectlootmessage <text>");
    bad("lootfarm Kül Çömezi 9", "lootfarm <monster name> <templateId> <pickups>");
    bad("lootfarm 9 2", "lootfarm <monster name>");
    bad("lootfarm Kül 0 2", "lootfarm <monster name>");
    bad("movestack 9", "movestack <templateId> <bag slot 0..27>");
    bad("movestack 9 28", "movestack <templateId> <bag slot 0..27>");

    CHECK(world::compare(Op::Ge, 5, 5));
    CHECK_FALSE(world::compare(Op::Gt, 5, 5));
    CHECK(world::compare(Op::Lt, 4, 5));
    CHECK(world::compare(Op::Le, 5, 5));
    CHECK(world::compare(Op::Eq, 5, 5));
}

TEST_CASE("Client inventory: a loot take's ITEM_UPDATEs settle no item move", "[client][loot][inventory]")
{
    using vo::client::world::ClientInventory;
    ClientInventory inv;
    inv.setRules(&catalog(), {kWarrior, 1});
    vn::Inventory msg;
    msg.items = {entry(vg::ItemPlace::equipment(vg::EquipSlot::MainHand), 101, kSword),
                 entry(vg::ItemPlace::equipment(vg::EquipSlot::OffHand), 102, kShield),
                 entry(vg::ItemPlace::bag(0), 103, kAshDust, 5), entry(vg::ItemPlace::bag(1), 104, kAshDust, 4)};
    std::string error;
    REQUIRE(inv.applyInventory(msg, error));

    // A take first, then a merge of bag 1 onto bag 0.
    inv.beginTake(7);
    CHECK(inv.takesPending() == 1);
    const auto b = inv.beginMove(vg::ItemPlace::bag(1), vg::ItemPlace::bag(0), 1.0);
    REQUIRE(b.request);
    CHECK(inv.view().at(vg::ItemPlace::bag(0)).quantity == 9); // the merge is predicted at once
    CHECK(inv.view().at(vg::ItemPlace::bag(1)).empty());
    // The take's update (a new stack in bag 2) comes first: the merge stays pending and shown.
    vn::ItemUpdate takeUpdate;
    takeUpdate.items = {entry(vg::ItemPlace::bag(2), 105, kEmberCore, 1)};
    REQUIRE(inv.applyUpdate(takeUpdate));
    CHECK(inv.view().at(vg::ItemPlace::bag(0)).quantity == 9);
    CHECK(inv.view().at(vg::ItemPlace::bag(1)).empty());
    CHECK(inv.view().at(vg::ItemPlace::bag(2)).templateId == kEmberCore);
    inv.endTake(7);
    inv.endTake(999); // unknown: ignored
    CHECK(inv.takesPending() == 0);
    // Then the merge's own update settles it; no double application.
    vn::ItemUpdate mergeUpdate;
    mergeUpdate.items = {entry(vg::ItemPlace::bag(0), 103, kAshDust, 9), entry(vg::ItemPlace::bag(1), 0, 0)};
    REQUIRE(inv.applyUpdate(mergeUpdate));
    CHECK(inv.view().at(vg::ItemPlace::bag(0)).quantity == 9);
    vn::ItemMoveRes res;
    res.seq = b.request->seq;
    res.result = vg::ItemMoveResult::Ok;
    REQUIRE(inv.applyResult(res));
    CHECK(inv.view() == inv.confirmed());
    CHECK(inv.view().at(vg::ItemPlace::bag(0)).quantity == 9);

    // A take requested after a move does not delay the move's settling.
    const auto b2 = inv.beginMove(vg::ItemPlace::bag(2), vg::ItemPlace::bag(5), 2.0);
    REQUIRE(b2.request);
    inv.beginTake(8);
    vn::ItemUpdate moveUpdate;
    moveUpdate.items = {entry(vg::ItemPlace::bag(5), 105, kEmberCore, 1), entry(vg::ItemPlace::bag(2), 0, 0)};
    REQUIRE(inv.applyUpdate(moveUpdate));
    res.seq = b2.request->seq;
    REQUIRE(inv.applyResult(res));
    CHECK(inv.view().at(vg::ItemPlace::bag(5)).templateId == kEmberCore);
    CHECK(inv.view().at(vg::ItemPlace::bag(2)).empty());
    inv.reset();
    CHECK(inv.takesPending() == 0);
}

TEST_CASE("World model: loot containers are entities of their own kind", "[client][loot][world]")
{
    world::WorldModel model;
    vn::EnterWorldRes enter;
    enter.entityId = 1;
    enter.zoneId = 1;
    model.enter(enter, nullptr, 0.0, true);
    vn::EntitySpawn bag;
    bag.entityId = 900;
    bag.entityType = vn::kEntityTypeLoot;
    bag.name = "Kül Çömezi";
    bag.level = 1;
    bag.templateId = 1;
    bag.x = 10.0f;
    bag.z = 20.0f;
    bag.state = vn::kEntityStateLootMine;
    model.handleSpawn(bag, 0.0);
    vn::EntitySpawn monster;
    monster.entityId = 50;
    monster.entityType = vn::kEntityTypeMonster;
    monster.name = "Kül Çömezi";
    monster.maxHp = 80;
    model.handleSpawn(monster, 0.0);
    REQUIRE(model.activeCount() == 2);
    CHECK(model.lootCount() == 1);
    CHECK(model.monsterCount() == 1);
    CHECK(model.playerCount() == 0);
    CHECK(model.stats().lootSpawns == 1);
    CHECK(model.stats().ignoredSpawns == 0);
    const world::RemoteEntity* e = model.find(900);
    REQUIRE(e != nullptr);
    CHECK(e->isLoot());
    CHECK_FALSE(e->isMonster());
    CHECK_FALSE(e->isPlayer());
    CHECK(e->position.x == 10.0f);
    CHECK(world::lootLabel(e->state) == LootLabel::Mine);
    model.handleDespawn({900});
    CHECK(model.lootCount() == 0);
    CHECK(model.stats().lootDespawns == 1);
    CHECK(model.monsterCount() == 1);
}
