// Item upgrades, shared code (docs/PROTOCOL_UPGRADE.md, v10): the chance / fee / stat tables, the tiers
// and stones of the shipped items, the planning of one attempt (every refusal and its order), the
// outcome of a roll, the stone key of items.toml and the smiths' flag in npcs.toml, the stone drops of
// loot.toml, the sale price of an upgraded item, the equipment bonus of an upgraded item and the
// economy table (VO_ECONOMY_TABLE=1 prints it; copied into DEVELOPMENT_PLAN.md §36).

#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/game/Npcs.h"
#include "shared/game/Shop.h"
#include "shared/game/Upgrade.h"
#include "shared/net/Messages.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace vg = vo::game;
namespace vn = vo::net;
using Catch::Matchers::ContainsSubstring;
using UR = vn::UpgradeResult;

namespace {

constexpr vg::ItemTemplateId kHam = 166, kSaf = 167, kUsta = 168;

std::string itemsText()
{
    const auto file = vg::locateItemsFile();
    REQUIRE(file);
    std::ifstream in(*file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

const vg::ItemCatalog& items()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        std::string error;
        const bool ok = vg::parseItemCatalog(itemsText(), "items.toml", out, error);
        INFO(error);
        REQUIRE(ok);
        return out;
    }();
    return c;
}

bool printTables()
{
    char* env = nullptr;
    std::size_t len = 0;
    const bool print = _dupenv_s(&env, &len, "VO_ECONOMY_TABLE") == 0 && env != nullptr;
    std::free(env);
    return print;
}

// The shipped items.toml with one more [[item]] before the starter kits; the parse error ("" if it parses).
std::string itemError(const std::string& item)
{
    std::string text = itemsText();
    const auto kits = text.find("[[starter_kit]]");
    REQUIRE(kits != std::string::npos);
    text.insert(kits, item + "\n");
    vg::ItemCatalog c;
    std::string error;
    if (vg::parseItemCatalog(text, "items.toml", c, error))
        return {};
    return error;
}

vg::Inventory inventory(const std::vector<vg::StoredItem>& rows)
{
    vg::Inventory inv;
    std::string error;
    INFO(error);
    REQUIRE(vg::Inventory::fromItems(rows, inv, error));
    return inv;
}

} // namespace

