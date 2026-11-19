// M10 "Ganimet" shared rules (docs/PROTOCOL_LOOT.md): materials and the new keys of items.toml, the
// stack rules (merge moves, adding units: stacks first, then free slots), the strict
// gamedata/loot.toml loader, the shipped tables and the roll distribution with a seeded generator.

#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace vg = vo::game;
using R = vg::ItemMoveResult;
using vg::ItemPlace;

namespace {

constexpr vg::ItemTemplateId kSword = 1;
constexpr vg::ItemTemplateId kAsh = 9;    // Kül Tozu, max_stack 20
constexpr vg::ItemTemplateId kSlag = 10;  // Cüruf Parçası, 20
constexpr vg::ItemTemplateId kCore = 11;  // Kor Çekirdeği, 5
constexpr vg::ItemTemplateId kDagger2 = 12;
constexpr vg::ItemTemplateId kShield5 = 15;

const vg::ItemCatalog& catalog()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        const auto path = vg::locateItemsFile();
        REQUIRE(path.has_value());
        std::string error;
        INFO(error);
        REQUIRE(vg::loadItemCatalog(*path, out, error));
        return out;
    }();
    return c;
}

const vg::MonsterData& monsters()
{
    static const vg::MonsterData m = [] {
        vg::MonsterData out;
        const auto path = vg::locateMonstersFile();
        REQUIRE(path.has_value());
        std::string error;
        INFO(error);
        REQUIRE(vg::loadMonsterData(*path, out, error));
        return out;
    }();
    return m;
}

struct Row
{
    ItemPlace place;
    vg::ItemTemplateId templateId = 0;
    std::uint16_t quantity = 1;
};

vg::Inventory inventory(const std::vector<Row>& rows)
{
    std::vector<vg::StoredItem> items;
    for (std::size_t i = 0; i < rows.size(); ++i)
        items.push_back({100 + i, rows[i].place, rows[i].templateId, rows[i].quantity});
    vg::Inventory inv;
    std::string error;
    REQUIRE(vg::Inventory::fromItems(items, inv, error));
    return inv;
}

const vg::ItemUser warrior{1, 5};

std::string lootError(const std::string& text)
{
    vg::LootTables t;
    std::string error;
    if (vg::parseLootTables(text, "loot.toml", catalog(), &monsters(), t, error))
        return {};
    CHECK_FALSE(error.empty());
    return error;
}

bool contains(const std::string& s, const std::string& part)
{
    return s.find(part) != std::string::npos;
}

std::string itemsError(const std::string& extra)
{
    const std::string base = R"([[item]]
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
)" + extra + R"(
[[starter_kit]]
class = "warrior"
[[starter_kit]]
class = "rogue"
[[starter_kit]]
class = "mage"
[[starter_kit]]
class = "priest"
)";
    vg::ItemCatalog c;
    std::string error;
    if (vg::parseItemCatalog(base, "items.toml", c, error))
        return {};
    CHECK_FALSE(error.empty());
    return error;
}

} // namespace

// ---- Items ----

