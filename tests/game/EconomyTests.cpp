// Economy check (DEVELOPMENT_PLAN.md §32): Akçe income per hour by level against the price of what a
// character of that level can buy, from the shipped gamedata (monsters.toml, loot.toml, items.toml,
// npcs.toml) and the shared formulas (deriveStats, computeDamage, regeneration, xpToNext). No content is
// changed by this file; it only measures.
//
// Model (expected values, one class at a time, basic attacks only - skills add roughly a quarter to the
// damage and shorten every fight a little):
//   - every stat point goes to the class's attack attribute; the gear is the best main hand (and for a
//     warrior the best shield) any merchant sells for the character's level, bought the level it unlocks;
//     M15: also the best armour piece of every armour slot a merchant sells (chests and legs; heads,
//     hands and feet only drop and are left out: the model stays a lower bound);
//   - the hunted monster is the non-elite template that gives the most xp per hour (after the
//     level-difference reduction, xpForKill) among those at most one level above the character;
//   - a kill cycle is the fight, 8 s of walking / looting and (rest mode) sitting until the hp lost in
//     the fight is back (6 s out-of-combat delay, then twice the standing regeneration), or (potion
//     mode) drinking the hp potion with the best hp per Akçe instead of sitting, within its 10 s cooldown;
//   - income per kill = average coins + every drop line's chance x average count x its sell price.
//
// VO_ECONOMY_TABLE=1 prints the tables (copied into DEVELOPMENT_PLAN.md §32).

#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/game/Npcs.h"
#include "shared/game/Shop.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace vg = vo::game;

