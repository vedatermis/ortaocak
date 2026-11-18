// shared/game/Items: the gamedata/items.toml loader (strict validation) and the pure placement rules
// of docs/PROTOCOL_ITEMS.md §1.1/§1.2, on the real catalog (template ids are stable forever).

#include "shared/game/Character.h"
#include "shared/game/Items.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace vg = vo::game;
using R = vg::ItemMoveResult;
using vg::EquipSlot;
using vg::ItemPlace;

namespace {

// Template ids of gamedata/items.toml.
constexpr vg::ItemTemplateId kSword = 1;    // warrior main_hand L1
constexpr vg::ItemTemplateId kShield = 2;   // warrior off_hand L1
constexpr vg::ItemTemplateId kDagger = 3;   // rogue either_hand L1
constexpr vg::ItemTemplateId kStaff = 4;    // mage two_hand L1
constexpr vg::ItemTemplateId kMace = 5;     // priest main_hand L1
constexpr vg::ItemTemplateId kAxe = 6;      // warrior two_hand L1
constexpr vg::ItemTemplateId kSword3 = 7;   // warrior main_hand L3
constexpr vg::ItemTemplateId kStaff3 = 8;   // mage two_hand L3
constexpr vg::ItemTemplateId kHidden = 999; // not in the catalog

constexpr std::uint8_t kWarrior = static_cast<std::uint8_t>(vg::CharClass::Warrior);
constexpr std::uint8_t kRogue = static_cast<std::uint8_t>(vg::CharClass::Rogue);
constexpr std::uint8_t kMage = static_cast<std::uint8_t>(vg::CharClass::Mage);
constexpr std::uint8_t kPriest = static_cast<std::uint8_t>(vg::CharClass::Priest);

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

// Builds an inventory from (place, template) pairs; item ids are 100 + index.
vg::Inventory inventory(const std::vector<std::pair<ItemPlace, vg::ItemTemplateId>>& items)
{
    std::vector<vg::StoredItem> rows;
    for (std::size_t i = 0; i < items.size(); ++i)
        rows.push_back({100 + i, items[i].first, items[i].second, 1});
    vg::Inventory inv;
    std::string error;
    REQUIRE(vg::Inventory::fromItems(rows, inv, error));
    return inv;
}

// Plans and, when Ok, applies; an unchanged inventory is checked for every failure.
R move(vg::Inventory& inv, const vg::ItemUser& user, ItemPlace from, ItemPlace to, vg::ItemMovePlan* out = nullptr)
{
    const vg::Inventory before = inv;
    const auto plan = vg::planItemMove(catalog(), inv, user, from, to);
    if (out)
        *out = plan;
    vg::applyItemMove(inv, plan);
    if (plan.result != R::Ok)
    {
        CHECK(plan.changes.empty());
        CHECK(inv == before);
    }
    else
    {
        CHECK(inv.count() == before.count()); // nothing appears or disappears
    }
    return plan.result;
}

vg::ItemTemplateId at(const vg::Inventory& inv, ItemPlace p)
{
    return inv.at(p).templateId;
}

const vg::ItemUser warrior{kWarrior, 1};

// A minimal valid items.toml; tests replace parts of it.
std::string validToml(const std::string& extraItems = "", const std::string& warriorKit = "equip = { main_hand = 1 }")
{
    return R"([[item]]
id = 1
name = "Kılıç"
kind = "weapon"
slot = "main_hand"
classes = ["warrior"]
level = 1
attack = 6
defense = 0
model = "sword"
icon = "sword"
grip = "sword"

[[item]]
id = 2
name = "Kalkan"
kind = "shield"
slot = "off_hand"
classes = ["warrior", "priest"]
level = 1
attack = 0
defense = 4
model = "shield"
icon = "shield"
grip = "shield"

[[item]]
id = 3
name = "Balta"
kind = "weapon"
slot = "two_hand"
classes = ["warrior"]
level = 1
attack = 10
defense = 0
model = "axe"
icon = "axe"
grip = "axe2h"
)" + extraItems +
           R"(
[[starter_kit]]
class = "warrior"
)" + warriorKit +
           R"(

[[starter_kit]]
class = "rogue"

[[starter_kit]]
class = "mage"
bag = []

[[starter_kit]]
class = "priest"
equip = { off_hand = 2 }
)";
}

std::string parseError(const std::string& text)
{
    vg::ItemCatalog c;
    std::string error;
    if (vg::parseItemCatalog(text, "items.toml", c, error))
        return {};
    CHECK_FALSE(error.empty());
    return error;
}

std::string replaced(std::string s, const std::string& what, const std::string& with)
{
    const auto pos = s.find(what);
    REQUIRE(pos != std::string::npos);
    s.replace(pos, what.size(), with);
    return s;
}

} // namespace

// ---- Loader ----