TEST_CASE("Upgrade: chance, fee and stat tables", "[game][upgrade]")
{
    const std::vector<std::uint16_t> chances{1000, 1000, 1000, 800, 600, 450, 300, 150, 80, 30};
    for (std::uint8_t t = 1; t <= 10; ++t)
        CHECK(vg::upgradeChancePermille(t) == chances[t - 1]);
    CHECK(vg::upgradeChancePermille(0) == 0);
    CHECK(vg::upgradeChancePermille(11) == 0);
    // Failure keeps the item at +4 / +5 and destroys it from +6 on.
    CHECK_FALSE(vg::failureDestroys(5));
    CHECK(vg::failureDestroys(6));
    CHECK(vg::failureDestroys(10));

    // Fees: tier base x the target's multiplier (tenths).
    CHECK(vg::upgradeFeeForTier(1, 1) == 6);
    CHECK(vg::upgradeFeeForTier(1, 5) == 24);
    CHECK(vg::upgradeFeeForTier(3, 6) == 330);
    CHECK(vg::upgradeFeeForTier(5, 7) == 1520);
    CHECK(vg::upgradeFeeForTier(5, 10) == 3800);
    CHECK(vg::upgradeFeeForTier(5, 0) == 0);
    CHECK(vg::upgradeFeeForTier(5, 11) == 0);
    for (std::uint8_t tier = 1; tier <= 5; ++tier)
        for (std::uint8_t t = 2; t <= 10; ++t)
        {
            CHECK(vg::upgradeFeeForTier(tier, t) > vg::upgradeFeeForTier(tier, static_cast<std::uint8_t>(t - 1))); // grows with the target
            if (tier > 1)
                CHECK(vg::upgradeFeeForTier(tier, t) > vg::upgradeFeeForTier(static_cast<std::uint8_t>(tier - 1), t)); // and with the tier
        }

    // Stat bonus: small steps to +5, larger from +6; rounded halves up; nothing on a 0.
    const std::vector<std::uint16_t> pct{0, 5, 10, 15, 20, 25, 31, 38, 46, 55, 65};
    for (std::uint8_t l = 0; l <= 10; ++l)
        CHECK(vg::upgradeBonusPercent(l) == pct[l]);
    for (std::uint8_t l = 2; l <= 10; ++l)
    {
        const int step = pct[l] - pct[l - 1];
        CHECK(step == (l <= 5 ? 5 : static_cast<int>(l))); // 5 % a level up to +5, then 6, 7, 8, 9, 10
    }
    CHECK(vg::upgradeBonus(16, 7) == 6);   // 6.08
    CHECK(vg::upgradeBonus(16, 1) == 1);   // 0.80
    CHECK(vg::upgradeBonus(10, 1) == 1);   // 0.50: halves up
    CHECK(vg::upgradeBonus(6, 1) == 0);    // 0.30
    CHECK(vg::upgradeBonus(19, 7) == 7);   // 7.22
    CHECK(vg::upgradeBonus(22, 5) == 6);   // 5.50
    CHECK(vg::upgradeBonus(20, 10) == 13); // 13.0
    CHECK(vg::upgradeBonus(1, 10) == 1);   // 0.65
    CHECK(vg::upgradeBonus(1, 5) == 0);    // 0.25
    CHECK(vg::upgradeBonus(0, 10) == 0);
    CHECK(vg::upgradeBonus(-5, 10) == 0);
    for (std::int32_t base = 0; base <= 40; ++base)
        for (std::uint8_t l = 1; l <= 10; ++l)
        {
            const auto prev = vg::upgradeBonus(base, static_cast<std::uint8_t>(l - 1));
            CHECK(vg::upgradeBonus(base, l) >= prev); // never goes down
            if (base >= vg::kUpgradeStepBase && l >= 6)
                CHECK(vg::upgradeBonus(base, l) > prev); // from +6 every level adds something
        }
    // The starter axe (10): +3 at +5, then +4 .. +8 (+6..+10; the percentages alone would stall at +6).
    CHECK(vg::upgradeBonus(10, 5) == 3);
    CHECK(vg::upgradeBonus(10, 6) == 4);
    CHECK(vg::upgradeBonus(10, 10) == 8);
    CHECK(vg::upgradeBonus(4, 6) == 1); // below the step base: the plain percentage (1.24)
}

TEST_CASE("Upgrade: tiers and stones of the shipped items", "[game][upgrade][content]")
{
    const auto& c = items();
    // The three stones: stackable materials of grades 1-3 with a price.
    const std::vector<std::pair<vg::ItemTemplateId, vg::StoneGrade>> stones{
        {kHam, vg::StoneGrade::Ham}, {kSaf, vg::StoneGrade::Saf}, {kUsta, vg::StoneGrade::Usta}};
    for (const auto& [id, grade] : stones)
    {
        const vg::ItemTemplate* t = c.find(id);
        REQUIRE(t);
        CHECK(t->kind == vg::ItemKind::Material);
        CHECK(t->upgradeStone == static_cast<std::uint8_t>(grade));
        CHECK(t->isStackable());
        CHECK(t->value > 0);
        CHECK_FALSE(vg::isUpgradable(*t));
        CHECK(vg::stoneTemplate(c, grade) == t);
        CHECK(t->name == vg::stoneGradeName(grade));
    }
    CHECK(c.find(kHam)->value < c.find(kSaf)->value);
    CHECK(c.find(kSaf)->value < c.find(kUsta)->value);
    CHECK(vg::stoneTemplate(c, vg::StoneGrade::None) == nullptr);

    std::map<vg::StoneGrade, int> byGrade;
    int upgradable = 0, stoneItems = 0;
    for (const vg::ItemTemplate& t : c.templates())
    {
        if (t.upgradeStone != 0)
            ++stoneItems;
        const bool gear = t.kind == vg::ItemKind::Weapon || t.kind == vg::ItemKind::Shield || t.kind == vg::ItemKind::Armor;
        CHECK(vg::isUpgradable(t) == gear); // every weapon, shield and armour piece, uniques included
        if (!gear)
            continue;
        ++upgradable;
        const std::uint8_t tier = vg::upgradeTier(t);
        INFO(t.name << " level " << t.level << " tier " << int(tier));
        CHECK(tier == std::min(5, (t.level - 1) / 4 + 1));
        const vg::StoneGrade g = vg::requiredStone(t);
        ++byGrade[g];
        if (t.rarity == vg::Rarity::Unique)
            CHECK(g == vg::StoneGrade::Usta);
        else
            CHECK(g == (tier <= 2 ? vg::StoneGrade::Ham : tier <= 4 ? vg::StoneGrade::Saf : vg::StoneGrade::Usta));
    }
    CHECK(stoneItems == 3);
    CHECK(upgradable >= 140);
    CHECK(byGrade[vg::StoneGrade::Ham] > 0);
    CHECK(byGrade[vg::StoneGrade::Saf] > 0);
    CHECK(byGrade[vg::StoneGrade::Usta] > 0);
}

