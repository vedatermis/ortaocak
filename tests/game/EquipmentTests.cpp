// M18 "Kuşanma" (docs/PROTOCOL_EQUIPMENT.md, protocol v12; docs/DEVELOPMENT_PLAN.md §39): the new item keys
// (damage range, speed_ms, reach, bonus lines, kind "accessory"), the accessory slots, the derived stats
// (weapon interval and reach, the basic swing factor, the bonus lines), the damage roll with a range, the
// v12 CHAR_STATS, and the shipped content: every weapon has its speed, the families are sidegrades (damage
// per second within a band at equal level), every class has a weapon, a full armour set and every
// accessory slot in each level band, requirements can be met, every template has a source. With
// VO_ECONOMY_TABLE=1 the balance table of §39 is printed.

#include "shared/core/Config.h"
#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/game/Npcs.h"
#include "shared/game/Quests.h"
#include "shared/game/Upgrade.h"
#include "shared/net/Messages.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vg = vo::game;
namespace vn = vo::net;
using Catch::Approx;
using vg::EquipSlot;
using vg::ItemPlace;
using R = vg::ItemMoveResult;

namespace {

constexpr std::uint8_t kWarrior = 1, kRogue = 2, kMage = 3, kPriest = 4;

const vg::ItemCatalog& items()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        std::string e;
        const bool ok = vg::loadItemCatalog(*vg::locateItemsFile(), out, e);
        INFO(e);
        REQUIRE(ok);
        return out;
    }();
    return c;
}

const vg::MonsterData& monsters()
{
    static const vg::MonsterData m = [] {
        vg::MonsterData out;
        std::string e;
        const bool ok = vg::loadMonsterData(*vg::locateMonstersFile(), out, e);
        INFO(e);
        REQUIRE(ok);
        return out;
    }();
    return m;
}

const vg::LootTables& loot()
{
    static const vg::LootTables t = [] {
        vg::LootTables out;
        std::string e;
        const bool ok = vg::loadLootTables(*vg::locateLootFile(), items(), &monsters(), out, e);
        INFO(e);
        REQUIRE(ok);
        return out;
    }();
    return t;
}

// A minimal valid file plus `extra` items (ids 10+); the kits use items 1-3.
std::string fileWith(const std::string& extra)
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
name = "Hançer"
kind = "weapon"
slot = "either_hand"
classes = ["rogue"]
level = 1
attack = 4
defense = 0
model = "dagger"
icon = "dagger"
grip = "dagger"

[[item]]
id = 3
name = "Asa"
kind = "weapon"
slot = "two_hand"
classes = ["mage", "priest"]
level = 1
attack = 7
defense = 0
model = "staff"
icon = "staff"
grip = "staff"
)" + extra + R"(
[[starter_kit]]
class = "warrior"
equip = { main_hand = 1 }

[[starter_kit]]
class = "rogue"
equip = { main_hand = 2 }

[[starter_kit]]
class = "mage"
equip = { main_hand = 3 }

[[starter_kit]]
class = "priest"
equip = { main_hand = 3 }
)";
}

std::string parseError(const std::string& text, vg::ItemCatalog* out = nullptr)
{
    vg::ItemCatalog c;
    std::string error;
    if (vg::parseItemCatalog(text, "items.toml", c, error))
    {
        if (out)
            *out = std::move(c);
        return {};
    }
    return error;
}

const std::string kHalberd = R"(
[[item]]
id = 10
name = "Teber"
kind = "weapon"
slot = "two_hand"
classes = ["warrior"]
level = 1
attack = 16
damage = [13, 19]
speed_ms = 2300
reach = 3.3
defense = 0
model = "halberd"
icon = "halberd"
grip = "axe2h"
max_hp = 20
crit = 5
)";

const std::string kRing = R"(
[[item]]
id = 11
name = "Yüzük"
kind = "accessory"
slot = "ring"
classes = ["warrior", "rogue", "mage", "priest"]
level = 1
attack = 0
defense = 1
icon = "acc_ring_band"
strength = 2
max_mp = 15
dodge = 7
)";

const std::string kEarring = R"(
[[item]]
id = 12
name = "Küpe"
kind = "accessory"
slot = "earring"
classes = ["warrior", "rogue", "mage", "priest"]
level = 1
attack = 0
defense = 0
icon = "acc_earring_hoop"
crit = 10
)";

std::string replaced(std::string s, const std::string& what, const std::string& with)
{
    const auto pos = s.find(what);
    REQUIRE(pos != std::string::npos);
    s.replace(pos, what.size(), with);
    return s;
}

bool contains(const std::string& s, const std::string& what)
{
    return s.find(what) != std::string::npos;
}

vg::Inventory inventoryOf(const std::vector<std::pair<ItemPlace, vg::ItemTemplateId>>& rows)
{
    std::vector<vg::StoredItem> stored;
    for (std::size_t i = 0; i < rows.size(); ++i)
        stored.push_back({100 + i, rows[i].first, rows[i].second, 1});
    vg::Inventory inv;
    std::string error;
    INFO(error);
    REQUIRE(vg::Inventory::fromItems(stored, inv, error));
    return inv;
}

ItemPlace eq(EquipSlot s)
{
    return ItemPlace::equipment(s);
}

// The template by name (content tests).
const vg::ItemTemplate& named(const std::string& name)
{
    const vg::ItemTemplate* found = nullptr;
    for (const auto& t : items().templates())
        if (t.name == name)
            found = &t;
    INFO("item " << name);
    REQUIRE(found != nullptr);
    return *found;
}