TEST_CASE("Items: the repository's items.toml loads with every template and kit", "[game][items]")
{
    const auto& c = catalog();
    REQUIRE(c.size() >= 8);
    const vg::ItemTemplate* sword = c.find(kSword);
    REQUIRE(sword);
    CHECK(sword->name == "Er Kılıcı");
    CHECK(sword->kind == vg::ItemKind::Weapon);
    CHECK(sword->slot == vg::SlotRule::MainHand);
    CHECK(sword->allowsClass(kWarrior));
    CHECK_FALSE(sword->allowsClass(kMage));
    CHECK(sword->attack == 6);
    CHECK(sword->grip == vg::Grip::Sword);
    CHECK(sword->model == "sword_soldier");
    CHECK(c.find(kShield)->kind == vg::ItemKind::Shield);
    CHECK(c.find(kDagger)->slot == vg::SlotRule::EitherHand);
    CHECK(c.find(kStaff)->isTwoHanded());
    CHECK(c.find(kStaff3)->level == 3);
    CHECK(c.find(0) == nullptr);
    CHECK(c.find(kHidden) == nullptr);
    for (std::size_t i = 1; i < c.templates().size(); ++i)
        CHECK(c.templates()[i - 1].id < c.templates()[i].id);

    const vg::StarterKit* w = c.kitFor(kWarrior);
    REQUIRE(w);
    CHECK(w->equip[0] == kSword);
    CHECK(w->equip[1] == kShield);
    CHECK(w->bag == std::vector<vg::ItemTemplateId>{kAxe});
    const vg::StarterKit* r = c.kitFor(kRogue);
    REQUIRE(r);
    CHECK(r->equip[0] == kDagger);
    CHECK(r->equip[1] == kDagger);
    CHECK(c.kitFor(kMage)->equip[0] == kStaff);
    CHECK(c.kitFor(kMage)->bag == std::vector<vg::ItemTemplateId>{kStaff3});
    CHECK(c.kitFor(kPriest)->equip[0] == kMace);
    CHECK(c.kitFor(0) == nullptr);
    CHECK(c.kitFor(5) == nullptr);

    CHECK(vg::starterKitVisible(c, kWarrior) == vg::VisibleEquipment{kSword, kShield});
    CHECK(vg::starterKitVisible(c, kMage) == vg::VisibleEquipment{kStaff, 0});
    CHECK(vg::starterKitVisible(c, 9) == vg::VisibleEquipment{});
}