TEST_CASE("Upgrade: +7 on a tier is about the next tier at +0", "[game][upgrade][content]")
{
    // For every weapon grip and every armour slot / line: the strongest non-unique item of a tier at +7
    // against the strongest of the next tier at +0 (attack for weapons, defence for shields / armour).
    const auto& c = items();
    struct Key
    {
        int kind = 0, slot = 0, grip = 0, classes = 0;
        bool operator<(const Key& o) const
        {
            return std::tie(kind, slot, grip, classes) < std::tie(o.kind, o.slot, o.grip, o.classes);
        }
    };
    std::map<Key, std::map<int, const vg::ItemTemplate*>> best;
    const auto power = [](const vg::ItemTemplate& t, std::uint8_t level) {
        return t.kind == vg::ItemKind::Weapon ? vg::upgradedAttack(t, level) : vg::upgradedDefense(t, level);
    };
    for (const vg::ItemTemplate& t : c.templates())
    {
        if (!vg::isUpgradable(t) || t.rarity == vg::Rarity::Unique || power(t, 0) == 0)
            continue;
        const Key k{static_cast<int>(t.kind), static_cast<int>(t.slot), t.kind == vg::ItemKind::Armor ? 0 : static_cast<int>(t.grip),
                    t.kind == vg::ItemKind::Armor ? t.classMask : 0};
        const vg::ItemTemplate*& slot = best[k][vg::upgradeTier(t)];
        if (slot == nullptr || power(t, 0) > power(*slot, 0))
            slot = &t;
    }
    const bool print = printTables();
    if (print)
        std::printf("\n| Group | Tier N item (+0 -> +7) | Tier N+1 item (+0) | +7 / next |\n|---|---|---|---|\n");
    int pairs = 0;
    double lo = 9.0, hi = 0.0;
    for (const auto& [key, tiers] : best)
    {
        for (const auto& [tier, a] : tiers)
        {
            const auto next = tiers.find(tier + 1);
            if (next == tiers.end())
                continue;
            const vg::ItemTemplate* b = next->second;
            if (power(*b, 0) < 6)
                continue; // tiny armour pieces (1-5 defence) round too coarsely to compare
            const double ratio = static_cast<double>(power(*a, 7)) / power(*b, 0);
            lo = std::min(lo, ratio);
            hi = std::max(hi, ratio);
            ++pairs;
            if (print)
                std::printf("| %s %s | %s %d -> %d | %s %d | %.2f |\n", std::string(vg::itemKindDisplayName(a->kind)).c_str(),
                            std::string(vg::slotRuleDisplayName(a->slot)).c_str(), a->name.c_str(), power(*a, 0),
                            power(*a, 7), b->name.c_str(), power(*b, 0), ratio);
            INFO(a->name << " +7 = " << power(*a, 7) << " against " << b->name << " " << power(*b, 0));
            // Tiers 2-5 meet the next tier at +7; the starter-kit / early items of tier 1 stay a little below.
            CHECK(ratio >= (tier >= 2 ? 0.85 : 0.60));
            CHECK(ratio <= 1.25);
        }
    }
    if (print)
        std::printf("pairs %d, ratio %.2f .. %.2f\n", pairs, lo, hi);
    CHECK(pairs >= 20);
}