TEST_CASE("Items: the M10 templates - materials stack, ids 1-8 unchanged, stats over the starter kit", "[items][loot]")
{
    const auto& c = catalog();
    REQUIRE(c.size() == 168); // M14: the four potions 16-19 (tests/game/ShopTests.cpp); M15: ids 20-165; M16: stones 166-168
    for (const vg::ItemTemplateId id : {kAsh, kSlag, kCore})
    {
        const vg::ItemTemplate* t = c.find(id);
        REQUIRE(t);
        CHECK(t->kind == vg::ItemKind::Material);
        CHECK(t->slot == vg::SlotRule::None);
        CHECK(t->classMask == 0);
        CHECK(t->model.empty());
        CHECK(t->isStackable());
    }
    CHECK(c.find(kAsh)->maxStack == 20);
    CHECK(c.find(kSlag)->maxStack == 20);
    CHECK(c.find(kCore)->maxStack == 5);
    CHECK(c.find(kCore)->rarity == vg::Rarity::Rare);
    CHECK(c.find(kAsh)->icon == "ash_dust");
    CHECK(c.find(kSlag)->icon == "slag_shard");
    CHECK(c.find(kCore)->icon == "ember_core");
    // Weapons and shields: unstackable, models reused, icons fixed for the art pipeline.
    const std::map<vg::ItemTemplateId, std::pair<std::string, std::string>> art = {
        {12, {"dagger_hunter", "dagger_thrall"}},
        {13, {"sword_soldier", "sword_charred"}},
        {14, {"mace_sun", "mace_slag"}},
        {15, {"shield_oak", "shield_warden"}}};
    for (const auto& [id, mi] : art)
    {
        const vg::ItemTemplate* t = c.find(id);
        REQUIRE(t);
        CHECK(t->maxStack == 1);
        CHECK(t->model == mi.first);
        CHECK(t->icon == mi.second);
    }
    // Modest upgrades: better than the class's starter weapon, not beyond the level-3 items.
    CHECK(c.find(12)->attack > c.find(3)->attack);
    CHECK(c.find(13)->attack > c.find(1)->attack);
    CHECK(c.find(13)->attack < c.find(7)->attack);
    CHECK(c.find(14)->attack > c.find(5)->attack);
    CHECK(c.find(15)->defense > c.find(2)->defense);
    CHECK(c.find(12)->level == 2);
    CHECK(c.find(13)->level == 2);
    CHECK(c.find(14)->level == 4);
    CHECK(c.find(15)->level == 5);
    CHECK(c.find(15)->allowsClass(1));
    CHECK(c.find(15)->allowsClass(4));
    // Ids 1-8 kept their M8 values.
    CHECK(c.find(1)->name == "Er Kılıcı");
    CHECK(c.find(8)->name == "Kırağı Asası");
    for (vg::ItemTemplateId id = 1; id <= 8; ++id)
    {
        CHECK(c.find(id)->maxStack == 1);
        CHECK(c.find(id)->value >= 10); // M14: every item has a price now (PROTOCOL_SHOP.md §2.5)
        CHECK(c.find(id)->rarity == vg::Rarity::Common);
    }
    CHECK(vg::itemKindDisplayName(vg::ItemKind::Material) == "Malzeme");
    CHECK(vg::rarityDisplayName(vg::Rarity::Rare) == "Nadir");
}

TEST_CASE("Items: materials and the M10 keys are validated strictly", "[items][loot]")
{
    const std::string material = R"(
[[item]]
id = 2
name = "Toz"
kind = "material"
icon = "dust"
max_stack = 20
value = 3
rarity = "uncommon"
)";
    CHECK(itemsError(material).empty());
    CHECK(contains(itemsError(material + "slot = \"main_hand\"\n"), "'slot' is not allowed for a material"));
    CHECK(contains(itemsError(material + "classes = [\"warrior\"]\n"), "'classes' is not allowed"));
    CHECK(contains(itemsError(material + "model = \"x\"\n"), "'model' is not allowed"));
    CHECK(contains(itemsError(material + "attack = 1\n"), "'attack' is not allowed"));
    CHECK(contains(itemsError(R"(
[[item]]
id = 2
name = "Toz"
kind = "material"
)"),
                   "missing 'icon'"));
    CHECK(contains(itemsError(R"(
[[item]]
id = 2
name = "Toz"
kind = "material"
icon = "d"
max_stack = 1000
)"),
                   "out of range"));
    CHECK(contains(itemsError(R"(
[[item]]
id = 2
name = "Toz"
kind = "material"
icon = "d"
rarity = "epic"
)"),
                   "unknown rarity 'epic'"));
    // A weapon cannot stack.
    CHECK(contains(itemsError(R"(
[[item]]
id = 2
name = "Hançer"
kind = "weapon"
slot = "either_hand"
classes = ["rogue"]
level = 1
attack = 1
defense = 0
model = "d"
icon = "d"
grip = "dagger"
max_stack = 5
)"),
                   "cannot stack"));
}

TEST_CASE("Items: materials cannot be equipped; quick moves leave them in the bag", "[items][loot]")
{
    auto inv = inventory({{ItemPlace::bag(0), kAsh, 3}});
    CHECK(vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(0), ItemPlace::equipment(vg::EquipSlot::MainHand))
              .result == R::WrongSlot);
    CHECK(vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(0), ItemPlace::equipment(vg::EquipSlot::OffHand))
              .result == R::WrongSlot);
    CHECK_FALSE(vg::quickMoveTarget(catalog(), inv, ItemPlace::bag(0)).has_value());
    CHECK(vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(0), ItemPlace::bag(5)).result == R::Ok);
}