TEST_CASE("Items: the loader accepts a minimal file and rejects every kind of mistake", "[game][items]")
{
    {
        vg::ItemCatalog c;
        std::string error;
        REQUIRE(vg::parseItemCatalog(validToml(), "items.toml", c, error));
        CHECK(c.size() == 3);
        CHECK(c.kitFor(kRogue)->bag.empty());
        CHECK(c.find(2)->allowsClass(kPriest));
    }
    const std::string extra = R"(
[[item]]
id = 9
name = "X"
kind = "weapon"
slot = "main_hand"
classes = ["warrior"]
level = 1
attack = 1
defense = 0
model = "x"
icon = "x"
grip = "sword"
)";
    const std::string base = validToml(extra);
    REQUIRE(parseError(base).empty());

    CHECK(parseError("this is = = not toml").find("items.toml:1") != std::string::npos);
    CHECK(parseError(replaced(base, "id = 9", "id = 1")).find("duplicate item id 1") != std::string::npos);
    CHECK(parseError(replaced(base, "id = 9", "id = 0")).find("out of range") != std::string::npos);
    CHECK(parseError(replaced(base, "id = 9", "id = 65536")).find("out of range") != std::string::npos);
    CHECK(parseError(replaced(base, "id = 9", "id = \"9\"")).find("integer") != std::string::npos);
    CHECK(parseError(replaced(base, "name = \"X\"", "name = \"\"")).find("empty name") != std::string::npos);
    CHECK(parseError(replaced(base, "name = \"X\"\n", "")).find("missing 'name'") != std::string::npos);
    CHECK(parseError(replaced(extra + validToml(), "kind = \"weapon\"", "kind = \"potion\""))
              .find("unknown kind 'potion'") != std::string::npos);
    CHECK(parseError(replaced(base, "slot = \"main_hand\"\nclasses = [\"warrior\"]\nlevel = 1\nattack = 1",
                              "slot = \"head\"\nclasses = [\"warrior\"]\nlevel = 1\nattack = 1"))
              .find("slot 'head' does not fit kind 'weapon'") != std::string::npos); // M15: armour slots exist
    CHECK(parseError(replaced(base, "slot = \"main_hand\"\nclasses = [\"warrior\"]\nlevel = 1\nattack = 1",
                              "slot = \"neck\"\nclasses = [\"warrior\"]\nlevel = 1\nattack = 1"))
              .find("unknown slot 'neck'") != std::string::npos);
    CHECK(parseError(replaced(base, "classes = [\"warrior\"]\nlevel = 1\nattack = 1",
                              "classes = [\"paladin\"]\nlevel = 1\nattack = 1"))
              .find("unknown class 'paladin'") != std::string::npos);
    CHECK(parseError(replaced(base, "classes = [\"warrior\"]\nlevel = 1\nattack = 1",
                              "classes = []\nlevel = 1\nattack = 1"))
              .find("classes") != std::string::npos);
    CHECK(parseError(replaced(base, "classes = [\"warrior\"]\nlevel = 1\nattack = 1",
                              "classes = [\"warrior\", \"warrior\"]\nlevel = 1\nattack = 1"))
              .find("listed twice") != std::string::npos);
    CHECK(parseError(replaced(base, "level = 1\nattack = 1", "level = 0\nattack = 1")).find("out of range") !=
          std::string::npos);
    CHECK(parseError(replaced(base, "attack = 1\n", "attack = -1\n")).find("out of range") != std::string::npos);
    CHECK(parseError(replaced(base, "grip = \"sword\"\n\n[[starter", "grip = \"bow\"\n\n[[starter"))
              .find("unknown grip 'bow'") != std::string::npos);
    CHECK(parseError(replaced(base, "icon = \"x\"", "icon = \"x\"\ncolour = \"red\"")).find("unknown key 'colour'") !=
          std::string::npos);
    CHECK(parseError(base + "\n[shops]\nx = 1\n").find("unknown key 'shops'") != std::string::npos);
    CHECK(parseError(replaced(validToml(), "[[item]]\nid = 1", "[[item]]\nid = 1\nslot2 = 1"))
              .find("unknown key") != std::string::npos);

    // Starter kits.
    CHECK(parseError(validToml("", "equip = { main_hand = 42 }")).find("unknown item id 42") != std::string::npos);
    CHECK(parseError(validToml("", "bag = [42]")).find("unknown item id 42") != std::string::npos);
    CHECK(parseError(validToml("", "equip = { off_hand = 1 }")).find("cannot be equipped in off_hand (WrongSlot)") !=
          std::string::npos);
    CHECK(parseError(validToml("", "equip = { left_foot = 1 }")).find("unknown equipment slot 'left_foot'") !=
          std::string::npos);
    CHECK(parseError(validToml("", "equip = { head = 1 }")).find("WrongSlot") != std::string::npos);
    CHECK(parseError(validToml("", "equip = { main_hand = 3, off_hand = 2 }")).find("two-handed") !=
          std::string::npos);
    CHECK(parseError(replaced(validToml(), "class = \"priest\"\nequip = { off_hand = 2 }",
                              "class = \"priest\"\nequip = { main_hand = 1 }"))
              .find("WrongClass") != std::string::npos);
    CHECK(parseError(replaced(validToml(), "class = \"rogue\"", "class = \"bard\"")).find("unknown class 'bard'") !=
          std::string::npos);
    CHECK(parseError(replaced(validToml(), "class = \"rogue\"", "class = \"warrior\"")).find("duplicate starter kit") !=
          std::string::npos);
    CHECK(parseError(replaced(validToml(), "[[starter_kit]]\nclass = \"rogue\"\n", "")).find("no [[starter_kit]] for "
                                                                                             "class 'rogue'") !=
          std::string::npos);
    std::string tooMany = "bag = [";
    for (int i = 0; i < 29; ++i)
        tooMany += i ? ", 1" : "1";
    CHECK(parseError(validToml("", tooMany + "]")).find("do not fit") != std::string::npos);
    CHECK(parseError(validToml("", "colour = 1")).find("unknown key 'colour'") != std::string::npos);
    // A level requirement above the start level cannot be part of the equipped kit (fine in the bag).
    const std::string highLevel = replaced(extra, "level = 1", "level = 2");
    CHECK(parseError(validToml(highLevel, "equip = { main_hand = 9 }")).find("LevelTooLow") != std::string::npos);
    CHECK(parseError(validToml(highLevel, "bag = [9]")).empty());

    // Errors name the line.
    CHECK(parseError(replaced(base, "id = 9", "id = 1")).starts_with("items.toml:"));
    // A failed parse leaves the output unchanged.
    vg::ItemCatalog kept;
    std::string error;
    REQUIRE(vg::parseItemCatalog(validToml(), "a", kept, error));
    CHECK_FALSE(vg::parseItemCatalog("x", "b", kept, error));
    CHECK(kept.size() == 3);
    CHECK_FALSE(vg::loadItemCatalog("does/not/exist.toml", kept, error));
    CHECK(error.find("cannot open") != std::string::npos);
}

// ---- Inventory ----