TEST_CASE("Upgrade: planning an attempt, every refusal in order", "[game][upgrade]")
{
    const auto& c = items();
    // Bag: 0 Er Kılıcı (+0, tier 1), 1 three Ham stones, 2 one Saf stone, 3 Kül Tozu, 4 a Kor sword (tier 5,
    // +9), 5 a unique (+10), 6 a Kızılca Şerbet; equipped: Meşe Kalkan.
    const vg::ItemTemplate* kor = nullptr;
    const vg::ItemTemplate* unique = nullptr;
    for (const vg::ItemTemplate& t : c.templates())
    {
        if (!kor && vg::isUpgradable(t) && t.rarity != vg::Rarity::Unique && vg::upgradeTier(t) == 5)
            kor = &t;
        if (!unique && t.rarity == vg::Rarity::Unique)
            unique = &t;
    }
    REQUIRE(kor);
    REQUIRE(unique);
    const auto inv = inventory({{10, vg::ItemPlace::bag(0), 1, 1, 0},
                                {11, vg::ItemPlace::bag(1), kHam, 3, 0},
                                {12, vg::ItemPlace::bag(2), kSaf, 1, 0},
                                {13, vg::ItemPlace::bag(3), 9, 5, 0},
                                {14, vg::ItemPlace::bag(4), kor->id, 1, 9},
                                {15, vg::ItemPlace::bag(5), unique->id, 1, 10},
                                {16, vg::ItemPlace::bag(6), 16, 2, 0},
                                {17, vg::ItemPlace::equipment(vg::EquipSlot::OffHand), 2, 1, 4}});
    const auto plan = [&](std::uint32_t coins, std::uint8_t slot, std::uint64_t id, std::uint8_t from,
                          std::uint8_t stone = vn::kUpgradeAnyStone) {
        return vg::planUpgrade(c, inv, coins, slot, id, from, stone);
    };
    // NoItem: out of range, empty, another item id.
    CHECK(plan(1000, 28, 10, 0).result == UR::NoItem);
    CHECK(plan(1000, 7, 10, 0).result == UR::NoItem);
    CHECK(plan(1000, 0, 11, 0).result == UR::NoItem);
    // Changed: not at the level the client named.
    CHECK(plan(1000, 0, 10, 1).result == UR::Changed);
    CHECK(plan(1000, 0, 10, 1).level == 0);
    // NotUpgradable: a stone, a material, a potion.
    CHECK(plan(1000, 1, 11, 0).result == UR::NotUpgradable);
    CHECK(plan(1000, 3, 13, 0).result == UR::NotUpgradable);
    CHECK(plan(1000, 6, 16, 0).result == UR::NotUpgradable);
    // MaxLevel before anything about stones or coins.
    CHECK(plan(0, 5, 15, 10).result == UR::MaxLevel);
    // NoStone: the Kor sword needs an Usta stone and the bag has none; a named empty slot; out of range.
    CHECK(plan(100000, 4, 14, 9).result == UR::NoStone);
    CHECK(plan(1000, 0, 10, 0, 20).result == UR::NoStone);
    CHECK(plan(1000, 0, 10, 0, 40).result == UR::NoStone);
    // WrongStone: a Saf stone, a material or the item itself named for a tier-1 sword.
    CHECK(plan(1000, 0, 10, 0, 2).result == UR::WrongStone);
    CHECK(plan(1000, 0, 10, 0, 3).result == UR::WrongStone);
    CHECK(plan(1000, 0, 10, 0, 0).result == UR::WrongStone);
    // NotEnoughCoins: the fee of +1 on tier 1 is 6.
    CHECK(plan(5, 0, 10, 0).result == UR::NotEnoughCoins);
    // An attempt: the lowest fitting stone, the fee, the chance, what a failure does.
    const auto ok = plan(6, 0, 10, 0);
    REQUIRE(ok.result == UR::Success);
    CHECK(ok.itemPlace == vg::ItemPlace::bag(0));
    CHECK(ok.stonePlace == vg::ItemPlace::bag(1));
    CHECK(ok.stone.quantity == 3);
    CHECK(ok.target == 1);
    CHECK(ok.fee == 6);
    CHECK(ok.chancePermille == 1000);
    CHECK_FALSE(ok.destroysOnFailure);
    CHECK(vg::upgradeOutcome(ok, 999) == UR::Success); // +1 cannot fail
    CHECK(vg::itemAfterUpgrade(ok, UR::Success).upgrade == 1);
    CHECK(vg::itemAfterUpgrade(ok, UR::Success).id == 10);
    CHECK(vg::stoneAfterUpgrade(ok).quantity == 2);
    CHECK(plan(6, 0, 10, 0, 1).result == UR::Success); // naming the right stone slot

    // Higher targets: +4 keeps the item on failure, +6 destroys it; the last stone empties its slot.
    const auto inv2 = inventory({{20, vg::ItemPlace::bag(0), 1, 1, 3}, {21, vg::ItemPlace::bag(1), kHam, 1, 0},
                                 {22, vg::ItemPlace::bag(2), 7, 1, 5}});
    const auto p4 = vg::planUpgrade(c, inv2, 1000, 0, 20, 3, vn::kUpgradeAnyStone);
    REQUIRE(p4.result == UR::Success);
    CHECK(p4.target == 4);
    CHECK(p4.chancePermille == 800);
    CHECK(vg::upgradeOutcome(p4, 799) == UR::Success);
    CHECK(vg::upgradeOutcome(p4, 800) == UR::Failed);
    CHECK(vg::itemAfterUpgrade(p4, UR::Failed) == inv2.at(vg::ItemPlace::bag(0)));
    CHECK(vg::stoneAfterUpgrade(p4).empty());
    const auto p6 = vg::planUpgrade(c, inv2, 1000, 2, 22, 5, vn::kUpgradeAnyStone);
    REQUIRE(p6.result == UR::Success);
    CHECK(p6.destroysOnFailure);
    CHECK(vg::upgradeOutcome(p6, 449) == UR::Success);
    CHECK(vg::upgradeOutcome(p6, 450) == UR::Destroyed);
    CHECK(vg::itemAfterUpgrade(p6, UR::Destroyed).empty());
    CHECK(vg::countStones(c, inv2, vg::StoneGrade::Ham) == 1);
    CHECK(vg::countStones(c, inv, vg::StoneGrade::Ham) == 3);
    CHECK(vg::countStones(c, inv, vg::StoneGrade::Usta) == 0);

    // A uniform roll hits each chance: over 0..999 exactly `chance` rolls succeed.
    for (std::uint8_t target = 1; target <= 10; ++target)
    {
        vg::UpgradePlan p;
        p.chancePermille = vg::upgradeChancePermille(target);
        p.destroysOnFailure = vg::failureDestroys(target);
        int wins = 0;
        for (std::uint16_t r = 0; r < 1000; ++r)
            wins += vg::upgradeOutcome(p, r) == UR::Success ? 1 : 0;
        CHECK(wins == p.chancePermille);
    }
}