// A sensible allocation at `level`: two thirds on the class's attack attribute, the rest on Dayanıklılık.
vg::Attributes build(std::uint8_t cls, std::uint16_t level)
{
    const std::uint32_t points = vg::pointsForLevel(level);
    vg::Attributes a{};
    const vg::Attribute primary = vg::classTable(cls)->attackAttribute;
    a[vg::attributeIndex(primary)] = static_cast<std::uint16_t>(points * 2 / 3);
    a[vg::attributeIndex(vg::Attribute::Vitality)] =
        static_cast<std::uint16_t>(a[vg::attributeIndex(vg::Attribute::Vitality)] + points - points * 2 / 3);
    return a;
}

// The highest defence of the ordinary monsters within one level of `level` (the target of a hunt).
std::uint32_t monsterDefense(std::uint16_t level)
{
    std::uint32_t d = 0;
    for (const auto& m : monsters().templates())
        if (!m.elite && m.level + 1 >= level && m.level <= level + 1)
            d = std::max(d, m.defense);
    return d;
}

struct Loadout
{
    std::string label;
    std::vector<std::pair<EquipSlot, const vg::ItemTemplate*>> pieces;
};

vg::Stats statsOf(std::uint8_t cls, std::uint16_t level, const Loadout& l)
{
    std::vector<std::pair<ItemPlace, vg::ItemTemplateId>> rows;
    for (const auto& [slot, t] : l.pieces)
        rows.push_back({eq(slot), t->id});
    const vg::Inventory inv = inventoryOf(rows);
    return vg::deriveStats(cls, level, build(cls, level), vg::equipmentBonus(items(), inv));
}

// The weapons of one class at exactly `level` (one entry per template), as loadouts: a one-hander with
// the class's shield of that level if it uses one, an either-hand weapon in both hands, a two-hander alone.
std::vector<Loadout> loadouts(std::uint8_t cls, std::uint16_t level, const vg::ItemTemplate* shield)
{
    std::vector<Loadout> out;
    for (const auto& t : items().templates())
    {
        if (!t.isWeapon() || t.level != level || !t.allowsClass(cls) || t.rarity == vg::Rarity::Unique)
            continue;
        Loadout l;
        l.label = t.name;
        l.pieces.push_back({EquipSlot::MainHand, &t});
        if (t.slot == vg::SlotRule::EitherHand)
            l.pieces.push_back({EquipSlot::OffHand, &t});
        else if (t.slot == vg::SlotRule::MainHand && shield)
            l.pieces.push_back({EquipSlot::OffHand, shield});
        out.push_back(std::move(l));
    }
    return out;
}