TEST_CASE("Items: inventory from stored rows", "[game][items]")
{
    vg::Inventory inv;
    std::string error;
    CHECK(vg::Inventory::fromItems({{1, mainHand(), kSword, 1}, {2, bag(27), kAxe, 1}}, inv, error));
    CHECK(inv.count() == 2);
    CHECK(inv.items() == std::vector<vg::StoredItem>{{1, mainHand(), kSword, 1}, {2, bag(27), kAxe, 1}});
    CHECK(inv.firstFreeBagSlot() == 0);
    CHECK_FALSE(vg::Inventory::fromItems({{1, mainHand(), kSword, 1}, {2, mainHand(), kAxe, 1}}, inv, error));
    CHECK(error.find("share") != std::string::npos);
    CHECK_FALSE(vg::Inventory::fromItems({{1, {0, 8}, kSword, 1}}, inv, error));
    CHECK_FALSE(vg::Inventory::fromItems({{1, {1, 28}, kSword, 1}}, inv, error));
    CHECK_FALSE(vg::Inventory::fromItems({{1, {2, 0}, kSword, 1}}, inv, error));
    CHECK_FALSE(vg::Inventory::fromItems({{0, bag(0), kSword, 1}}, inv, error));
    CHECK_FALSE(vg::Inventory::fromItems({{1, bag(0), kSword, 0}}, inv, error));
    CHECK(inv.count() == 2); // unchanged by the failures
    // Unknown templates are accepted (hidden) and not shown as visible equipment.
    REQUIRE(vg::Inventory::fromItems({{1, mainHand(), kHidden, 1}, {2, offHand(), kShield, 1}}, inv, error));
    CHECK(vg::visibleEquipment(catalog(), inv) == vg::VisibleEquipment{0, kShield});
}

// ---- Rules (§1.1, §1.2) ----

TEST_CASE("Items: checkEquip applies slot rule, class and level", "[game][items]")
{
    const auto& c = catalog();
    CHECK(vg::checkEquip(*c.find(kSword), EquipSlot::MainHand, warrior) == R::Ok);
    CHECK(vg::checkEquip(*c.find(kSword), EquipSlot::OffHand, warrior) == R::WrongSlot);
    CHECK(vg::checkEquip(*c.find(kShield), EquipSlot::MainHand, warrior) == R::WrongSlot);
    CHECK(vg::checkEquip(*c.find(kShield), EquipSlot::OffHand, warrior) == R::Ok);
    CHECK(vg::checkEquip(*c.find(kDagger), EquipSlot::MainHand, {kRogue, 1}) == R::Ok);
    CHECK(vg::checkEquip(*c.find(kDagger), EquipSlot::OffHand, {kRogue, 1}) == R::Ok);
    CHECK(vg::checkEquip(*c.find(kStaff), EquipSlot::MainHand, {kMage, 1}) == R::Ok);
    CHECK(vg::checkEquip(*c.find(kStaff), EquipSlot::OffHand, {kMage, 1}) == R::WrongSlot);
    for (const auto s : {EquipSlot::Head, EquipSlot::Chest, EquipSlot::Legs, EquipSlot::Hands, EquipSlot::Feet,
                         EquipSlot::Reserved})
        CHECK(vg::checkEquip(*c.find(kSword), s, warrior) == R::WrongSlot);
    CHECK(vg::checkEquip(*c.find(kDagger), EquipSlot::MainHand, warrior) == R::WrongClass);
    CHECK(vg::checkEquip(*c.find(kMace), EquipSlot::MainHand, warrior) == R::WrongClass);
    CHECK(vg::checkEquip(*c.find(kSword3), EquipSlot::MainHand, warrior) == R::LevelTooLow);
    CHECK(vg::checkEquip(*c.find(kSword3), EquipSlot::MainHand, {kWarrior, 2}) == R::LevelTooLow);
    CHECK(vg::checkEquip(*c.find(kSword3), EquipSlot::MainHand, {kWarrior, 3}) == R::Ok);
    // Slot rule before class before level.
    CHECK(vg::checkEquip(*c.find(kStaff3), EquipSlot::OffHand, warrior) == R::WrongSlot);
    CHECK(vg::checkEquip(*c.find(kStaff3), EquipSlot::MainHand, warrior) == R::WrongClass);
}