TEST_CASE("Upgrade: stored upgrade levels in an inventory", "[game][upgrade]")
{
    vg::Inventory inv;
    std::string error;
    CHECK(vg::Inventory::fromItems({{1, vg::ItemPlace::bag(0), 1, 1, 10}}, inv, error));
    CHECK(inv.items().front().upgrade == 10);
    CHECK_FALSE(vg::Inventory::fromItems({{1, vg::ItemPlace::bag(0), 1, 1, 11}}, inv, error));
    CHECK_THAT(error, ContainsSubstring("invalid upgrade level 11"));
    CHECK_FALSE(vg::Inventory::fromItems({{1, vg::ItemPlace::bag(0), 9, 2, 1}}, inv, error)); // a stack
    // A move keeps the level (the whole stack moves).
    vg::Inventory moved;
    REQUIRE(vg::Inventory::fromItems({{1, vg::ItemPlace::bag(0), 1, 1, 7}}, moved, error));
    const vg::ItemMovePlan plan = vg::planItemMove(items(), moved, {1, 1, {}}, vg::ItemPlace::bag(0),
                                                   vg::ItemPlace::equipment(vg::EquipSlot::MainHand));
    REQUIRE(plan.result == vg::ItemMoveResult::Ok);
    vg::applyItemMove(moved, plan);
    CHECK(moved.equipped(vg::EquipSlot::MainHand).upgrade == 7);
}