// The class's best (highest defence) sold-or-dropped shield of that level, nullptr if none.
const vg::ItemTemplate* shieldAt(std::uint8_t cls, std::uint16_t level)
{
    const vg::ItemTemplate* best = nullptr;
    for (const auto& t : items().templates())
        if (t.kind == vg::ItemKind::Shield && t.allowsClass(cls) && t.level == level && t.rarity != vg::Rarity::Unique &&
            (!best || t.defense > best->defense))
            best = &t;
    return best;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Data: the new keys
// ---------------------------------------------------------------------------------------------

TEST_CASE("Equipment: weapon speed, reach, damage range and bonus lines parse", "[game][items][equipment]")
{
    vg::ItemCatalog c;
    REQUIRE(parseError(fileWith(kHalberd + kRing + kEarring), &c).empty());
    const vg::ItemTemplate* h = c.find(10);
    REQUIRE(h);
    CHECK(h->attack == 16);
    CHECK(h->attackSpread == 3);
    CHECK(h->attackMin() == 13);
    CHECK(h->attackMax() == 19);
    CHECK(h->speedMs == 2300);
    CHECK(h->reach == Approx(3.3f));
    CHECK(h->bonusHp == 20);
    CHECK(h->bonusCritPermille == 5);
    CHECK(h->hasBonusLines());
    const vg::ItemTemplate* ring = c.find(11);
    REQUIRE(ring);
    CHECK(ring->isAccessory());
    CHECK(ring->slot == vg::SlotRule::Ring);
    CHECK(ring->model.empty()); // never drawn
    CHECK(ring->bonusMp == 15);
    CHECK(ring->bonusDodgePermille == 7);
    CHECK(ring->attributes[vg::attributeIndex(vg::Attribute::Strength)] == 2);
    CHECK_FALSE(vg::isUpgradable(*ring)); // §5: accessories are not upgraded
    CHECK(vg::isUpgradable(*h));
    // Without the new keys a weapon keeps one number and the class's speed / range.
    CHECK(c.find(1)->attackSpread == 0);
    CHECK(c.find(1)->speedMs == 0);
    CHECK(c.find(1)->reach == 0.0f);
    CHECK(vg::itemKindName(vg::ItemKind::Accessory) == "accessory");
    CHECK(vg::itemKindDisplayName(vg::ItemKind::Accessory) == "Takı");
    CHECK(vg::slotRuleName(vg::SlotRule::Earring) == "earring");
    CHECK(vg::slotRuleDisplayName(vg::SlotRule::Ring) == "Yüzük");
    CHECK(vg::slotRuleDisplayName(vg::SlotRule::Necklace) == "Kolye");
    CHECK(vg::slotRuleDisplayName(vg::SlotRule::Belt) == "Kemer");
    CHECK(vg::equipSlotName(EquipSlot::Ring2) == "ring2");
    CHECK(vg::equipSlotName(EquipSlot::Earring1) == "earring1");
    CHECK(vg::isAccessorySlot(EquipSlot::Necklace));
    CHECK(vg::isAccessorySlot(EquipSlot::Earring2));
    CHECK_FALSE(vg::isAccessorySlot(EquipSlot::Feet));
}

TEST_CASE("Equipment: the loader refuses broken new keys", "[game][items][equipment]")
{
    const std::string h = fileWith(kHalberd);
    CHECK(contains(parseError(replaced(h, "damage = [13, 19]", "damage = [12, 19]")), "low + high = 2 x attack"));
    CHECK(contains(parseError(replaced(h, "damage = [13, 19]", "damage = [19, 13]")), "'damage' must be"));
    CHECK(contains(parseError(replaced(h, "damage = [13, 19]", "damage = [13]")), "'damage' must be"));
    CHECK(contains(parseError(replaced(h, "damage = [13, 19]", "damage = \"13-19\"")), "'damage' must be"));
    CHECK(contains(parseError(replaced(h, "speed_ms = 2300", "speed_ms = 700")), "out of range"));
    CHECK(contains(parseError(replaced(h, "speed_ms = 2300", "speed_ms = 3100")), "out of range"));
    CHECK(contains(parseError(replaced(h, "reach = 3.3", "reach = 4.5")), "out of range"));
    CHECK(contains(parseError(replaced(h, "reach = 3.3", "reach = \"far\"")), "must be a number"));
    CHECK(contains(parseError(replaced(h, "max_hp = 20", "max_hp = 1001")), "out of range"));
    CHECK(contains(parseError(replaced(h, "crit = 5", "crit = 101")), "out of range"));
    CHECK(contains(parseError(replaced(h, "crit = 5", "crit = -1")), "out of range"));
    CHECK(parseError(replaced(h, "reach = 3.3", "reach = 3")).empty()); // an integer reach is fine
    // Weapon-only keys on other kinds.
    const std::string r = fileWith(kRing);
    CHECK(contains(parseError(replaced(r, "dodge = 7", "dodge = 7\nspeed_ms = 1500")), "only allowed for a weapon"));
    CHECK(contains(parseError(replaced(r, "dodge = 7", "dodge = 7\ndamage = [0, 0]")), "only allowed for a weapon"));
    CHECK(contains(parseError(replaced(r, "dodge = 7", "dodge = 7\nreach = 2.0")), "only allowed for a weapon"));
    // Accessories: no model, no grip, no attack, an accessory slot only; armour / weapons not in accessory slots.
    CHECK(contains(parseError(replaced(r, "icon = \"acc_ring_band\"", "icon = \"x\"\nmodel = \"x\"")),
                   "'model' is not allowed for an accessory"));
    CHECK(contains(parseError(replaced(r, "icon = \"acc_ring_band\"", "icon = \"x\"\ngrip = \"sword\"")),
                   "'grip' is not allowed for an accessory"));
    CHECK(contains(parseError(replaced(r, "attack = 0", "attack = 2")), "an accessory has no attack"));
    CHECK(contains(parseError(replaced(r, "slot = \"ring\"", "slot = \"head\"")), "does not fit kind"));
    CHECK(contains(parseError(replaced(h, "slot = \"two_hand\"", "slot = \"ring\"")), "does not fit kind"));
    CHECK(contains(parseError(replaced(r, "icon = \"acc_ring_band\"\n", "")), "missing 'icon'"));
    CHECK(contains(parseError(replaced(r, "kind = \"accessory\"", "kind = \"trinket\"")), "unknown kind"));
    CHECK(contains(parseError(replaced(r, "slot = \"ring\"", "slot = \"toe\"")), "unknown slot"));
    // Bonus lines are for equippable items only.
    const std::string material = R"(
[[item]]
id = 13
name = "Toz"
kind = "material"
icon = "ash_dust"
max_hp = 5
)";
    CHECK(contains(parseError(fileWith(material)), "'max_hp' is not allowed for a material"));
    // A kit may equip an accessory into its slot ("necklace", "ring1"); the old "reserved" name is gone.
    CHECK(parseError(replaced(fileWith(kRing), "equip = { main_hand = 1 }", "equip = { main_hand = 1, ring2 = 11 }"))
              .empty());
    CHECK(contains(parseError(replaced(fileWith(kRing), "equip = { main_hand = 1 }", "equip = { main_hand = 1, earring1 = 11 }")),
                   "cannot be equipped"));
    CHECK(contains(parseError(replaced(fileWith(kRing), "equip = { main_hand = 1 }", "equip = { main_hand = 1, reserved = 11 }")),
                   "unknown equipment slot"));
}

// ---------------------------------------------------------------------------------------------
// Rules: the accessory slots
// ---------------------------------------------------------------------------------------------