TEST_CASE("Items: attribute bonuses and requirements (M11, PROTOCOL_STATS.md section 5)", "[game][items][stats]")
{
    // The shipped data: Cüruf Topuzu İrade +2, Bekçi Kalkanı Dayanıklılık +3, no requirements.
    const auto& c = catalog();
    CHECK(c.find(14)->attributes == vg::Attributes{0, 0, 0, 0, 2});
    CHECK(c.find(15)->attributes == vg::Attributes{0, 0, 3, 0, 0});
    for (const auto& t : c.templates())
        CHECK_FALSE(t.hasRequirements());
    vg::Inventory inv = inventory({{mainHand(), 13}, {offHand(), 15}});
    const vg::EquipBonus b = vg::equipmentBonus(c, inv);
    CHECK(b.attack == 7);
    CHECK(b.defense == 7);
    CHECK(b.attributes == vg::Attributes{0, 0, 3, 0, 0});

    // A template with bonuses and requirements.
    const std::string extra = R"(
[[item]]
id = 9
name = "Ağır Gürz"
kind = "weapon"
slot = "main_hand"
classes = ["warrior"]
level = 1
attack = 9
defense = 0
model = "x"
icon = "x"
grip = "mace"
strength = 3
vitality = 1
req_strength = 20
req_vitality = 12
)";
    vg::ItemCatalog parsed;
    std::string error;
    INFO(error);
    REQUIRE(vg::parseItemCatalog(validToml(extra), "items.toml", parsed, error));
    const vg::ItemTemplate& t = *parsed.find(9);
    CHECK(t.attributes == vg::Attributes{3, 0, 1, 0, 0});
    CHECK(t.requirements == vg::Attributes{20, 0, 12, 0, 0});
    CHECK(t.hasRequirements());
    // Requirements are checked against the base values (class start + allocated), after the level.
    const vg::ItemUser weak{kWarrior, 5, vg::baseAttributes(kWarrior, {})};          // Güç 12
    const vg::ItemUser strong{kWarrior, 5, vg::baseAttributes(kWarrior, {8, 0, 1, 0, 0})}; // Güç 20, VIT 12
    const vg::ItemUser almost{kWarrior, 5, vg::baseAttributes(kWarrior, {8, 0, 0, 0, 0})}; // VIT 11
    CHECK(vg::checkEquip(t, EquipSlot::MainHand, weak) == R::AttributeTooLow);
    CHECK(vg::checkEquip(t, EquipSlot::MainHand, almost) == R::AttributeTooLow);
    CHECK(vg::checkEquip(t, EquipSlot::MainHand, strong) == R::Ok);
    CHECK(vg::checkEquip(t, EquipSlot::OffHand, weak) == R::WrongSlot); // slot rule first
    CHECK(vg::checkEquip(t, EquipSlot::MainHand, {kRogue, 5, vg::baseAttributes(kRogue, {20, 0, 9, 0, 0})}) ==
          R::WrongClass);
    // ... and they block a move into the slot (planItemMove), never a move out of it.
    vg::Inventory bagged;
    std::string why;
    REQUIRE(vg::Inventory::fromItems({{1, bag(0), 9, 1}}, bagged, why));
    CHECK(vg::planItemMove(parsed, bagged, weak, bag(0), mainHand()).result == R::AttributeTooLow);
    CHECK(vg::planItemMove(parsed, bagged, strong, bag(0), mainHand()).result == R::Ok);
    vg::Inventory worn;
    REQUIRE(vg::Inventory::fromItems({{1, mainHand(), 9, 1}}, worn, why));
    CHECK(vg::planItemMove(parsed, worn, weak, mainHand(), bag(0)).result == R::Ok);

    // Strictness: ranges, materials, starter kits.
    CHECK(parseError(validToml(replaced(extra, "strength = 3", "strength = 51"))).find("out of range") !=
          std::string::npos);
    CHECK(parseError(validToml(replaced(extra, "req_strength = 20", "req_strength = 256"))).find("out of range") !=
          std::string::npos);
    CHECK(parseError(validToml(replaced(extra, "strength = 3", "strength = -1"))).find("out of range") !=
          std::string::npos);
    CHECK(parseError(validToml(replaced(extra, "strength = 3", "luck = 3"))).find("unknown key 'luck'") !=
          std::string::npos);
    const std::string material = "\n[[item]]\nid = 9\nname = \"Toz\"\nkind = \"material\"\nicon = \"x\"\nspirit = 1\n";
    CHECK(parseError(validToml(material)).find("not allowed for a material") != std::string::npos);
    // A kit item with a requirement cannot be equipped by a fresh character (kits are checked
    // with no attributes).
    CHECK_FALSE(parseError(validToml(extra, "equip = { main_hand = 9 }")).empty());
}

TEST_CASE("Items: bag moves and swaps", "[game][items]")
{
    auto inv = inventory({{bag(0), kSword}, {bag(5), kAxe}});
    vg::ItemMovePlan plan;
    // Move to an empty bag slot.
    REQUIRE(move(inv, warrior, bag(0), bag(3), &plan) == R::Ok);
    CHECK(at(inv, bag(0)) == 0);
    CHECK(at(inv, bag(3)) == kSword);
    REQUIRE(plan.changes.size() == 2);
    CHECK(plan.changes[0].place == bag(3)); // `to` first
    CHECK(plan.changes[0].content.id == 100);
    CHECK(plan.changes[1].place == bag(0));
    CHECK(plan.changes[1].content.empty());
    // Swap two bag items; class and level do not matter in the bag.
    REQUIRE(move(inv, {kMage, 1}, bag(3), bag(5), &plan) == R::Ok);
    CHECK(at(inv, bag(3)) == kAxe);
    CHECK(at(inv, bag(5)) == kSword);
    CHECK(plan.changes.size() == 2);
}