TEST_CASE("Items: a bag move onto a stack of the same material merges; a full stack or another item swaps",
          "[items][loot]")
{
    // Partial merge: 15 + 8 -> 20 at the target, 3 stay at the source.
    auto inv = inventory({{ItemPlace::bag(0), kAsh, 8}, {ItemPlace::bag(1), kAsh, 15}});
    auto plan = vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(0), ItemPlace::bag(1));
    REQUIRE(plan.result == R::Ok);
    REQUIRE(plan.isMerge());
    CHECK(plan.relocations(inv).empty());
    CHECK(plan.merge == std::vector<vg::ItemQuantityChange>{{101, ItemPlace::bag(1), 15, 20}, {100, ItemPlace::bag(0), 8, 3}});
    REQUIRE(plan.changes.size() == 2);
    CHECK(plan.changes[0] == vg::ItemSlotChange{ItemPlace::bag(1), {101, kAsh, 20}});
    CHECK(plan.changes[1] == vg::ItemSlotChange{ItemPlace::bag(0), {100, kAsh, 3}});
    vg::applyItemMove(inv, plan);
    CHECK(inv.at(ItemPlace::bag(1)).quantity == 20);
    CHECK(inv.at(ItemPlace::bag(0)).quantity == 3);

    // The target is full now: a plain swap.
    plan = vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(0), ItemPlace::bag(1));
    REQUIRE(plan.result == R::Ok);
    CHECK_FALSE(plan.isMerge());
    CHECK(plan.relocations(inv).size() == 2);

    // Complete merge: the source disappears (row deleted).
    inv = inventory({{ItemPlace::bag(3), kCore, 2}, {ItemPlace::bag(4), kCore, 3}});
    plan = vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(3), ItemPlace::bag(4));
    REQUIRE(plan.isMerge());
    CHECK(plan.merge == std::vector<vg::ItemQuantityChange>{{101, ItemPlace::bag(4), 3, 5}, {100, ItemPlace::bag(3), 2, 0}});
    vg::applyItemMove(inv, plan);
    CHECK(inv.at(ItemPlace::bag(3)).empty());
    CHECK(inv.at(ItemPlace::bag(4)) == vg::ItemStack{101, kCore, 5});
    CHECK(inv.count() == 1);

    // Different materials, or weapons of one template: swaps, never merges.
    inv = inventory({{ItemPlace::bag(0), kAsh, 2}, {ItemPlace::bag(1), kSlag, 2}, {ItemPlace::bag(2), kSword},
                     {ItemPlace::bag(3), kSword}});
    CHECK_FALSE(vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(0), ItemPlace::bag(1)).isMerge());
    CHECK_FALSE(vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(2), ItemPlace::bag(3)).isMerge());
    // A stored stack above max_stack never grows.
    inv = inventory({{ItemPlace::bag(0), kCore, 1}, {ItemPlace::bag(1), kCore, 9}});
    CHECK_FALSE(vg::planItemMove(catalog(), inv, warrior, ItemPlace::bag(0), ItemPlace::bag(1)).isMerge());
}