TEST_CASE("Equipment: accessories go into their own slots, rings and earrings into either of two",
          "[game][items][equipment]")
{
    vg::ItemCatalog c;
    REQUIRE(parseError(fileWith(kHalberd + kRing + kEarring), &c).empty());
    const vg::ItemUser warrior{kWarrior, 1};
    const vg::ItemTemplate& ring = *c.find(11);
    const vg::ItemTemplate& earring = *c.find(12);
    CHECK(vg::checkEquip(ring, EquipSlot::Ring1, warrior) == R::Ok);
    CHECK(vg::checkEquip(ring, EquipSlot::Ring2, warrior) == R::Ok);
    for (const auto s : {EquipSlot::MainHand, EquipSlot::OffHand, EquipSlot::Head, EquipSlot::Necklace, EquipSlot::Belt,
                         EquipSlot::Earring1, EquipSlot::Earring2})
        CHECK(vg::checkEquip(ring, s, warrior) == R::WrongSlot);
    CHECK(vg::checkEquip(earring, EquipSlot::Earring2, warrior) == R::Ok);
    CHECK(vg::checkEquip(earring, EquipSlot::Ring1, warrior) == R::WrongSlot);
    CHECK(vg::checkEquip(*c.find(10), EquipSlot::Ring1, warrior) == R::WrongSlot);

    // Quick move: the first free ring slot, then the second; both full: the first (a swap).
    std::vector<vg::StoredItem> rows{{1, ItemPlace::bag(0), 11, 1}, {2, ItemPlace::bag(1), 11, 1}, {3, ItemPlace::bag(2), 11, 1},
                                     {4, ItemPlace::bag(3), 12, 1}};
    vg::Inventory inv;
    std::string error;
    REQUIRE(vg::Inventory::fromItems(rows, inv, error));
    CHECK(vg::quickMoveTarget(c, inv, ItemPlace::bag(0)) == eq(EquipSlot::Ring1));
    auto plan = vg::planItemMove(c, inv, warrior, ItemPlace::bag(0), eq(EquipSlot::Ring1));
    REQUIRE(plan.result == R::Ok);
    vg::applyItemMove(inv, plan);
    CHECK(vg::quickMoveTarget(c, inv, ItemPlace::bag(1)) == eq(EquipSlot::Ring2));
    plan = vg::planItemMove(c, inv, warrior, ItemPlace::bag(1), eq(EquipSlot::Ring2));
    REQUIRE(plan.result == R::Ok);
    vg::applyItemMove(inv, plan);
    CHECK(vg::quickMoveTarget(c, inv, ItemPlace::bag(2)) == eq(EquipSlot::Ring1));
    CHECK(vg::quickMoveTarget(c, inv, ItemPlace::bag(3)) == eq(EquipSlot::Earring1));
    // Ring1 <-> Ring2 swaps; a ring onto an earring slot is WrongSlot and changes nothing.
    CHECK(vg::planItemMove(c, inv, warrior, eq(EquipSlot::Ring1), eq(EquipSlot::Ring2)).result == R::Ok);
    CHECK(vg::planItemMove(c, inv, warrior, ItemPlace::bag(2), eq(EquipSlot::Earring1)).result == R::WrongSlot);
    // Unequipping goes to the first free bag slot.
    CHECK(vg::quickMoveTarget(c, inv, eq(EquipSlot::Ring2)) == ItemPlace::bag(0));
    // The visible equipment never includes accessories (they are not drawn).
    const vg::VisibleEquipment v = vg::visibleEquipment(c, inv);
    CHECK(v.mainHand == 0);
    CHECK(v.armor == vg::ArmorAppearance{});
}

// ---------------------------------------------------------------------------------------------
// Derived stats
// ---------------------------------------------------------------------------------------------

TEST_CASE("Equipment: the main-hand weapon sets the interval, the swing factor and the melee reach",
          "[game][combat][equipment]")
{
    vg::ItemCatalog c;
    REQUIRE(parseError(fileWith(kHalberd + kRing + kEarring), &c).empty());
    const vg::Stats bare = vg::deriveStats(kWarrior, 5, {});
    CHECK(bare.attackIntervalMs == 1600);
    CHECK(bare.swingFactor == 1.0f);
    CHECK(bare.attackRange == Approx(2.5f));
    CHECK(bare.attackMin == bare.attack);
    CHECK(bare.attackMax == bare.attack);

    const vg::Inventory inv = [&] {
        vg::Inventory i;
        std::string e;
        REQUIRE(vg::Inventory::fromItems({{1, eq(EquipSlot::MainHand), 10, 1}, {2, eq(EquipSlot::Ring1), 11, 1},
                                          {3, eq(EquipSlot::Ring2), 11, 1}, {4, eq(EquipSlot::Earring1), 12, 1}},
                                         i, e));
        return i;
    }();
    const vg::EquipBonus b = vg::equipmentBonus(c, inv);
    CHECK(b.attack == 16);
    CHECK(b.attackSpread == 3);
    CHECK(b.swingIntervalMs == 2300);
    CHECK(b.reach == Approx(3.3f));
    CHECK(b.defense == 2);
    CHECK(b.maxHp == 20);
    CHECK(b.maxMp == 30);
    CHECK(b.critPermille == 15);
    CHECK(b.dodgePermille == 14);
    CHECK(b.attributes[vg::attributeIndex(vg::Attribute::Strength)] == 4);

    const vg::Stats s = vg::deriveStats(kWarrior, 5, b);
    CHECK(s.attackIntervalMs == 2300);
    CHECK(s.swingFactor == Approx(2300.0f / 1600.0f));
    CHECK(s.attackRange == Approx(3.3f));
    CHECK(s.attackMin < s.attack);
    CHECK(s.attackMax > s.attack);
    // The range scales like the weapon's attack: spread x (1 + STR / 20) on each side.
    const double scale = 1.0 + (12.0 + 4.0) / 20.0;
    CHECK(static_cast<double>(s.attackMax - s.attackMin) == Approx(2.0 * 3.0 * scale).margin(1.0));
    // The bonus lines feed the totals and are reported.
    vg::EquipBonus noLines = b;
    noLines.maxHp = noLines.maxMp = noLines.critPermille = noLines.dodgePermille = 0;
    const vg::Stats without = vg::deriveStats(kWarrior, 5, noLines);
    CHECK(s.maxHp == without.maxHp + 20);
    CHECK(s.maxMp == without.maxMp + 30);
    CHECK(s.critPermille == without.critPermille + 15);
    CHECK(s.dodgePermille == without.dodgePermille + 14);
    CHECK(s.bonusHp == 20);
    CHECK(s.bonusMp == 30);
    CHECK(s.bonusCritPermille == 15);
    CHECK(s.bonusDodgePermille == 14);
    CHECK(s.hpRegen > without.hpRegen); // regeneration follows the larger pool

    // Caps still hold.
    vg::EquipBonus huge = b;
    huge.critPermille = 2000;
    huge.dodgePermille = 2000;
    const vg::Stats capped = vg::deriveStats(kWarrior, 5, huge);
    CHECK(capped.critPermille == vg::kMaxCritPermille);
    CHECK(capped.dodgePermille == vg::kMaxDodgePermille);
    // A Mage's basic attack stays a 14 m spell whatever the weapon's reach; its speed counts.
    vg::EquipBonus staff{};
    staff.attack = 7;
    staff.swingIntervalMs = 1500;
    staff.reach = 2.9f;
    const vg::Stats mage = vg::deriveStats(kMage, 5, staff);
    CHECK(mage.attackRange == Approx(14.0f));
    CHECK(mage.attackIntervalMs == 1500);
    CHECK(mage.swingFactor == Approx(1500.0f / 1800.0f));
    // Haste shortens the interval but keeps the swing factor (a real gain).
    vg::EffectModifiers haste;
    haste.hastePct = 25.0f;
    const vg::Stats hasted = vg::applyEffectModifiers(s, haste);
    CHECK(hasted.attackIntervalMs == 1840);
    CHECK(hasted.swingFactor == s.swingFactor);
    // An off-hand weapon adds its attack but not its speed.
    vg::Inventory off;
    std::string e;
    REQUIRE(vg::Inventory::fromItems({{1, eq(EquipSlot::OffHand), 2, 1}}, off, e));
    CHECK(vg::equipmentBonus(c, off).swingIntervalMs == 0);
}