TEST_CASE("Items: equip, unequip and swap with equipment", "[game][items]")
{
    vg::ItemMovePlan plan;
    SECTION("equip into an empty slot and unequip to an empty bag slot")
    {
        auto inv = inventory({{bag(2), kSword}});
        REQUIRE(move(inv, warrior, bag(2), mainHand(), &plan) == R::Ok);
        CHECK(at(inv, mainHand()) == kSword);
        CHECK(at(inv, bag(2)) == 0);
        CHECK(vg::visibleEquipment(catalog(), inv) == vg::VisibleEquipment{kSword, 0});
        REQUIRE(move(inv, warrior, mainHand(), bag(9)) == R::Ok);
        CHECK(at(inv, bag(9)) == kSword);
        CHECK(at(inv, mainHand()) == 0);
    }
    SECTION("equip onto an occupied slot swaps")
    {
        auto inv = inventory({{mainHand(), kSword}, {bag(4), kSword3}});
        REQUIRE(move(inv, {kWarrior, 3}, bag(4), mainHand(), &plan) == R::Ok);
        CHECK(at(inv, mainHand()) == kSword3);
        CHECK(at(inv, bag(4)) == kSword);
        const auto rel = plan.relocations(inventory({{mainHand(), kSword}, {bag(4), kSword3}}));
        REQUIRE(rel.size() == 2);
        CHECK(rel[0] == vg::ItemMovePlan::Relocation{101, bag(4), mainHand()});
        CHECK(rel[1] == vg::ItemMovePlan::Relocation{100, mainHand(), bag(4)});
    }
    SECTION("unequip onto an occupied bag slot swaps when the bag item may take the slot")
    {
        auto inv = inventory({{mainHand(), kSword}, {bag(1), kSword3}});
        REQUIRE(move(inv, {kWarrior, 3}, mainHand(), bag(1)) == R::Ok);
        CHECK(at(inv, mainHand()) == kSword3);
        CHECK(at(inv, bag(1)) == kSword);
    }
    SECTION("unequip onto a bag item that may not take the slot fails")
    {
        auto inv = inventory({{mainHand(), kSword}, {bag(1), kShield}, {bag(2), kSword3}, {bag(3), kDagger}});
        CHECK(move(inv, warrior, mainHand(), bag(1)) == R::WrongSlot);   // a shield cannot go to MainHand
        CHECK(move(inv, warrior, mainHand(), bag(2)) == R::LevelTooLow); // level 3 sword, level 1 warrior
        CHECK(move(inv, warrior, mainHand(), bag(3)) == R::WrongClass);  // a rogue's dagger
    }
    SECTION("MainHand <-> OffHand: two daggers swap, a sword cannot go to OffHand")
    {
        auto inv = inventory({{mainHand(), kDagger}, {offHand(), kDagger}});
        REQUIRE(move(inv, {kRogue, 1}, mainHand(), offHand(), &plan) == R::Ok);
        CHECK(inv.at(mainHand()).id == 101);
        CHECK(inv.at(offHand()).id == 100);
        CHECK(plan.changes.size() == 2);
        auto one = inventory({{mainHand(), kDagger}});
        REQUIRE(move(one, {kRogue, 1}, mainHand(), offHand()) == R::Ok); // either_hand moves over
        CHECK(at(one, offHand()) == kDagger);
        auto w = inventory({{mainHand(), kSword}, {offHand(), kShield}});
        CHECK(move(w, warrior, mainHand(), offHand()) == R::WrongSlot);
        CHECK(move(w, warrior, offHand(), mainHand()) == R::WrongSlot);
    }
    SECTION("WrongSlot, WrongClass, LevelTooLow when equipping")
    {
        auto inv = inventory({{bag(0), kShield}, {bag(1), kSword}, {bag(2), kDagger}, {bag(3), kSword3}, {bag(4), kStaff}});
        CHECK(move(inv, warrior, bag(0), mainHand()) == R::WrongSlot);
        CHECK(move(inv, warrior, bag(1), offHand()) == R::WrongSlot);
        CHECK(move(inv, warrior, bag(1), ItemPlace::equipment(EquipSlot::Head)) == R::WrongSlot);
        CHECK(move(inv, warrior, bag(1), ItemPlace::equipment(EquipSlot::Reserved)) == R::WrongSlot);
        CHECK(move(inv, warrior, bag(2), mainHand()) == R::WrongClass);
        CHECK(move(inv, warrior, bag(4), mainHand()) == R::WrongClass);
        CHECK(move(inv, warrior, bag(3), mainHand()) == R::LevelTooLow);
        CHECK(move(inv, {kWarrior, 3}, bag(3), mainHand()) == R::Ok);
    }
}