TEST_CASE("Upgrade: the equipment bonus counts the upgrade level, the sale price does not", "[game][upgrade]")
{
    const auto& c = items();
    vg::Inventory plain, upgraded;
    std::string error;
    REQUIRE(vg::Inventory::fromItems({{1, vg::ItemPlace::equipment(vg::EquipSlot::MainHand), 7, 1, 0},
                                      {2, vg::ItemPlace::equipment(vg::EquipSlot::OffHand), 2, 1, 0}},
                                     plain, error));
    REQUIRE(vg::Inventory::fromItems({{1, vg::ItemPlace::equipment(vg::EquipSlot::MainHand), 7, 1, 10},
                                      {2, vg::ItemPlace::equipment(vg::EquipSlot::OffHand), 2, 1, 10}},
                                     upgraded, error));
    const vg::ItemTemplate& sword = *c.find(7);  // Burç Kılıcı, attack 9
    const vg::ItemTemplate& shield = *c.find(2); // Meşe Kalkan, defence 4
    const vg::EquipBonus a = vg::equipmentBonus(c, plain);
    const vg::EquipBonus b = vg::equipmentBonus(c, upgraded);
    CHECK(b.attack == a.attack + vg::upgradeBonus(sword.attack, 10));
    CHECK(b.defense == a.defense + vg::upgradeBonus(shield.defense, 10));
    CHECK(b.attributes == a.attributes); // nothing else changes
    CHECK(vg::deriveStats(1, 5, {}, b).attack > vg::deriveStats(1, 5, {}, a).attack);
    // A merchant pays the plain price for an upgraded item (no Akçe from upgrading), uniques nothing.
    for (const vg::ItemTemplate& t : c.templates())
    {
        if (!vg::isUpgradable(t))
            continue;
        CHECK(vg::sellPrice(t) <= t.value / 2);
        if (t.rarity == vg::Rarity::Unique)
            CHECK(vg::sellPrice(t) == 0);
    }
}

TEST_CASE("Upgrade: items.toml upgrade_stone and npcs.toml upgrades are checked", "[game][upgrade][data]")
{
    CHECK(itemError(R"([[item]]
id = 900
name = "Taş"
kind = "material"
icon = "x"
max_stack = 20
value = 5
upgrade_stone = 2
)")
              .empty());
    CHECK_THAT(itemError(R"([[item]]
id = 900
name = "Taş"
kind = "material"
icon = "x"
upgrade_stone = 4
)"),
               ContainsSubstring("upgrade_stone"));
    CHECK_THAT(itemError(R"([[item]]
id = 900
name = "Kılıç"
kind = "weapon"
slot = "main_hand"
classes = ["warrior"]
level = 1
attack = 5
defense = 0
model = "sword_soldier"
icon = "sword_soldier"
grip = "sword"
upgrade_stone = 1
)"),
               ContainsSubstring("'upgrade_stone' is only allowed for a material"));

    const std::string npc = R"([[npc]]
id = 1
name = "Ad"
title = "Unvan"
greeting = "Merhaba"
model = "npc_test"
zone = 1
x = 100.0
z = 120.0
yaw = 0.0
kind = "merchant"
stock = [16, 1]
)";
    vg::NpcCatalog n;
    std::string error;
    REQUIRE(vg::parseNpcCatalog(npc, "npcs.toml", &items(), n, error));
    CHECK_FALSE(n.npcs().front().upgrades);
    REQUIRE(vg::parseNpcCatalog(npc + "upgrades = true\n", "npcs.toml", &items(), n, error));
    CHECK(n.npcs().front().upgrades);
    vg::NpcCatalog bad;
    CHECK_FALSE(vg::parseNpcCatalog(npc + "upgrades = 1\n", "npcs.toml", &items(), bad, error));
    CHECK_THAT(error, ContainsSubstring("'upgrades' must be true or false"));

    // The shipped smiths upgrade and sell the three stones; the potion sellers do neither.
    vg::NpcCatalog shipped;
    const auto file = vg::locateNpcsFile();
    REQUIRE(file);
    REQUIRE(vg::loadNpcCatalog(*file, &items(), shipped, error));
    int smiths = 0;
    for (const vg::NpcDef& d : shipped.npcs())
    {
        const bool sellsStones = std::count_if(d.stock.begin(), d.stock.end(), [](vg::ItemTemplateId t) {
                                     return t == kHam || t == kSaf || t == kUsta;
                                 }) == 3;
        CHECK(d.upgrades == sellsStones);
        CHECK(d.stock.size() <= vn::kMaxShopEntries);
        smiths += d.upgrades ? 1 : 0;
    }
    CHECK(smiths == 3);
}