TEST_CASE("Equipment: a damage range rolls between its ends; one number is the old formula",
          "[game][combat][equipment]")
{
    // min == max: exactly the M9 formula.
    for (const float v : {0.0f, 0.37f, 0.999f})
        for (const float crit : {0.0f, 0.9f})
        {
            const vg::Hit a = vg::computeDamage(40, 1.25f, 10, 0.5f, v, crit);
            const vg::Hit b = vg::computeDamage(40, 40, 1.25f, 10, 0.5f, v, crit);
            CHECK(a.amount == b.amount);
            CHECK(a.critical == b.critical);
        }
    // 30..50 (middle 40): the roll covers [26, 54] (10 % of the middle beyond each end).
    CHECK(vg::rolledAttackLow(30, 50) == Approx(26.0f));
    CHECK(vg::rolledAttackHigh(30, 50) == Approx(54.0f));
    CHECK(vg::computeDamage(30, 50, 1.0f, 0, 0.0f, 0.0f, 1.0f).amount == 26);
    CHECK(vg::computeDamage(30, 50, 1.0f, 0, 0.0f, 1.0f, 1.0f).amount == 54);
    CHECK(vg::computeDamage(30, 50, 1.0f, 0, 0.0f, 0.5f, 1.0f).amount == 40);
    // The multiplier (a skill, or a weapon's swing factor) applies before defence; a crit after.
    CHECK(vg::computeDamage(30, 50, 2.0f, 20, 0.0f, 0.5f, 1.0f).amount == 70);
    const vg::Hit crit = vg::computeDamage(30, 50, 2.0f, 20, 0.5f, 0.5f, 0.1f);
    CHECK(crit.critical);
    CHECK(crit.amount == 105);
    // Damage per second of basic attacks: the swing factor cancels the interval (pre-defence).
    vg::Stats fast;
    fast.attack = 100;
    fast.attackIntervalMs = 1200;
    fast.swingFactor = 0.75f;
    fast.critPermille = 0;
    vg::Stats slow = fast;
    slow.attackIntervalMs = 2400;
    slow.swingFactor = 1.5f;
    CHECK(vg::basicDamagePerSecond(fast, 0) == Approx(vg::basicDamagePerSecond(slow, 0)));
    CHECK(vg::basicDamagePerSecond(slow, 40) > vg::basicDamagePerSecond(fast, 40)); // defence costs fast hits more
}

TEST_CASE("Equipment: v12 CHAR_STATS carries the range, reach, swing factor and bonus lines",
          "[net][messages][equipment]")
{
    vn::CharStats m;
    m.level = 7;
    m.attack = 52;
    m.attackMin = 47;
    m.attackMax = 58;
    m.reachCm = 330;
    m.swingPermille = 1438;
    m.bonusHp = 45;
    m.bonusMp = 30;
    m.bonusCritPermille = 15;
    m.bonusDodgePermille = 9;
    m.attackIntervalMs = 2300;
    const auto bytes = vn::encodeMessage(m);
    REQUIRE(bytes);
    CHECK(bytes->size() == 64);
    vn::CharStats back;
    REQUIRE(vn::decodeMessage(*bytes, back));
    CHECK(back == m);
    // Readers reject a bonus permille above 1000; a short packet (a v11 sender) fails.
    vn::CharStats bad = m;
    bad.bonusCritPermille = 1001;
    const auto badBytes = vn::encodeMessage(bad);
    REQUIRE(badBytes);
    CHECK_FALSE(vn::decodeMessage(*badBytes, back));
    std::vector<std::uint8_t> v11(bytes->begin(), bytes->begin() + 48);
    CHECK_FALSE(vn::decodeMessage(v11, back));
    CHECK(vn::kProtocolVersion == 12);
    CHECK(vn::kItemEquipmentSlots == 13);
    CHECK(vn::kMaxInventoryEntries == 41);
}

// ---------------------------------------------------------------------------------------------
// The shipped content
// ---------------------------------------------------------------------------------------------