TEST_CASE("Items: two-handed weapons and the off hand", "[game][items]")
{
    vg::ItemMovePlan plan;
    SECTION("equipping a two-hander moves the off-hand item to the lowest free bag slot")
    {
        const auto before = inventory({{mainHand(), kSword}, {offHand(), kShield}, {bag(0), kAxe}});
        auto inv = before;
        REQUIRE(move(inv, warrior, bag(0), mainHand(), &plan) == R::Ok);
        CHECK(at(inv, mainHand()) == kAxe);
        CHECK(at(inv, bag(0)) == kSword); // swapped into the slot the axe left
        CHECK(at(inv, offHand()) == 0);
        CHECK(at(inv, bag(1)) == kShield); // lowest free slot
        CHECK(vg::visibleEquipment(catalog(), inv) == vg::VisibleEquipment{kAxe, 0});
        REQUIRE(plan.changes.size() == 4);
        CHECK(plan.changes[0].place == mainHand());
        CHECK(plan.changes[1].place == bag(0));
        CHECK(plan.changes[2] == vg::ItemSlotChange{offHand(), {}});
        CHECK(plan.changes[3].place == bag(1));
        CHECK(plan.changes[3].content.id == 101);
        const auto rel = plan.relocations(before);
        REQUIRE(rel.size() == 3);
        CHECK(rel[0] == vg::ItemMovePlan::Relocation{102, bag(0), mainHand()});
        CHECK(rel[1] == vg::ItemMovePlan::Relocation{100, mainHand(), bag(0)});
        CHECK(rel[2] == vg::ItemMovePlan::Relocation{101, offHand(), bag(1)});
    }
    SECTION("the slot the two-hander left counts as free")
    {
        std::vector<std::pair<ItemPlace, vg::ItemTemplateId>> items{{offHand(), kShield}};
        for (std::uint8_t s = 0; s < vg::kBagSlots; ++s)
            items.push_back({bag(s), s == 5 ? kAxe : kSword3});
        auto inv = inventory(items); // MainHand empty, bag full
        REQUIRE(move(inv, warrior, bag(5), mainHand(), &plan) == R::Ok);
        CHECK(at(inv, mainHand()) == kAxe);
        CHECK(at(inv, bag(5)) == kShield);
        CHECK(at(inv, offHand()) == 0);
        CHECK(plan.changes.size() == 4);
    }
    SECTION("BagFull: no free bag slot for the off-hand item, nothing changes")
    {
        std::vector<std::pair<ItemPlace, vg::ItemTemplateId>> items{{mainHand(), kSword}, {offHand(), kShield}};
        for (std::uint8_t s = 0; s < vg::kBagSlots; ++s)
            items.push_back({bag(s), s == 0 ? kAxe : kSword3});
        auto inv = inventory(items);
        CHECK(move(inv, warrior, bag(0), mainHand()) == R::BagFull);
    }
    SECTION("SlotBlocked: nothing goes into OffHand next to a two-hander")
    {
        auto inv = inventory({{mainHand(), kAxe}, {bag(0), kShield}});
        CHECK(move(inv, warrior, bag(0), offHand()) == R::SlotBlocked);
        auto mage = inventory({{mainHand(), kStaff}, {bag(0), kDagger}});
        CHECK(move(mage, {kRogue, 1}, bag(0), offHand()) == R::SlotBlocked);
    }
    SECTION("unequipping the off hand onto a two-hander cannot put it in OffHand")
    {
        auto inv = inventory({{mainHand(), kSword}, {offHand(), kShield}, {bag(0), kAxe}});
        CHECK(move(inv, warrior, offHand(), bag(0)) == R::WrongSlot);
        CHECK(move(inv, warrior, mainHand(), offHand()) == R::WrongSlot);
    }
    SECTION("unequipping the main hand onto a two-hander in the bag also clears the off hand")
    {
        auto inv = inventory({{mainHand(), kSword}, {offHand(), kShield}, {bag(3), kAxe}});
        REQUIRE(move(inv, warrior, mainHand(), bag(3), &plan) == R::Ok);
        CHECK(at(inv, mainHand()) == kAxe);
        CHECK(at(inv, bag(3)) == kSword);
        CHECK(at(inv, offHand()) == 0);
        CHECK(at(inv, bag(0)) == kShield);
        CHECK(plan.changes.size() == 4);
    }
    SECTION("a two-hander with an empty off hand needs nothing else")
    {
        auto inv = inventory({{bag(0), kStaff}});
        REQUIRE(move(inv, {kMage, 1}, bag(0), mainHand(), &plan) == R::Ok);
        CHECK(plan.changes.size() == 2);
        REQUIRE(move(inv, {kMage, 1}, mainHand(), bag(0)) == R::Ok);
    }
}