TEST_CASE("Upgrade: monsters drop the stones of their level", "[game][upgrade][content]")
{
    const auto& c = items();
    vg::LootTables loot;
    vg::MonsterData monsters;
    std::string error;
    const auto lootFile = vg::locateLootFile();
    const auto monsterFile = vg::locateMonstersFile();
    REQUIRE(lootFile);
    REQUIRE(monsterFile);
    REQUIRE(vg::loadMonsterData(*monsterFile, monsters, error));
    REQUIRE(vg::loadLootTables(*lootFile, c, &monsters, loot, error));
    std::map<vg::ItemTemplateId, int> dropping;
    for (const vg::MonsterTemplate& m : monsters.templates())
    {
        const vg::LootTable* t = loot.forMonster(m.id);
        if (!t)
            continue;
        for (const vg::LootDrop& d : t->drops)
        {
            const vg::ItemTemplate* it = c.find(d.item);
            if (!it || it->upgradeStone == 0)
                continue;
            ++dropping[d.item];
            INFO(m.name << " (level " << m.level << ") drops " << it->name << " at " << d.chance);
            // The grade of the levels it serves: Ham up to level 10, Saf from 8, Usta from 14.
            if (d.item == kHam)
                CHECK(m.level <= 10);
            if (d.item == kSaf)
                CHECK(m.level >= 8);
            if (d.item == kUsta)
                CHECK(m.level >= 14);
            // Modest from ordinary monsters, generous from bosses.
            CHECK(d.chance <= (m.elite ? 0.5 : 0.03));
        }
    }
    CHECK(dropping[kHam] > 0);
    CHECK(dropping[kSaf] > 0);
    CHECK(dropping[kUsta] > 0);
}

TEST_CASE("Upgrade: v10 messages", "[net][messages][upgrade]")
{
    CHECK(static_cast<std::uint16_t>(vn::Opcode::UpgradeReq) == 0x0A00);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::UpgradeRes) == 0x0A01);
    CHECK(vn::opcodeName(vn::Opcode::UpgradeReq) == "UPGRADE_REQ");
    CHECK(vn::isKnownOpcode(0x0A01));
    const vn::UpgradeReq req{7, 1234, 5, 0x0102030405060708ull, 6, vn::kUpgradeAnyStone};
    const auto bytes = vn::encodeMessage(req);
    REQUIRE(bytes);
    CHECK(bytes->size() == 19);
    vn::UpgradeReq back;
    REQUIRE(vn::decodeMessage(*bytes, back));
    CHECK(back == req);
    for (std::uint8_t r = 0; r <= vn::kUpgradeResultMax; ++r)
    {
        const vn::UpgradeRes res{9, static_cast<UR>(r), 3, 46, 7, 167, 330};
        const auto b = vn::encodeMessage(res);
        REQUIRE(b);
        CHECK(b->size() == 15);
        vn::UpgradeRes out;
        REQUIRE(vn::decodeMessage(*b, out));
        CHECK(out == res);
        CHECK(vn::upgradeResultName(static_cast<UR>(r)) != "Unknown");
    }
    // Writers and readers refuse an unknown result and a level above 10.
    CHECK_FALSE(vn::encodeMessage(vn::UpgradeRes{1, static_cast<UR>(15), 0, 0, 0, 0, 0}));
    CHECK_FALSE(vn::encodeMessage(vn::UpgradeRes{1, UR::Success, 0, 0, 11, 0, 0}));
    auto raw = *vn::encodeMessage(vn::UpgradeRes{1, UR::Success, 0, 0, 10, 0, 0});
    raw[4] = 15;
    vn::UpgradeRes bad;
    CHECK_FALSE(vn::decodeMessage(raw, bad));
    raw[4] = 0;
    raw[8] = 11;
    CHECK_FALSE(vn::decodeMessage(raw, bad));
    // ItemEntry carries the level (v10): 15 bytes; more than 10 is refused.
    const auto entry = vn::encodeMessage(vn::ItemUpdate{{{1, 2, 99, 46, 1, 7}}});
    REQUIRE(entry);
    CHECK(entry->size() == 1 + 15);
    vn::ItemUpdate u;
    REQUIRE(vn::decodeMessage(*entry, u));
    CHECK(u.items.front().upgrade == 7);
    auto e2 = *entry;
    e2.back() = 11;
    CHECK_FALSE(vn::decodeMessage(e2, u));
    CHECK(vn::isUpgradeAttempt(UR::Destroyed));
    CHECK_FALSE(vn::isUpgradeAttempt(UR::NoStone));
}