namespace {

constexpr double kWalkAndLootSeconds = 8.0;
constexpr double kMonsterCritChance = 0.05;

double expectedHit(double attack, std::uint32_t defense, double critChance)
{
    const auto a = static_cast<std::uint32_t>(std::lround(std::max(attack, 0.0)));
    const double normal = vg::computeDamage(a, 1.0f, defense, 0.0f, 0.5f, 1.0f).amount;
    const double crit = vg::computeDamage(a, 1.0f, defense, 1.0f, 0.5f, 0.0f).amount;
    return normal * (1.0 - critChance) + crit * critChance;
}

// Everything any merchant sells (npcs.toml stock).
std::set<vg::ItemTemplateId> merchantStock()
{
    std::set<vg::ItemTemplateId> out;
    for (const auto& n : vg::npcs().npcs())
        out.insert(n.stock.begin(), n.stock.end());
    return out;
}

// The best buyable item of `kind` / `slot` for the class at `level` (most attack, then defense), or null.
const vg::ItemTemplate* bestGear(const vg::ItemCatalog& items, const std::set<vg::ItemTemplateId>& stock,
                                 std::uint8_t charClass, std::uint16_t level, const vg::Attributes& base, bool shield)
{
    const vg::ItemTemplate* best = nullptr;
    for (const auto& t : items.templates())
    {
        if (t.id == 0 || !stock.contains(t.id) || !t.allowsClass(charClass) || t.level > level)
            continue;
        bool meets = true; // attribute requirements against class start + allocated (PROTOCOL_STATS.md §5)
        for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
            meets = meets && base[i] >= t.requirements[i];
        if (!meets)
            continue;
        if (shield ? t.kind != vg::ItemKind::Shield : (t.kind != vg::ItemKind::Weapon || t.slot == vg::SlotRule::OffHand))
            continue;
        if (t.rarity == vg::Rarity::Unique)
            continue;
        if (!shield && charClass == 1 && t.isTwoHanded())
            continue; // warriors keep sword and shield (their starter kit)
        const auto score = [&](const vg::ItemTemplate* x) { return shield ? x->defense : x->attack; };
        if (!best || score(&t) > score(best))
            best = &t;
    }
    return best;
}

// M15: the best sold armour piece for `slot` (most defense, then attributes), or null.
const vg::ItemTemplate* bestArmor(const vg::ItemCatalog& items, const std::set<vg::ItemTemplateId>& stock,
                                  std::uint8_t charClass, std::uint16_t level, vg::SlotRule slot)
{
    const vg::ItemTemplate* best = nullptr;
    for (const auto& t : items.templates())
        if (t.kind == vg::ItemKind::Armor && t.slot == slot && stock.contains(t.id) && t.allowsClass(charClass) &&
            t.level <= level && (!best || t.defense > best->defense))
            best = &t;
    return best;
}

struct LevelRow
{
    std::uint16_t level = 0;
    const vg::MonsterTemplate* target = nullptr;
    double ttk = 0.0;             // seconds per fight
    double damageTaken = 0.0;     // hp lost per fight
    double incomePerKill = 0.0;   // Akçe
    double killsPerHourRest = 0.0;
    double akcePerHourRest = 0.0;
    double hoursAtLevel = 0.0;    // in rest mode (0 at the cap)
    double akceAtLevel = 0.0;     // earned while at this level (rest mode)
    std::uint32_t gearCost = 0;   // gear unlocked at this level
    std::string gear;
    double potionAkcePerHour = 0.0; // potion mode: spent on potions
    double akcePerHourPotion = 0.0; // potion mode: earned (gross)
    double potionShare = 0.0;       // spent / earned in potion mode
};

std::vector<LevelRow> simulate(std::uint8_t charClass, const vg::ItemCatalog& items, const vg::MonsterData& monsters,
                               const vg::LootTables& loot)
{
    const auto stock = merchantStock();
    // The hp potion with the most hp per Akçe (no level requirement on potions).
    const vg::ItemTemplate* potion = nullptr;
    for (const auto& t : items.templates())
        if (t.isConsumable() && t.restoreHp > 0 && stock.contains(t.id) &&
            (!potion || static_cast<double>(t.restoreHp) / t.value > static_cast<double>(potion->restoreHp) / potion->value))
            potion = &t;
    REQUIRE(potion != nullptr);
    const vg::ClassTable* table = vg::classTable(charClass);
    REQUIRE(table != nullptr);

    std::vector<LevelRow> rows;
    // Start with the starter kit (PROTOCOL_ITEMS.md §1.3); anything better is bought.
    const vg::StarterKit* kit = items.kitFor(charClass);
    REQUIRE(kit != nullptr);
    const vg::ItemTemplate* ownedWeapon = items.find(kit->equip[static_cast<std::size_t>(vg::EquipSlot::MainHand)]);
    const vg::ItemTemplate* ownedShield = items.find(kit->equip[static_cast<std::size_t>(vg::EquipSlot::OffHand)]);
    if (ownedShield && ownedShield->kind != vg::ItemKind::Shield)
        ownedShield = nullptr;
    static constexpr std::array<vg::SlotRule, 5> kArmorSlots = {vg::SlotRule::Head, vg::SlotRule::Chest, vg::SlotRule::Legs,
                                                                vg::SlotRule::Hands, vg::SlotRule::Feet};
    std::array<const vg::ItemTemplate*, 5> ownedArmor{};
    for (std::uint16_t level = 1; level <= vg::kLevelCap; ++level)
    {
        LevelRow r;
        r.level = level;
        vg::Attributes allocated{};
        allocated[vg::attributeIndex(table->attackAttribute)] = static_cast<std::uint16_t>(vg::kPointsPerLevel * (level - 1));
        const vg::Attributes base = vg::deriveStats(charClass, level, allocated, {}).base;
        const vg::ItemTemplate* weapon = bestGear(items, stock, charClass, level, base, false);
        const vg::ItemTemplate* shield = charClass == 1 ? bestGear(items, stock, charClass, level, base, true) : nullptr;
        if (weapon && weapon != ownedWeapon && (!ownedWeapon || weapon->attack > ownedWeapon->attack))
        {
            r.gearCost += weapon->value;
            r.gear += weapon->name;
            ownedWeapon = weapon;
        }
        if (shield && shield != ownedShield && (!ownedShield || shield->defense > ownedShield->defense))
        {
            r.gearCost += shield->value;
            r.gear += (r.gear.empty() ? "" : " + ") + shield->name;
            ownedShield = shield;
        }
        for (std::size_t a = 0; a < kArmorSlots.size(); ++a)
        {
            const vg::ItemTemplate* piece = bestArmor(items, stock, charClass, level, kArmorSlots[a]);
            if (piece && piece != ownedArmor[a] && (!ownedArmor[a] || piece->defense > ownedArmor[a]->defense))
            {
                r.gearCost += piece->value;
                r.gear += (r.gear.empty() ? "" : " + ") + piece->name;
                ownedArmor[a] = piece;
            }
        }
        vg::EquipBonus equip;
        for (const vg::ItemTemplate* g : {ownedWeapon, ownedShield, ownedArmor[0], ownedArmor[1], ownedArmor[2],
                                          ownedArmor[3], ownedArmor[4]})
            if (g)
            {
                equip.attack += g->attack;
                equip.defense += g->defense;
                for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
                    equip.attributes[i] = static_cast<std::uint16_t>(equip.attributes[i] + g->attributes[i]);
            }
        const vg::Stats s = vg::deriveStats(charClass, level, allocated, equip);
        const double sitRegen = std::max(vg::hpRegenPerSecond(s, true), 0.01f);

        // The non-elite monster with the most xp per hour, at most one level above.
        double bestXpPerHour = -1.0;
        for (const auto& m : monsters.templates())
        {
            if (m.id == 0 || m.elite || m.level > level + 1)
                continue;
            const double dps = expectedHit(s.attack, m.defense, s.critChance) * 1000.0 / s.attackIntervalMs;
            const double ttk = m.hp / dps;
            const double taken = expectedHit(m.attack, s.defense, kMonsterCritChance) * (1.0 - s.dodgeChance) * ttk *
                                 1000.0 / m.attackIntervalMs;
            if (taken >= s.maxHp)
                continue; // loses the fight
            const double rest = taken > 0.0 ? 6.0 + taken / sitRegen : 0.0;
            const double cycle = ttk + kWalkAndLootSeconds + rest;
            double income = 0.0;
            if (const vg::LootTable* lt = loot.forMonster(m.id))
            {
                income += (lt->coinsMin + lt->coinsMax) / 2.0;
                for (const auto& d : lt->drops)
                    if (const vg::ItemTemplate* it = items.find(d.item))
                        income += d.chance * (d.minCount + d.maxCount) / 2.0 * vg::sellPrice(*it);
            }
            // At the level cap no experience comes any more: the best hunt is the one paying the most.
            const double xpPerHour = level >= vg::kLevelCap ? 3600.0 / cycle * income
                                                            : 3600.0 / cycle * vg::xpForKill(m.xp, level, m.level);
            if (xpPerHour <= bestXpPerHour)
                continue;
            bestXpPerHour = xpPerHour;
            r.target = &m;
            r.ttk = ttk;
            r.damageTaken = taken;
            r.killsPerHourRest = 3600.0 / cycle;
            r.incomePerKill = income;
            // Potion mode: no sitting; potions cover the loss within their shared cooldown.
            const double potionCycle = ttk + kWalkAndLootSeconds;
            const double potionsPerKill = taken / potion->restoreHp;
            const double maxPerKill = potionCycle / (vg::kConsumableCooldownMs / 1000.0);
            const double drunk = std::min(potionsPerKill, maxPerKill);
            const double leftover = std::max(0.0, taken - drunk * potion->restoreHp);
            const double cycleP = potionCycle + (leftover > 0.0 ? 6.0 + leftover / sitRegen : 0.0);
            r.akcePerHourPotion = 3600.0 / cycleP * income;
            r.potionAkcePerHour = 3600.0 / cycleP * drunk * potion->value;
        }
        REQUIRE(r.target != nullptr);
        r.akcePerHourRest = r.killsPerHourRest * r.incomePerKill;
        const std::uint32_t toNext = vg::xpToNext(level);
        r.hoursAtLevel = toNext == 0 ? 0.0 : toNext / (r.killsPerHourRest * vg::xpForKill(r.target->xp, level, r.target->level));
        r.akceAtLevel = r.hoursAtLevel * r.akcePerHourRest;
        r.potionShare = r.akcePerHourPotion > 0.0 ? r.potionAkcePerHour / r.akcePerHourPotion : 0.0;
        rows.push_back(r);
    }
    return rows;
}

} // namespace