TEST_CASE("Equipment content: every weapon has its speed, melee weapons their reach, families keep their tempo",
          "[game][items][content][equipment]")
{
    const auto& c = items();
    CHECK(c.size() >= 300);
    std::map<std::string, std::set<std::uint32_t>> speedsByModel;
    for (const auto& t : c.templates())
    {
        INFO(t.id << " " << t.name);
        if (t.isWeapon())
        {
            CHECK(t.speedMs >= 1200);
            CHECK(t.speedMs <= 2400);
            const bool mageOnly = t.classMask == (1u << kMage);
            CHECK((t.reach > 0.0f) == !mageOnly); // a Mage's staff casts at 14 m, no melee reach
            CHECK(t.attackSpread >= 0);
            CHECK(t.attackMin() >= 1);
            speedsByModel[t.model].insert(t.speedMs);
        }
        else
        {
            CHECK(t.speedMs == 0);
            CHECK(t.reach == 0.0f);
            CHECK(t.attackSpread == 0);
        }
        if (t.isAccessory())
        {
            CHECK(t.model.empty());
            CHECK(t.attack == 0);
            CHECK(t.defense <= 5);
            CHECK(t.hasBonusLines());
            CHECK_FALSE(vg::isUpgradable(t));
            CHECK(t.classMask == 0x1E); // every class
        }
    }
    // The family tempo (docs/DEVELOPMENT_PLAN.md §39.2): daggers fastest, the two-handers and the halberd slowest.
    const auto speed = [&](const std::string& n) { return named(n).speedMs; };
    CHECK(speed("Sınır Hançeri") < speed("Sınır Palası"));
    CHECK(speed("Sınır Palası") < speed("Sınır Kılıcı"));
    CHECK(speed("Sınır Kılıcı") < speed("Sınır Topuzu"));
    CHECK(speed("Sınır Topuzu") < speed("Sağlam Büyük Kılıç"));
    CHECK(speed("Sağlam Büyük Kılıç") < speed("Sınır Baltası"));
    CHECK(speed("Sınır Baltası") < speed("Sağlam Teber"));
    CHECK(speed("Sağlam Teber") < speed("Sağlam Savaş Çekici"));
    CHECK(speed("Sağlam Billur Asa") < speed("Kıvılcım Asası"));
    // Reach: the halberd and the rod reach farther than a sword, a dagger shortest.
    CHECK(named("Sağlam Teber").reach > named("Sınır Kılıcı").reach);
    CHECK(named("Çiy Değneği").reach > named("Sınır Kılıcı").reach);
    CHECK(named("Sınır Hançeri").reach < named("Sınır Kılıcı").reach);
    // All 22 weapon and shield models are used by at least one template (the 14 forged ones included).
    std::set<std::string> models;
    for (const auto& t : c.templates())
        if (t.isWeapon() || t.kind == vg::ItemKind::Shield)
            models.insert(t.model);
    for (const char* m : {"sword_soldier", "sword_keep", "sword_ridge", "sword_curved", "sword_broad", "axe_hearth",
                          "axe_bearded", "hammer_war", "halberd", "mace_sun", "mace_flanged", "mace_spiked",
                          "dagger_hunter", "dagger_fang", "dagger_twin", "staff_ember", "staff_frost", "staff_ritual",
                          "staff_crystal", "shield_oak", "shield_tower", "shield_buckler"})
    {
        INFO(m);
        CHECK(models.contains(m));
    }
    // Two-handers block the off hand (M8 rule, still in force).
    vg::Inventory inv = inventoryOf({{ItemPlace::bag(0), named("Sağlam Teber").id}, {eq(EquipSlot::OffHand), 2}});
    const auto plan = vg::planItemMove(c, inv, {kWarrior, 5}, ItemPlace::bag(0), eq(EquipSlot::MainHand));
    REQUIRE(plan.result == R::Ok);
    vg::applyItemMove(inv, plan);
    CHECK(inv.equipped(EquipSlot::OffHand).empty());
    CHECK(vg::planItemMove(c, inv, {kWarrior, 5}, ItemPlace::bag(0), eq(EquipSlot::OffHand)).result == R::SlotBlocked);
}

TEST_CASE("Equipment content: the weapon families are sidegrades at equal level (damage per second band)",
          "[game][items][content][equipment][balance]")
{
    // Levels where several families share a tier: 5 (Sınır / Sağlam), 17 (Kor / Korlu), 19 (Ant).
    for (const std::uint16_t level : std::initializer_list<std::uint16_t>{5, 17, 19})
    {
        const std::uint32_t defense = monsterDefense(level);
        for (const std::uint8_t cls : {kWarrior, kRogue, kMage, kPriest})
        {
            const vg::ItemTemplate* shield = shieldAt(cls, level);
            std::map<std::string, std::vector<std::pair<std::string, double>>> groups; // by handedness
            for (const Loadout& l : loadouts(cls, level, shield))
            {
                const vg::ItemTemplate& w = *l.pieces.front().second;
                const vg::Stats s = statsOf(cls, level, l);
                const double dps = vg::basicDamagePerSecond(s, defense);
                std::string key = w.slot == vg::SlotRule::TwoHand ? "two_hand" : "one_hand";
                // A priest's rod is a two-hander; the Mage's staves are compared together.
                groups[key].push_back({l.label, dps});
            }
            for (const auto& [key, list] : groups)
            {
                if (list.size() < 2)
                    continue;
                double lo = 1e9, hi = 0.0;
                for (const auto& [n, d] : list)
                {
                    lo = std::min(lo, d);
                    hi = std::max(hi, d);
                }
                INFO("level " << level << " class " << int(cls) << " " << key);
                for (const auto& [n, d] : list)
                    INFO(n << " " << d);
                // Within 15 % of each other; a one-hander + shield of the warrior / priest within 20 % (the maces
                // trade damage for Dayanıklılık / İrade lines, docs/DEVELOPMENT_PLAN.md §39.4).
                const bool maces = key == "one_hand" && (cls == kWarrior || cls == kPriest);
                CHECK(lo / hi >= (maces ? 0.80 : 0.85));
            }
            // A warrior's two-hander out-damages sword + shield by 10-60 % (it gives up the shield's defence; a slow
            // swing also loses less to the monster's defence per second).
            if (cls == kWarrior && groups.contains("two_hand") && groups.contains("one_hand"))
            {
                double best1 = 0.0, best2 = 0.0;
                for (const auto& [n, d] : groups["one_hand"])
                    best1 = std::max(best1, d);
                for (const auto& [n, d] : groups["two_hand"])
                    best2 = std::max(best2, d);
                INFO("level " << level);
                CHECK(best2 / best1 >= 1.1);
                CHECK(best2 / best1 <= 1.6);
            }
        }
    }
    // The feel differs: a hammer hits far harder per swing than a dagger at the same level, a halberd reaches farther.
    const vg::Stats hammer = statsOf(kWarrior, 17, {"", {{EquipSlot::MainHand, &named("Korlu Savaş Çekici")}}});
    const vg::Stats sword = statsOf(kWarrior, 17, {"", {{EquipSlot::MainHand, &named("Kor Kılıcı")}}});
    CHECK(hammer.attack * hammer.swingFactor > 1.5 * sword.attack * sword.swingFactor);
    CHECK(hammer.attackIntervalMs > sword.attackIntervalMs);
}