TEST_CASE("Items: bad requests change nothing", "[game][items]")
{
    auto inv = inventory({{mainHand(), kSword}, {offHand(), kShield}, {bag(0), kAxe}});
    CHECK(move(inv, warrior, mainHand(), mainHand()) == R::BadSlot);
    CHECK(move(inv, warrior, bag(0), bag(0)) == R::BadSlot);
    CHECK(move(inv, warrior, {2, 0}, bag(1)) == R::BadSlot);
    CHECK(move(inv, warrior, bag(0), {2, 0}) == R::BadSlot);
    CHECK(move(inv, warrior, {0, 8}, bag(1)) == R::BadSlot);
    CHECK(move(inv, warrior, bag(28), bag(1)) == R::BadSlot);
    CHECK(move(inv, warrior, bag(0), bag(255)) == R::BadSlot);
    CHECK(move(inv, warrior, {255, 255}, {255, 255}) == R::BadSlot);
    CHECK(move(inv, warrior, bag(1), bag(2)) == R::NoItem);
    CHECK(move(inv, warrior, bag(1), mainHand()) == R::NoItem);
    CHECK(move(inv, warrior, ItemPlace::equipment(EquipSlot::Head), bag(2)) == R::NoItem);
    // Out of range wins over an empty `from`.
    CHECK(move(inv, warrior, bag(1), bag(28)) == R::BadSlot);
}

TEST_CASE("Items: hidden items (unknown templates) are never touched", "[game][items]")
{
    auto inv = inventory({{bag(0), kHidden}, {bag(1), kSword}, {offHand(), kHidden}, {bag(2), kAxe}});
    CHECK(move(inv, warrior, bag(0), bag(5)) == R::NoItem);      // looks empty to the client
    CHECK(move(inv, warrior, bag(1), bag(0)) == R::SlotBlocked); // occupied by something invisible
    CHECK(move(inv, warrior, bag(2), mainHand()) == R::SlotBlocked); // would have to move the hidden off-hand item
    CHECK(inv.firstFreeBagSlot() == 3);
    REQUIRE(move(inv, warrior, bag(1), mainHand()) == R::Ok);
    CHECK(vg::visibleEquipment(catalog(), inv) == vg::VisibleEquipment{kSword, 0});
}

TEST_CASE("Items: quick move target (double click)", "[game][items]")
{
    const auto& c = catalog();
    auto inv = inventory({{mainHand(), kSword}, {bag(0), kShield}, {bag(1), kAxe}, {bag(2), kDagger}, {bag(4), kHidden}});
    CHECK(vg::quickMoveTarget(c, inv, bag(0)) == offHand());
    CHECK(vg::quickMoveTarget(c, inv, bag(1)) == mainHand());
    CHECK(vg::quickMoveTarget(c, inv, bag(2)) == offHand()); // either_hand, MainHand taken, OffHand free
    CHECK(vg::quickMoveTarget(c, inv, mainHand()) == bag(3));
    CHECK_FALSE(vg::quickMoveTarget(c, inv, bag(3)).has_value());
    CHECK_FALSE(vg::quickMoveTarget(c, inv, bag(4)).has_value());
    CHECK_FALSE(vg::quickMoveTarget(c, inv, {7, 7}).has_value());
    auto empty = inventory({{bag(0), kDagger}});
    CHECK(vg::quickMoveTarget(c, empty, bag(0)) == mainHand());
}

TEST_CASE("Items: starter kit placement", "[game][items]")
{
    const vg::StarterKit& kit = *catalog().kitFor(kWarrior);
    std::size_t dropped = 9;
    const auto fresh = vg::starterKitItems(kit, {}, &dropped);
    CHECK(dropped == 0);
    CHECK(fresh == std::vector<vg::NewItem>{{mainHand(), kSword, 1}, {offHand(), kShield, 1}, {bag(0), kAxe, 1}});

    // An item already in MainHand (and bag 0): the kit's sword goes to the bag instead.
    const auto around = vg::starterKitItems(kit, {{7, mainHand(), kSword3, 1}, {8, bag(0), kSword3, 1}}, &dropped);
    CHECK(dropped == 0);
    CHECK(around == std::vector<vg::NewItem>{{bag(1), kSword, 1}, {offHand(), kShield, 1}, {bag(2), kAxe, 1}});

    // Full bag: what does not fit is left out.
    std::vector<vg::StoredItem> full;
    for (std::uint8_t s = 0; s < vg::kBagSlots; ++s)
        full.push_back({100u + s, bag(s), kSword3, 1});
    full.push_back({200, mainHand(), kSword3, 1});
    const auto none = vg::starterKitItems(kit, full, &dropped);
    CHECK(none == std::vector<vg::NewItem>{{offHand(), kShield, 1}});
    CHECK(dropped == 2);

    const auto rogue = vg::starterKitItems(*catalog().kitFor(kRogue), {});
    CHECK(rogue == std::vector<vg::NewItem>{{mainHand(), kDagger, 1}, {offHand(), kDagger, 1}});
}