TEST_CASE("Economy: Akce income per hour by level against what a character can buy", "[game][economy]")
{
    vg::ItemCatalog items;
    vg::MonsterData monsters;
    vg::LootTables loot;
    std::string error;
    const auto itemsFile = vg::locateItemsFile();
    const auto monstersFile = vg::locateMonstersFile();
    const auto lootFile = vg::locateLootFile();
    REQUIRE(itemsFile);
    REQUIRE(monstersFile);
    REQUIRE(lootFile);
    REQUIRE(vg::loadItemCatalog(*itemsFile, items, error));
    REQUIRE(vg::loadMonsterData(*monstersFile, monsters, error));
    REQUIRE(vg::loadLootTables(*lootFile, items, &monsters, loot, error));
    REQUIRE_FALSE(vg::npcs().empty());

    char* env = nullptr;
    std::size_t envLength = 0;
    const bool print = _dupenv_s(&env, &envLength, "VO_ECONOMY_TABLE") == 0 && env != nullptr;
    std::free(env);
    const char* names[] = {"", "Savaşçı", "Haydut", "Büyücü", "Rahip"};
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
    {
        const auto rows = simulate(cls, items, monsters, loot);
        double total = 0.0, gear = 0.0;
        for (const auto& r : rows)
        {
            CHECK(r.akcePerHourRest > 0.0);
            total += r.akceAtLevel;
            gear += r.gearCost;
        }
        // M15: everything the merchants sell for levels 1-20 costs a fraction of what levels 1-20 earn (the
        // Akçe sink of the next milestone is still missing), and the income keeps growing with the level.
        CHECK(total > 3.0 * gear);
        for (const std::size_t a : {std::size_t{10}, std::size_t{14}, std::size_t{18}})
            CHECK(rows[a].akcePerHourRest > rows[a - 6].akcePerHourRest);
        // Every level has a monster to hunt at most one level above (the slots of levels 5-19 exist).
        for (const auto& r : rows)
            CHECK(r.target->level + 3 >= std::min<std::uint16_t>(r.level, 19));
        if (!print)
            continue;
        std::printf("\n%s (class %u): level | target (lvl) | fight s | hp lost | Akçe/kill | kills/h | Akçe/h | h at level | "
                    "Akçe at level | gear unlocked (Akçe) | potion mode: Akçe/h earned, spent on potions (share)\n",
                    names[cls], cls);
        for (const auto& r : rows)
            std::printf("  %2u | %s (%u) | %5.1f | %5.0f | %5.1f | %5.0f | %6.0f | %5.2f | %6.0f | %s (%u) | %6.0f, %5.0f (%3.0f %%)\n",
                        r.level, r.target->name.c_str(), r.target->level, r.ttk, r.damageTaken, r.incomePerKill,
                        r.killsPerHourRest, r.akcePerHourRest, r.hoursAtLevel, r.akceAtLevel,
                        r.gear.empty() ? "-" : r.gear.c_str(), r.gearCost, r.akcePerHourPotion, r.potionAkcePerHour,
                        r.potionShare * 100.0);
        std::printf("  total earned 1-20: %.0f Akçe in %.1f h; gear bought: %.0f Akçe\n", total,
                    [&] {
                        double h = 0.0;
                        for (const auto& r : rows)
                            h += r.hoursAtLevel;
                        return h;
                    }(),
                    gear);
    }
}