TEST_CASE("Items: adding units fills stacks first (lowest slot), then the lowest free slots", "[items][loot]")
{
    auto inv = inventory({{ItemPlace::bag(0), kSword},
                          {ItemPlace::bag(1), kAsh, 19},
                          {ItemPlace::bag(4), kAsh, 12},
                          {ItemPlace::bag(5), kAsh, 20},
                          {ItemPlace::equipment(vg::EquipSlot::MainHand), kSword}});
    // 30 Kül Tozu: 1 into slot 1, 8 into slot 4, 20 + 1 into free slots 2 and 3.
    const auto plan = vg::planItemAdd(catalog(), inv, {{kAsh, 30}, {kDagger2, 1}, {kCore, 2}});
    CHECK(plan.placed == std::vector<std::uint32_t>{30, 1, 2});
    CHECK(plan.totalPlaced() == 33);
    CHECK(plan.grown == std::vector<vg::ItemQuantityChange>{{101, ItemPlace::bag(1), 19, 20}, {102, ItemPlace::bag(4), 12, 20}});
    CHECK(plan.created == std::vector<vg::NewItem>{{ItemPlace::bag(2), kAsh, 20},
                                                   {ItemPlace::bag(3), kAsh, 1},
                                                   {ItemPlace::bag(6), kDagger2, 1},
                                                   {ItemPlace::bag(7), kCore, 2}});
    // Placeholder ids for the created stacks: the plan applies to exactly these slots.
    const std::vector<vg::ItemInstanceId> ids{900, 901, 902, 903};
    const auto changes = plan.changes(inv, ids);
    REQUIRE(changes.size() == 6);
    CHECK(changes[0] == vg::ItemSlotChange{ItemPlace::bag(1), {101, kAsh, 20}});
    CHECK(changes[2] == vg::ItemSlotChange{ItemPlace::bag(2), {900, kAsh, 20}});
    vg::applyItemAdd(inv, plan, ids);
    CHECK(inv.at(ItemPlace::bag(3)) == vg::ItemStack{901, kAsh, 1});
    CHECK(inv.at(ItemPlace::bag(7)) == vg::ItemStack{903, kCore, 2});
    CHECK(inv.count() == 9);

    // A second request of the same template continues the stack the first one created.
    inv = inventory({});
    auto p2 = vg::planItemAdd(catalog(), inv, {{kCore, 3}, {kCore, 4}});
    CHECK(p2.placed == std::vector<std::uint32_t>{3, 4});
    CHECK(p2.created == std::vector<vg::NewItem>{{ItemPlace::bag(0), kCore, 5}, {ItemPlace::bag(1), kCore, 2}});
    CHECK(p2.grown.empty());
}

TEST_CASE("Items: adding to a full bag places what fits; unknown templates and hidden items are left alone",
          "[items][loot]")
{
    std::vector<Row> rows;
    for (std::uint8_t s = 0; s < vg::kBagSlots; ++s)
        rows.push_back({ItemPlace::bag(s), s == 10 ? kAsh : kSword, static_cast<std::uint16_t>(s == 10 ? 17 : 1)});
    auto inv = inventory(rows);
    const auto plan = vg::planItemAdd(catalog(), inv, {{kAsh, 5}, {kShield5, 1}, {999, 1}, {kSlag, 0}});
    CHECK(plan.placed == std::vector<std::uint32_t>{3, 0, 0, 0});
    CHECK(plan.grown == std::vector<vg::ItemQuantityChange>{{110, ItemPlace::bag(10), 17, 20}});
    CHECK(plan.created.empty());
    // A hidden item (template unknown here) is never merged into nor counted as free.
    inv = inventory({{ItemPlace::bag(0), 999, 1}});
    const auto p2 = vg::planItemAdd(catalog(), inv, {{kAsh, 1}});
    CHECK(p2.created == std::vector<vg::NewItem>{{ItemPlace::bag(1), kAsh, 1}});
}

// ---- Loot tables ----

TEST_CASE("Loot: the shipped gamedata/loot.toml loads against the shipped items and monsters", "[loot]")
{
    const auto path = vg::locateLootFile();
    REQUIRE(path.has_value());
    vg::LootTables t;
    std::string error;
    INFO(error);
    REQUIRE(vg::loadLootTables(*path, catalog(), &monsters(), t, error));
    // M10's three tables plus one per M13 homeland kind (monsters 6-13, two levels each); M15: one per new
    // kind (14-34, three levels each) and one per new boss (35-38).
    REQUIRE(t.tables().size() == 18);
    for (vg::MonsterTemplateId m = 1; m <= 38; ++m)
        CHECK(t.forMonster(m) != nullptr); // every monster drops something
    CHECK(t.forMonster(39) == nullptr);
    CHECK(t.forMonster(1) == t.forMonster(2));
    for (vg::MonsterTemplateId m = 6; m <= 12; m += 2)
        CHECK(t.forMonster(m) == t.forMonster(static_cast<vg::MonsterTemplateId>(m + 1)));
    const vg::LootTable& thrall = *t.forMonster(1);
    CHECK(thrall.coinsMin == 3);
    CHECK(thrall.coinsMax == 9);
    REQUIRE(thrall.drops.size() == 21); // M14: two potion lines (PROTOCOL_SHOP.md §5); M15: + 15 Eski armour lines; M16: + Ham Tav Taşı
    CHECK(thrall.drops[0].item == kAsh);
    CHECK(thrall.drops[0].chance == 0.45);
    CHECK(thrall.drops[0].maxCount == 2);
    const vg::LootTable& warden = *t.forMonster(5);
    CHECK(warden.coinsMin == 120);
    CHECK(warden.coinsMax == 180);
    REQUIRE(warden.drops.size() == 10); // M14: + Közlü Şerbet; M15: + its four uniques; M16: + Ham Tav Taşı
    for (std::size_t i = 5; i < 9; ++i)
        CHECK(warden.drops[i].unique);
    CHECK(warden.drops[0].item == kCore);
    CHECK(warden.drops[0].chance == 1.0);
    // Every count fits into one stack.
    for (const auto& table : t.tables())
        for (const auto& d : table.drops)
            CHECK(d.maxCount <= catalog().find(d.item)->maxStack);
}