TEST_CASE("Equipment content: each class has a weapon, a full armour set and every accessory slot in each band",
          "[game][items][content][equipment]")
{
    const auto& c = items();
    for (const std::uint8_t cls : {kWarrior, kRogue, kMage, kPriest})
        for (std::uint16_t lo = 1; lo <= 17; lo += 4)
        {
            INFO("class " << int(cls) << " levels " << lo << "-" << lo + 3);
            const auto has = [&](auto pred) {
                for (const auto& t : c.templates())
                    if (t.allowsClass(cls) && t.level >= lo && t.level < lo + 4 && t.rarity != vg::Rarity::Unique &&
                        pred(t))
                        return true;
                return false;
            };
            CHECK(has([](const vg::ItemTemplate& t) { return t.isWeapon(); }));
            for (const auto slot : {vg::SlotRule::Head, vg::SlotRule::Chest, vg::SlotRule::Legs, vg::SlotRule::Hands,
                                    vg::SlotRule::Feet, vg::SlotRule::Ring, vg::SlotRule::Earring, vg::SlotRule::Necklace,
                                    vg::SlotRule::Belt})
            {
                INFO(vg::slotRuleName(slot));
                CHECK(has([slot](const vg::ItemTemplate& t) { return t.slot == slot; }));
            }
        }
    // Higher tiers of armour carry bonus lines (Ayazlı 9 on: the chest at least).
    for (const char* n : {"Ayazlı Göğüs Zırhı", "Yeminli Deri Yelek", "Korlu Cübbe", "Ant Göğüs Zırhı"})
    {
        INFO(n);
        CHECK(named(n).hasBonusLines());
        CHECK((named(n).bonusHp + named(n).bonusMp + named(n).bonusDodgePermille) > 0);
    }
}

TEST_CASE("Equipment content: requirements can be met at the item's level, and only top tiers have them",
          "[game][items][content][equipment]")
{
    int withRequirements = 0;
    for (const auto& t : items().templates())
    {
        if (!t.hasRequirements())
            continue;
        ++withRequirements;
        INFO(t.name);
        CHECK(t.level >= 17);
        for (std::uint8_t cls = 1; cls <= 4; ++cls)
        {
            if (!t.allowsClass(cls))
                continue;
            // Half of the level's points into the required attribute is enough.
            const vg::Attributes start = vg::classStartAttributes(cls);
            for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
                if (t.requirements[i] > 0)
                {
                    INFO("class " << int(cls) << " attribute " << i);
                    CHECK(start[i] + vg::pointsForLevel(t.level) / 2 >= t.requirements[i]);
                    CHECK(start[i] < t.requirements[i]); // never met without spending a point
                }
        }
    }
    CHECK(withRequirements >= 30);
    // An unmet requirement refuses the move (AttributeTooLow) after the class and level checks.
    const vg::ItemTemplate& axe = named("Ant Baltası");
    const vg::ItemUser fresh{kWarrior, 19, vg::baseAttributes(kWarrior, {})};
    CHECK(vg::checkEquip(axe, EquipSlot::MainHand, fresh) == R::AttributeTooLow);
    vg::Attributes spent{};
    spent[vg::attributeIndex(vg::Attribute::Strength)] = 13;
    CHECK(vg::checkEquip(axe, EquipSlot::MainHand, {kWarrior, 19, vg::baseAttributes(kWarrior, spent)}) == R::Ok);
    CHECK(vg::checkEquip(axe, EquipSlot::MainHand, {kWarrior, 18, vg::baseAttributes(kWarrior, spent)}) == R::LevelTooLow);
}