TEST_CASE("Upgrade: the cost of +5, +7 and +10 by tier", "[game][upgrade][economy]")
{
    // Expected Akçe to own one item at +k, starting from a fresh one bought at its value: up to +5 a
    // failure only costs the attempt (V_k = V_(k-1) + c_k / p_k); from +6 a failure destroys it, so a new
    // +(k-1) item is needed (V_k = (V_(k-1) + c_k) / p_k). c_k = fee + the stone at its merchant price.
    const auto& c = items();
    const bool print = printTables();
    // A representative item price per tier: the median value of the common / uncommon gear of that tier.
    std::map<int, std::vector<std::uint32_t>> values;
    for (const vg::ItemTemplate& t : c.templates())
        if (vg::isUpgradable(t) && t.rarity != vg::Rarity::Unique && t.value > 0)
            values[vg::upgradeTier(t)].push_back(t.value);
    if (print)
        std::printf("\n| Tier | Stone | Item value | Fee +1 / +5 / +7 / +10 | To +5 | To +7 | To +10 |\n|---|---|---|---|---|---|---|\n");
    std::vector<double> to5(6), to7(6), to10(6);
    for (int tier = 1; tier <= 5; ++tier)
    {
        auto& v = values[tier];
        REQUIRE(!v.empty());
        std::sort(v.begin(), v.end());
        const double item = v[v.size() / 2];
        const auto t8 = static_cast<std::uint8_t>(tier);
        const vg::StoneGrade g = tier <= 2 ? vg::StoneGrade::Ham : tier <= 4 ? vg::StoneGrade::Saf : vg::StoneGrade::Usta;
        const double stone = vg::stoneTemplate(c, g)->value;
        double cost = item;
        for (std::uint8_t k = 1; k <= 10; ++k)
        {
            const double ck = vg::upgradeFeeForTier(t8, k) + stone;
            const double p = vg::upgradeChancePermille(k) / 1000.0;
            cost = vg::failureDestroys(k) ? (cost + ck) / p : cost + ck / p;
            if (k == 5)
                to5[tier] = cost - item;
            if (k == 7)
                to7[tier] = cost - item;
            if (k == 10)
                to10[tier] = cost - item;
        }
        if (print)
            std::printf("| %d | %s | %.0f | %u / %u / %u / %u | %.0f | %.0f | %.0f |\n", tier,
                        std::string(vg::stoneGradeName(g)).c_str(), item, vg::upgradeFeeForTier(t8, 1),
                        vg::upgradeFeeForTier(t8, 5), vg::upgradeFeeForTier(t8, 7), vg::upgradeFeeForTier(t8, 10),
                        to5[tier], to7[tier], to10[tier]);
    }
    for (int tier = 2; tier <= 5; ++tier)
    {
        CHECK(to5[tier] > to5[tier - 1]);
        CHECK(to7[tier] > to7[tier - 1]);
    }
    // The sink against the income of DEVELOPMENT_PLAN §35.5 (level 12: ~4 200-5 600 Akçe / hour, level 20:
    // ~8 000-11 000): one tier-3 item to +5 is a fraction of an hour at level 12, a set of seven tier-5 items
    // to +5 is several hours at level 20, +7 is a long-term goal and +10 a fortune.
    CHECK(to5[3] > 500.0);
    CHECK(to5[3] < 4200.0);
    CHECK(7.0 * to5[5] > 2.0 * 11000.0);
    CHECK(7.0 * to5[5] < 8.0 * 8000.0);
    CHECK(to7[5] > 3.0 * 8000.0);
    CHECK(to10[5] > 100.0 * 8000.0);
    // Levels 1-4 earn ~440 Akçe (§30): a tier-1 item to +3 is affordable there.
    const double to3tier1 = 3.0 * vg::stoneTemplate(c, vg::StoneGrade::Ham)->value + vg::upgradeFeeForTier(1, 1) +
                            vg::upgradeFeeForTier(1, 2) + vg::upgradeFeeForTier(1, 3);
    CHECK(to3tier1 < 150.0);
}