TEST_CASE("Loot: the loader is strict", "[loot]")
{
    const std::string ok = R"(
[[table]]
monsters = [1]
coins = [1, 2]
[[table.drop]]
item = 9
chance = 0.5
count = [1, 3]
)";
    CHECK(lootError(ok).empty());
    CHECK(lootError("").empty()); // nothing drops
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\nshiny = true\n"), "unknown key 'shiny'"));
    CHECK(contains(lootError("[[tables]]\nmonsters = [1]\n"), "unknown key 'tables'"));
    CHECK(contains(lootError("[[table]]\ncoins = [1, 2]\n"), "'monsters' must be a non-empty list"));
    CHECK(contains(lootError("[[table]]\nmonsters = []\n"), "'monsters' must be a non-empty list"));
    CHECK(contains(lootError("[[table]]\nmonsters = [77]\n"), "unknown monster id 77"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table]]\nmonsters = [2, 1]\n"),
                   "monster 1 is already in another table"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\ncoins = [5, 2]\n"), "min 5 is above max 2"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\ncoins = [0, 100001]\n"), "out of range"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\ncoins = 5\n"), "must be [min, max]"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 999\nchance = 0.5\n"),
                   "unknown item id 999"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 9\nchance = 0\n"), "must be in (0, 1]"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 9\nchance = 1.5\n"), "must be in (0, 1]"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 9\n"), "missing 'chance'"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 11\nchance = 1\ncount = [1, 6]\n"),
                   "out of range (1..5)")); // Kor Çekirdeği stacks to 5
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 12\nchance = 1\ncount = [2, 2]\n"),
                   "out of range (1..1)")); // weapons do not stack
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 9\nchance = 1\ncount = [0, 1]\n"),
                   "out of range"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 9\nchance = 1\n[[table.drop]]\nitem = "
                             "9\nchance = 0.5\n"),
                   "item 9 is listed twice"));
    std::string nine = "[[table]]\nmonsters = [1]\n";
    for (int id : {1, 2, 3, 4, 5, 6, 7, 8, 9})
        nine += "[[table.drop]]\nitem = " + std::to_string(id) + "\nchance = 1\n";
    CHECK(lootError(nine).empty()); // M15: up to 24 lines, M16: 32 (a roll keeps 8 items)
    std::string many = "[[table]]\nmonsters = [1]\n";
    for (int id = 20; id <= 52; ++id)
        many += "[[table.drop]]\nitem = " + std::to_string(id) + "\nchance = 1\n";
    CHECK(contains(lootError(many), "33 drop lines, at most 32"));
    CHECK(contains(lootError("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 9\nchance = 1\ncolour = 1\n"),
                   "unknown key 'colour'"));
    CHECK(contains(lootError("[[table]\n"), "loot.toml:1")); // a TOML syntax error names the line
    // Without monster data the ids are not checked (tools, tests).
    vg::LootTables t;
    std::string error;
    CHECK(vg::parseLootTables("[[table]]\nmonsters = [77]\n", "loot.toml", catalog(), nullptr, t, error));
    // On failure the output is unchanged.
    vg::LootTables keep;
    REQUIRE(vg::parseLootTables(ok, "loot.toml", catalog(), &monsters(), keep, error));
    CHECK_FALSE(vg::parseLootTables("[[table]]\nmonsters = [77]\n", "loot.toml", catalog(), &monsters(), keep, error));
    CHECK(keep.tables().size() == 1);
}