TEST_CASE("Equipment content: every template has a source, shops sell only basic tiers, accessories drop",
          "[game][items][content][equipment]")
{
    const auto& c = items();
    std::set<vg::ItemTemplateId> shop, dropped, rewarded, kit;
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
        if (const auto* k = c.kitFor(cls))
        {
            for (const auto id : k->equip)
                kit.insert(id);
            for (const auto id : k->bag)
                kit.insert(id);
        }
    for (const auto& n : vg::npcs().npcs())
        for (const auto id : n.stock)
            shop.insert(id);
    for (const auto& t : loot().tables())
        for (const auto& d : t.drops)
            dropped.insert(d.item);
    vg::QuestCatalog questCatalog;
    std::string questError;
    INFO(questError);
    REQUIRE(vg::loadQuestCatalog(*vg::locateQuestsFile(), &c, &vg::npcs(), &monsters(), questCatalog, questError));
    for (const auto& q : questCatalog.quests())
    {
        for (const auto& it : q.reward.items)
            rewarded.insert(it.templateId);
        for (const auto id : q.reward.classItems)
            if (id != 0)
                rewarded.insert(id);
        for (const auto id : q.reward.choice)
            rewarded.insert(id);
    }
    for (const auto& t : c.templates())
    {
        INFO(t.id << " " << t.name);
        CHECK((kit.contains(t.id) || shop.contains(t.id) || dropped.contains(t.id) || rewarded.contains(t.id)));
        if (shop.contains(t.id) && t.slot != vg::SlotRule::None && t.id >= 169)
            CHECK(t.rarity == vg::Rarity::Common); // better tiers come from loot, bosses and quests
        if (t.isAccessory() && t.rarity != vg::Rarity::Unique)
            CHECK(dropped.contains(t.id));
    }
    // Every new M18 item from 169 on drops somewhere or is sold (no quest-only gear).
    for (const auto& t : c.templates())
        if (t.id >= 169)
        {
            INFO(t.name);
            CHECK((dropped.contains(t.id) || shop.contains(t.id)));
        }
    // Quest rewards use M18 items (Sağlam weapons of the new families, Sağlam accessories).
    bool newReward = false;
    for (const auto id : rewarded)
        newReward = newReward || id >= 169;
    CHECK(newReward);
}

TEST_CASE("Equipment content: upgrades scale attack and defence only; bonus lines and the damage width stay",
          "[game][items][upgrade][equipment]")
{
    const vg::ItemTemplate& hammer = named("Korlu Savaş Çekici");
    vg::Inventory plain = inventoryOf({{eq(EquipSlot::MainHand), hammer.id}});
    std::vector<vg::StoredItem> rows{{1, eq(EquipSlot::MainHand), hammer.id, 1, 7}};
    vg::Inventory up;
    std::string error;
    REQUIRE(vg::Inventory::fromItems(rows, up, error));
    const vg::EquipBonus a = vg::equipmentBonus(items(), plain);
    const vg::EquipBonus b = vg::equipmentBonus(items(), up);
    CHECK(b.attack == vg::upgradedAttack(hammer, 7));
    CHECK(b.attack > a.attack);
    CHECK(b.attackSpread == a.attackSpread);
    CHECK(b.attributes == a.attributes);
    CHECK(b.maxHp == a.maxHp);
    CHECK(b.swingIntervalMs == a.swingIntervalMs);
}

TEST_CASE("Equipment: balance table (VO_ECONOMY_TABLE=1 prints it)", "[game][items][equipment][economy]")
{
    const auto env = vo::core::Config::processEnv("VO_ECONOMY_TABLE");
    const bool print = env && *env == "1";
    // Per class and level: the best weapon set of the level (dps against the level's toughest ordinary
    // monster), the armour of the matching tier (defence) and the accessories of the tier.
    struct Row
    {
        std::uint16_t level;
        const char* tier;
        const char* armour; // armour tier prefix
        const char* acc;    // accessory tier prefix ("" none)
    };
    const Row rows[] = {{1, "1", "Eski", ""},       {5, "5", "Sağlam", "Eski"},  {9, "9", "Ayazlı", "Sağlam"},
                        {13, "13", "Yeminli", "Ayazlı"}, {17, "17", "Korlu", "Yeminli"}, {19, "19", "Ant", "Korlu"}};
    const auto armourOf = [](std::uint8_t cls, const char* prefix) {
        std::vector<const vg::ItemTemplate*> out;
        const char* line = cls == kWarrior || cls == kPriest ? "heavy" : (cls == kRogue ? "leather" : "cloth");
        for (const auto& t : items().templates())
            if (t.isArmor() && t.allowsClass(cls) && t.name.starts_with(prefix) && t.model.starts_with(line))
                out.push_back(&t);
        return out;
    };
    if (print)
        std::printf("class    lvl  weapon                    ms   hit   dps(def)  def  hp   (monster def)\n");
    for (const std::uint8_t cls : {kWarrior, kRogue, kMage, kPriest})
        for (const Row& r : rows)
        {
            const vg::ItemTemplate* shield = shieldAt(cls, r.level);
            std::vector<Loadout> ls = loadouts(cls, r.level, shield);
            if (ls.empty() && r.level == 1)
                ls = loadouts(cls, 1, shieldAt(cls, 1));
            const std::uint32_t defense = monsterDefense(r.level);
            for (Loadout& l : ls)
            {
                for (const vg::ItemTemplate* a : armourOf(cls, r.armour))
                    l.pieces.push_back({static_cast<EquipSlot>(static_cast<int>(a->slot) - static_cast<int>(vg::SlotRule::Head) +
                                                               static_cast<int>(EquipSlot::Head)),
                                        a});
                const vg::Stats s = statsOf(cls, r.level, l);
                CHECK(s.attackIntervalMs >= 1200);
                if (print)
                    std::printf("%-8s %3u  %-24s %4u %5.0f %8.1f  %4u %4u (%u)\n",
                                std::string(vg::className(static_cast<vg::CharClass>(cls))).c_str(), r.level, l.label.c_str(),
                                s.attackIntervalMs, s.attack * s.swingFactor, vg::basicDamagePerSecond(s, defense), s.defense,
                                s.maxHp, defense);
            }
        }
}