TEST_CASE("Loot: rolls follow the table - coin range, chances and counts (seeded)", "[loot]")
{
    vg::LootTables t;
    std::string error;
    REQUIRE(vg::parseLootTables(R"(
[[table]]
monsters = [1]
coins = [3, 9]
[[table.drop]]
item = 9
chance = 0.45
count = [1, 2]
[[table.drop]]
item = 12
chance = 0.04
[[table.drop]]
item = 11
chance = 1.0
count = [1, 2]
)",
                                "loot.toml", catalog(), &monsters(), t, error));
    const vg::LootTable& table = *t.forMonster(1);
    std::mt19937 rng(12345);
    constexpr int kRolls = 20000;
    std::map<std::uint32_t, int> coins;
    std::map<vg::ItemTemplateId, int> hits;
    std::map<std::uint16_t, int> ashCounts;
    for (int i = 0; i < kRolls; ++i)
    {
        const vg::LootRoll r = vg::rollLoot(table, rng);
        CHECK_FALSE(r.empty());
        ++coins[r.coins];
        int last = -1;
        for (const auto& item : r.items)
        {
            // Drop-line order: 9, 12, 11.
            const int line = item.templateId == kAsh ? 0 : item.templateId == kDagger2 ? 1 : 2;
            CHECK(line > last);
            last = line;
            ++hits[item.templateId];
            if (item.templateId == kAsh)
                ++ashCounts[item.quantity];
            CHECK(item.quantity >= 1);
            CHECK(item.quantity <= 2);
        }
    }
    // Coins: every value 3..9, roughly uniform (1/7 each).
    REQUIRE(coins.size() == 7);
    CHECK(coins.begin()->first == 3);
    CHECK(coins.rbegin()->first == 9);
    for (const auto& [value, n] : coins)
        CHECK(std::abs(n - kRolls / 7) < kRolls / 7 / 5);
    const auto rate = [&](vg::ItemTemplateId id) { return static_cast<double>(hits[id]) / kRolls; };
    CHECK(std::abs(rate(kAsh) - 0.45) < 0.015);
    CHECK(std::abs(rate(kDagger2) - 0.04) < 0.006);
    CHECK(hits[kCore] == kRolls); // chance 1: always
    CHECK(std::abs(ashCounts[1] - ashCounts[2]) < hits[kAsh] / 10);
    // The same seed gives the same rolls.
    std::mt19937 a(7), b(7);
    for (int i = 0; i < 100; ++i)
    {
        const auto ra = vg::rollLoot(table, a);
        const auto rb = vg::rollLoot(table, b);
        CHECK(ra.coins == rb.coins);
        CHECK(ra.items == rb.items);
    }
    // A table without coins and with an unlucky roll is empty.
    vg::LootTable none;
    none.drops.push_back({kAsh, 1e-9, 1, 1});
    std::mt19937 r(1);
    CHECK(vg::rollLoot(none, r).empty());
}

TEST_CASE("Loot: the shipped tables give the intended averages", "[loot]")
{
    const auto path = vg::locateLootFile();
    REQUIRE(path.has_value());
    vg::LootTables t;
    std::string error;
    REQUIRE(vg::loadLootTables(*path, catalog(), &monsters(), t, error));
    std::mt19937 rng(2026);
    const auto average = [&](vg::MonsterTemplateId m, vg::ItemTemplateId item, int n, double& coinsOut) {
        double units = 0.0, coins = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto r = vg::rollLoot(*t.forMonster(m), rng);
            coins += r.coins;
            for (const auto& x : r.items)
                if (x.templateId == item)
                    units += x.quantity;
        }
        coinsOut = coins / n;
        return units / n;
    };
    double coins = 0.0;
    CHECK(std::abs(average(1, kAsh, 20000, coins) - 0.45 * 1.5) < 0.03);
    CHECK(std::abs(coins - 6.0) < 0.1);
    CHECK(std::abs(average(3, kSlag, 20000, coins) - 0.5 * 1.5) < 0.03);
    CHECK(std::abs(coins - 18.5) < 0.2);
    CHECK(std::abs(average(5, kCore, 20000, coins) - 1.5) < 0.03);
    CHECK(std::abs(coins - 150.0) < 1.0);
    CHECK(std::abs(average(5, kShield5, 20000, coins) - 0.35) < 0.015);
}
