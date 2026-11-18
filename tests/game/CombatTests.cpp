// Shared combat rules (shared/game/Combat.h, docs/PROTOCOL_COMBAT.md §1.1-1.2) and the monster data
// loader (shared/game/Monsters.h, §1.3): formulas with injected rolls, the level curve, experience
// reduction, stats with equipment, the skill table, the tuning target against the shipped
// gamedata files, and the strict loader.

#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Monsters.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace vg = vo::game;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

TEST_CASE("Combat: level curve 80 * level^1.6 rounded to 10, 0 at the cap", "[game][combat]")
{
    CHECK(vg::kLevelCap == 20); // M15
    CHECK(vg::xpToNext(1) == 80);
    CHECK(vg::xpToNext(2) == 240);
    CHECK(vg::xpToNext(3) == 460);
    CHECK(vg::xpToNext(4) == 740);
    CHECK(vg::xpToNext(5) == 1050);
    CHECK(vg::xpToNext(9) == 2690);
    CHECK(vg::xpToNext(20) == 0);
    CHECK(vg::xpToNext(0) == 0);
    // Below level 10 exactly the M9 curve (stored xp of existing characters stays valid).
    for (std::uint16_t l = 1; l < vg::kSteepCurveFrom; ++l)
    {
        CHECK(vg::xpToNext(l) % 10 == 0);
        CHECK(std::abs(static_cast<double>(vg::xpToNext(l)) - 80.0 * std::pow(l, 1.6)) <= 5.0);
    }
}

TEST_CASE("Combat: levels 10-19 use the steeper M15 curve and keep growing", "[game][combat][levels]")
{
    // 80 * L^1.6 * (1 + 0.6 * (L - 9)), rounded to 10 (docs/PROTOCOL_LEVELS.md §1).
    CHECK(vg::xpToNext(10) == 5100);
    CHECK(vg::xpToNext(11) == 8160);
    CHECK(vg::xpToNext(15) == 28030);
    CHECK(vg::xpToNext(19) == 62260);
    std::uint64_t total = 0;
    for (std::uint16_t l = 1; l < vg::kLevelCap; ++l)
    {
        INFO(l);
        CHECK(vg::xpToNext(l) % 10 == 0);
        if (l >= vg::kSteepCurveFrom)
            CHECK(std::abs(static_cast<double>(vg::xpToNext(l)) -
                           80.0 * std::pow(l, 1.6) * (1.0 + 0.6 * (l - (vg::kSteepCurveFrom - 1)))) <= 5.0);
        if (l > 1)
            CHECK(vg::xpToNext(l) > vg::xpToNext(static_cast<std::uint16_t>(l - 1)));
        total += vg::xpToNext(l);
    }
    CHECK(total == 295'010); // 10 710 to level 10, 284 300 from 10 to 20

    // Stat points and skill points per level stay as they were: level 20 has 57 and 19.
    CHECK(vg::pointsForLevel(20) == 57);
    CHECK(vg::pointsForLevel(99) == 57);
}

TEST_CASE("Combat: experience - full up to 2 levels above, -20 % per further level, 0 from 7", "[game][combat]")
{
    CHECK(vg::xpForKill(100, 1, 1) == 100);
    CHECK(vg::xpForKill(100, 1, 6) == 100); // lower level than the monster: full
    CHECK(vg::xpForKill(100, 3, 1) == 100); // 2 above
    CHECK(vg::xpForKill(100, 4, 1) == 80);
    CHECK(vg::xpForKill(100, 5, 1) == 60);
    CHECK(vg::xpForKill(100, 6, 1) == 40);
    CHECK(vg::xpForKill(100, 7, 1) == 20);
    CHECK(vg::xpForKill(100, 8, 1) == 0);
    CHECK(vg::xpForKill(100, 9, 1) == 0);
    CHECK(vg::xpForKill(25, 5, 1) == 15);
    CHECK(vg::xpForKill(100, vg::kLevelCap, 10) == 0); // nothing at the cap

    // Carry-over into several level-ups, stop at the cap with xp 0.
    CHECK(vg::addExperience({1, 70}, 25).level == 2);
    CHECK(vg::addExperience({1, 70}, 25).xp == 15);
    const auto many = vg::addExperience({1, 0}, 80 + 240 + 460 + 5);
    CHECK(many.level == 4);
    CHECK(many.xp == 5);
    const auto capped = vg::addExperience({9, 2000}, 1'000'000);
    CHECK(capped.level == vg::kLevelCap);
    CHECK(capped.xp == 0);
    CHECK(vg::addExperience({20, 0}, 50).xp == 0);
    // M15: a level-10 character (xp 0 at the old cap) gains experience again.
    CHECK(vg::addExperience({10, 0}, 50).xp == 50);
    CHECK(vg::addExperience({10, 0}, 5100).level == 11);
}

TEST_CASE("Combat: stats from class, level and equipment", "[game][combat]")
{
    const auto w1 = vg::deriveStats(1, 1, {6, 4});
    CHECK(w1.maxHp == 90);
    CHECK(w1.maxMp == 34);
    CHECK(w1.attack == 18);
    CHECK(w1.defense == 7);
    CHECK(w1.attackRange == 2.5f);
    CHECK(w1.attackIntervalMs == 1600);
    const auto w10 = vg::deriveStats(1, 10, {6, 4});
    CHECK(w10.maxHp == 270);
    CHECK(w10.attack == 36);
    CHECK(vg::deriveStats(1, 50) == vg::deriveStats(1, vg::kLevelCap)); // clamped
    CHECK(vg::deriveStats(1, 0) == vg::deriveStats(1, 1));
    CHECK(vg::deriveStats(3, 1).attackRange == 14.0f);
    CHECK(vg::deriveStats(3, 1).attackIntervalMs == 1800);
    CHECK(vg::deriveStats(2, 1).attackIntervalMs == 1200);
    CHECK(vg::deriveStats(2, 1).critChance == 0.15f);
    CHECK(vg::deriveStats(4, 1).attackIntervalMs == 1700);
    CHECK(vg::deriveStats(4, 1).critChance == 0.05f);
    CHECK(vg::deriveStats(1, 1, {-100, -100}).attack == 0); // never below 0

    // Equipment bonus: every item in the Equipment container, hidden ones count as nothing.
    vg::ItemCatalog catalog;
    std::string error;
    REQUIRE(vg::loadItemCatalog(*vg::locateItemsFile(), catalog, error));
    vg::Inventory inv;
    REQUIRE(vg::Inventory::fromItems({{1, vg::ItemPlace::equipment(vg::EquipSlot::MainHand), 1, 1},
                                      {2, vg::ItemPlace::equipment(vg::EquipSlot::OffHand), 2, 1},
                                      {3, vg::ItemPlace::bag(0), 6, 1},
                                      {4, vg::ItemPlace::bag(1), 999, 1}},
                                     inv, error));
    CHECK(vg::equipmentBonus(catalog, inv) == vg::EquipBonus{6, 4});
    vg::Inventory daggers;
    REQUIRE(vg::Inventory::fromItems({{1, vg::ItemPlace::equipment(vg::EquipSlot::MainHand), 3, 1},
                                      {2, vg::ItemPlace::equipment(vg::EquipSlot::OffHand), 3, 1}},
                                     daggers, error));
    CHECK(vg::equipmentBonus(catalog, daggers) == vg::EquipBonus{8, 0});
    CHECK(vg::equipmentBonus(catalog, vg::Inventory{}) == vg::EquipBonus{});
}

TEST_CASE("Combat: damage formula with injected rolls", "[game][combat]")
{
    // max(1, round(attack * mult * (0.9 + 0.2 * roll) - defense * 0.5)), x1.5 when critical.
    CHECK(vg::computeDamage(18, 1.0f, 4, 0.05f, 0.5f, 0.9f).amount == 16);
    CHECK_FALSE(vg::computeDamage(18, 1.0f, 4, 0.05f, 0.5f, 0.9f).critical);
    CHECK(vg::computeDamage(18, 1.0f, 4, 0.05f, 0.0f, 0.9f).amount == 14); // 16.2 - 2
    CHECK(vg::computeDamage(18, 1.0f, 4, 0.05f, 1.0f, 0.9f).amount == 18); // 19.8 - 2
    const auto crit = vg::computeDamage(18, 1.0f, 4, 0.05f, 0.5f, 0.01f);
    CHECK(crit.critical);
    CHECK(crit.amount == 24); // 16 x 1.5
    CHECK(vg::computeDamage(18, 2.2f, 4, 0.05f, 0.5f, 0.9f).amount == 38); // Ağır Darbe
    CHECK(vg::computeDamage(1, 1.0f, 100, 0.05f, 0.5f, 0.9f).amount == 1); // never below 1
    CHECK(vg::computeDamage(0, 1.0f, 0, 0.0f, 0.5f, 0.0f).amount == 1);
    // A seeded generator stays inside the bounds and crits about as often as it should.
    std::mt19937 rng(7);
    int crits = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const auto h = vg::rollDamage(18, 1.0f, 4, 0.15f, rng);
        crits += h.critical ? 1 : 0;
        if (h.critical)
            CHECK((h.amount >= 21 && h.amount <= 27));
        else
            CHECK((h.amount >= 14 && h.amount <= 18));
    }
    CHECK((crits > 2700 && crits < 3300)); // 15 %
}

TEST_CASE("Combat: skill table (one per class) and regeneration", "[game][combat]")
{
    const auto* heavy = vg::findSkill(1);
    REQUIRE(heavy);
    CHECK(heavy->name == "Ağır Darbe");
    CHECK(heavy->charClass == 1);
    CHECK(heavy->mpCost == 12);
    CHECK(heavy->cooldownMs == 6000);
    CHECK(heavy->castMs == 0);
    CHECK(heavy->multiplier == 2.2f);
    const auto* twin = vg::findSkill(2);
    REQUIRE(twin);
    CHECK(twin->hits == 2);
    CHECK(twin->multiplier == 1.2f);
    const auto* bolt = vg::findSkill(3);
    REQUIRE(bolt);
    CHECK(bolt->range == 16.0f);
    CHECK(bolt->castMs == 600);
    CHECK(bolt->projectile);
    const auto* sun = vg::findSkill(4);
    REQUIRE(sun);
    CHECK(sun->target == vg::SkillTarget::SelfOrFriendly);
    CHECK(sun->castMs == 800);
    // M11 (PROTOCOL_STATS.md §2): 19 % x (1 + (INT + SPI) / 60) of the target's max hp.
    CHECK(sun->healFraction == 0.19f);
    CHECK(vg::healAmount(*sun, 90) == 17);
    CHECK(vg::healAmount(*sun, 90, 1.3f) == 22);
    CHECK(vg::healAmount(*sun, 95, vg::deriveStats(4, 1).healMultiplier) == 23); // v4: 24
    CHECK(vg::healAmount(*sun, 1) == 1);
    CHECK(vg::healPermille(*sun, 1.3f) == 247);
    CHECK(vg::findSkill(0) == nullptr);
    CHECK(vg::findSkill(29) == nullptr); // M12: ids 5..28 are the new skills of gamedata/skills.toml
    for (std::uint8_t c = 1; c <= 4; ++c)
    {
        REQUIRE(vg::classSkill(c));
        CHECK(vg::classSkill(c)->charClass == c);
    }
    CHECK(vg::classSkill(9) == nullptr);
    // Regeneration: 2 % max_hp x (1 + VIT / 100), 3 % max_mp x (1 + SPI / 100), doubled sitting.
    const vg::Stats w = vg::deriveStats(1, 1, {6, 4}); // 90 hp, VIT 11; 34 mp, SPI 3
    CHECK_THAT(vg::hpRegenPerSecond(w, false), WithinAbs(90 * 0.02 * 1.11, 1e-4));
    CHECK_THAT(vg::hpRegenPerSecond(w, true), WithinAbs(2 * 90 * 0.02 * 1.11, 1e-4));
    CHECK_THAT(vg::mpRegenPerSecond(w, false), WithinAbs(34 * 0.03 * 1.03, 1e-4));
    CHECK(vg::inRange(3.0f, 2.5f));
    CHECK_FALSE(vg::inRange(3.01f, 2.5f));
}

TEST_CASE("Combat: tuning target - a level-1 starter kit vs a level-1 ash thrall", "[game][combat][tuning]")
{
    vg::ItemCatalog items;
    vg::MonsterData monsters;
    std::string error;
    REQUIRE(vg::loadItemCatalog(*vg::locateItemsFile(), items, error));
    INFO(error);
    REQUIRE(vg::loadMonsterData(*vg::locateMonstersFile(), monsters, error));
    const vg::MonsterTemplate* thrall = nullptr;
    for (const auto& m : monsters.templates())
        if (m.model == "ash_thrall" && m.level == 1)
            thrall = &m;
    REQUIRE(thrall);
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
    {
        INFO("class " << int(cls));
        const vg::StarterKit* kit = items.kitFor(cls);
        REQUIRE(kit);
        vg::EquipBonus bonus;
        for (const auto id : kit->equip)
            if (const auto* t = items.find(id))
            {
                bonus.attack += t->attack;
                bonus.defense += t->defense;
            }
        const vg::Stats s = vg::deriveStats(cls, 1, bonus);
        // Average hit (variance roll 0.5, no crit) both ways.
        const auto dealt = vg::computeDamage(s.attack, 1.0f, thrall->defense, 0.0f, 0.5f, 1.0f).amount;
        const auto taken = vg::computeDamage(thrall->attack, 1.0f, s.defense, 0.0f, 0.5f, 1.0f).amount;
        const double hitsToKill = std::ceil(static_cast<double>(thrall->hp) / dealt);
        const double hitsSurvived = std::floor(static_cast<double>(s.maxHp - 1) / taken);
        CHECK((hitsToKill >= 4 && hitsToKill <= 6));
        CHECK((hitsSurvived >= 8 && hitsSurvived <= 13));
    }
    // Levels 1 -> 3 take a few minutes: 320 xp are 13 level-1 thralls at most.
    CHECK(vg::xpToNext(1) + vg::xpToNext(2) <= 13 * thrall->xp);
}

// ---- Attributes and stat points (docs/PROTOCOL_STATS.md) ----

namespace {

vg::Attributes pts(std::uint16_t str, std::uint16_t dex, std::uint16_t vit, std::uint16_t intl, std::uint16_t spi)
{
    return vg::Attributes{str, dex, vit, intl, spi};
}

} // namespace

TEST_CASE("Stats: starting spreads, points per level, unspent points", "[game][stats]")
{
    CHECK(vg::classStartAttributes(1) == pts(12, 6, 11, 3, 3));
    CHECK(vg::classStartAttributes(2) == pts(6, 13, 8, 4, 4));
    CHECK(vg::classStartAttributes(3) == pts(3, 5, 6, 13, 8));
    CHECK(vg::classStartAttributes(4) == pts(5, 3, 9, 8, 10));
    CHECK(vg::classStartAttributes(9) == vg::classStartAttributes(1)); // unknown: the Warrior's
    for (std::uint8_t c = 1; c <= 4; ++c)
        CHECK(vg::allocatedTotal(vg::classStartAttributes(c)) == 35); // the same sum for every class
    CHECK(vg::classTable(1)->attackAttribute == vg::Attribute::Strength);
    CHECK(vg::classTable(2)->attackAttribute == vg::Attribute::Dexterity);
    CHECK(vg::classTable(3)->attackAttribute == vg::Attribute::Intelligence);
    CHECK(vg::classTable(4)->attackAttribute == vg::Attribute::Strength);

    CHECK(vg::pointsForLevel(1) == 0);
    CHECK(vg::pointsForLevel(2) == 3);
    CHECK(vg::pointsForLevel(10) == 27);
    CHECK(vg::pointsForLevel(0) == 0);   // clamped
    CHECK(vg::pointsForLevel(99) == 57); // clamped (M15: cap 20)
    CHECK(vg::unspentPoints(7, {}) == 18); // a character from before M11: everything unspent
    CHECK(vg::unspentPoints(7, pts(5, 0, 3, 0, 0)) == 10);
    CHECK(vg::unspentPoints(2, pts(5, 0, 3, 0, 0)) == 0); // more spent than the level allows: 0, kept
    CHECK(vg::baseAttributes(1, pts(5, 0, 3, 0, 0)) == pts(17, 6, 14, 3, 3));

    CHECK(vg::attributeDisplayName(vg::Attribute::Strength) == "Güç");
    CHECK(vg::attributeDisplayName(vg::Attribute::Dexterity) == "Çeviklik");
    CHECK(vg::attributeDisplayName(vg::Attribute::Vitality) == "Dayanıklılık");
    CHECK(vg::attributeDisplayName(vg::Attribute::Intelligence) == "Zekâ");
    CHECK(vg::attributeDisplayName(vg::Attribute::Spirit) == "İrade");
    CHECK(vg::attributeKey(vg::Attribute::Spirit) == "spirit");
}

TEST_CASE("Stats: allocation checks (InvalidAmount, NotEnoughPoints, AttributeCap)", "[game][stats]")
{
    using R = vo::net::StatAllocateResult;
    CHECK(vg::checkAllocation(1, 10, {}, {}) == R::InvalidAmount);
    CHECK(vg::checkAllocation(1, 1, {}, pts(1, 0, 0, 0, 0)) == R::NotEnoughPoints); // level 1 has none
    CHECK(vg::checkAllocation(1, 2, {}, pts(1, 1, 1, 0, 0)) == R::Ok);
    CHECK(vg::checkAllocation(1, 2, {}, pts(2, 1, 1, 0, 0)) == R::NotEnoughPoints);
    CHECK(vg::checkAllocation(1, 10, pts(20, 0, 0, 0, 0), pts(7, 0, 0, 0, 0)) == R::Ok);
    CHECK(vg::checkAllocation(1, 10, pts(20, 0, 0, 0, 0), pts(7, 1, 0, 0, 0)) == R::NotEnoughPoints);
    CHECK(vg::checkAllocation(1, 10, {}, pts(65535, 0, 0, 0, 0)) == R::NotEnoughPoints);
    // The cap (255 for class start + allocated) only matters for stored values beyond what
    // levels give (an imported or admin-edited row): base 12 + 243 = 255 is the most.
    CHECK(vg::checkAllocation(1, 10, pts(243, 0, 0, 0, 0), pts(1, 0, 0, 0, 0)) == R::NotEnoughPoints);
    CHECK(vg::checkAllocation(1, 10, pts(0, 0, 0, 0, 0), pts(0, 0, 0, 0, 0)) == R::InvalidAmount);
    // Order: InvalidAmount, NotEnoughPoints, AttributeCap. With 27 points nothing reaches 255, so
    // the cap is checked with a synthetic allocation that leaves points free.
    CHECK(vg::checkAllocation(1, 10, pts(243, 0, 0, 0, 0), {}) == R::InvalidAmount);
}

TEST_CASE("Stats: every attribute feeds its derived values", "[game][stats]")
{
    const vg::EquipBonus kit{6, 4}; // Er Kılıcı + Meşe Kalkan
    const vg::Stats base = vg::deriveStats(1, 10, {}, kit);
    CHECK(base.base == vg::classStartAttributes(1));
    CHECK(base.unspent == 27);
    CHECK(base.critPermille == 50);
    CHECK(base.dodgePermille == 24);
    CHECK(base.dodgeChance == 0.024f);

    // Güç: the Warrior's attack, W x (1 + P / 20) + P / 3 -> +0.3 + 0.333 per point with a 6-attack sword.
    const vg::Stats str = vg::deriveStats(1, 10, pts(9, 0, 0, 0, 0), kit);
    CHECK(str.attack == 41); // round(2 + 20 + 6 x 2.05 + 7) = 41.3
    CHECK(str.maxHp == base.maxHp);
    CHECK(str.unspent == 18);
    // ... but not the Rogue's (Çeviklik) or the Mage's (Zekâ).
    CHECK(vg::deriveStats(2, 10, pts(9, 0, 0, 0, 0), {8, 0}).attack == vg::deriveStats(2, 10, {}, {8, 0}).attack);
    CHECK(vg::deriveStats(2, 10, pts(0, 9, 0, 0, 0), {8, 0}).attack > vg::deriveStats(2, 10, {}, {8, 0}).attack);
    CHECK(vg::deriveStats(3, 10, pts(0, 0, 0, 9, 0), {7, 0}).attack > vg::deriveStats(3, 10, {}, {7, 0}).attack);
    CHECK(vg::deriveStats(4, 10, pts(9, 0, 0, 0, 0), {5, 0}).attack > vg::deriveStats(4, 10, {}, {5, 0}).attack);

    // Çeviklik: +0.5 % crit, +0.4 % dodge per point; caps 50 % / 25 %.
    const vg::Stats dex = vg::deriveStats(1, 10, pts(0, 10, 0, 0, 0), kit);
    CHECK(dex.critPermille == 100);
    CHECK(dex.dodgePermille == 64);
    CHECK(dex.attack == base.attack);
    const vg::Stats agile = vg::deriveStats(2, 10, pts(0, 200, 0, 0, 0), {});
    CHECK(agile.critPermille == vg::kMaxCritPermille);
    CHECK(agile.dodgePermille == vg::kMaxDodgePermille);
    CHECK(agile.critChance == 0.5f);
    CHECK(agile.dodgeChance == 0.25f);

    // Dayanıklılık: +5 max hp, +0.2 defense, +1 % hp regeneration per point.
    const vg::Stats vit = vg::deriveStats(1, 10, pts(0, 0, 10, 0, 0), kit);
    CHECK(vit.maxHp == base.maxHp + 50);
    CHECK(vit.defense == base.defense + 2);
    CHECK_THAT(vit.hpRegen, WithinAbs(vit.maxHp * 0.02 * (1.0 + 21 / 100.0), 1e-3));

    // Zekâ: +1 max mp, heal strength; İrade: +3 max mp, mp regeneration, heal strength.
    const vg::Stats intl = vg::deriveStats(4, 10, pts(0, 0, 0, 6, 0), {});
    const vg::Stats spi = vg::deriveStats(4, 10, pts(0, 0, 0, 0, 6), {});
    const vg::Stats priest = vg::deriveStats(4, 10, {}, {});
    CHECK(intl.maxMp == priest.maxMp + 6);
    CHECK(spi.maxMp == priest.maxMp + 18);
    CHECK(intl.healMultiplier == spi.healMultiplier);
    CHECK_THAT(spi.healMultiplier, WithinAbs(1.0 + 24 / 60.0, 1e-6));
    CHECK_THAT(spi.mpRegen, WithinAbs(spi.maxMp * 0.03 * (1.0 + 16 / 100.0), 1e-3));

    // Item bonuses count like base points (effective = base + bonus) but are reported apart.
    vg::EquipBonus bonus = kit;
    bonus.attributes = pts(9, 0, 0, 0, 0);
    const vg::Stats withItem = vg::deriveStats(1, 10, {}, bonus);
    CHECK(withItem.attack == str.attack);
    CHECK(withItem.base == base.base);
    CHECK(withItem.bonus == pts(9, 0, 0, 0, 0));
    CHECK(withItem.unspent == 27);
}

namespace {

struct Build
{
    const char* name;
    std::uint8_t charClass;
    std::uint16_t level;
    vg::Attributes allocated;
    std::vector<std::uint16_t> items; // equipped template ids
};

struct Duel
{
    double secondsToKill = 0.0; // the player kills the monster after (0 = never within the limit)
    double secondsToDie = 0.0;  // the monster kills the player after (0 = never within the limit)
    bool won = false;
    double damagePerSecond = 0.0;
};

vg::EquipBonus bonusOf(const vg::ItemCatalog& items, const std::vector<std::uint16_t>& ids)
{
    vg::EquipBonus b;
    for (const auto id : ids)
        if (const auto* t = items.find(id))
        {
            b.attack += t->attack;
            b.defense += t->defense;
            for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
                b.attributes[i] = static_cast<std::uint16_t>(b.attributes[i] + t->attributes[i]);
        }
    return b;
}

// Expected damage of one hit with average variance (roll 0.5) and the critical chance spread over
// the hits (expected value).
double expectedHit(std::uint32_t attack, float multiplier, std::uint32_t defense, float critChance)
{
    const double normal = vg::computeDamage(attack, multiplier, defense, 0.0f, 0.5f, 1.0f).amount;
    const double crit = vg::computeDamage(attack, multiplier, defense, 1.0f, 0.5f, 0.0f).amount;
    return normal * (1.0 - critChance) + crit * critChance;
}

// A straight fight in melee / casting range, 50 ms steps, with the server's timing rules: basic
// attacks on their interval, the class skill whenever it is ready (heals below 60 % hp, damage
// skills always), the 500 ms global cooldown, casts blocking swings, the monster's first attack
// after 400 ms. Damage as expected values (average variance, crits and dodges spread).
Duel duel(const vg::Stats& s, std::uint8_t charClass, const vg::MonsterTemplate& m)
{
    const vg::SkillDef* skill = vg::classSkill(charClass);
    REQUIRE(skill);
    const double basic = expectedHit(s.attack, 1.0f, m.defense, s.critChance);
    const double taken = expectedHit(m.attack, 1.0f, s.defense, 0.05f) * (1.0 - s.dodgeChance);
    double monsterHp = m.hp, hp = s.maxHp, mp = s.maxMp;
    double nextSwing = 0.0, gcd = 0.0, skillReady = 0.0, castEnds = -1.0, nextMonster = 0.4;
    double dealt = 0.0;
    Duel d;
    constexpr double kStep = 0.05, kLimit = 180.0;
    for (double t = 0.0; t < kLimit; t += kStep)
    {
        if (castEnds >= 0.0 && t >= castEnds)
        {
            castEnds = -1.0;
            if (skill->healFraction > 0.0f)
                hp = std::min<double>(s.maxHp, hp + vg::healAmount(*skill, s.maxHp, s.healMultiplier));
            else
            {
                const double h = expectedHit(s.attack, skill->multiplier, m.defense, s.critChance) * skill->hits;
                monsterHp -= h;
                dealt += h;
            }
        }
        const bool wantSkill = skill->healFraction > 0.0f ? hp < 0.6 * s.maxHp : true;
        if (castEnds < 0.0 && wantSkill && t >= skillReady && t >= gcd && mp >= skill->mpCost)
        {
            mp -= skill->mpCost;
            skillReady = t + skill->cooldownMs / 1000.0;
            gcd = t + 0.5;
            if (skill->castMs > 0)
                castEnds = t + skill->castMs / 1000.0;
            else
            {
                const double h = expectedHit(s.attack, skill->multiplier, m.defense, s.critChance) * skill->hits;
                monsterHp -= h;
                dealt += h;
            }
        }
        if (castEnds < 0.0 && t >= nextSwing && t >= gcd)
        {
            nextSwing = t + s.attackIntervalMs / 1000.0;
            gcd = t + 0.5;
            monsterHp -= basic;
            dealt += basic;
        }
        if (monsterHp <= 0.0)
        {
            d.secondsToKill = t;
            d.won = true;
            break;
        }
        if (t >= nextMonster)
        {
            nextMonster = t + m.attackIntervalMs / 1000.0;
            hp -= taken;
            if (hp <= 0.0)
            {
                d.secondsToDie = t;
                break;
            }
        }
    }
    const double elapsed = d.won ? d.secondsToKill : d.secondsToDie;
    d.damagePerSecond = elapsed > 0.0 ? dealt / elapsed : 0.0;
    if (d.won)
    {
        // How long the player would have lasted without killing it (same incoming damage).
        d.secondsToDie = 0.4 + std::floor((s.maxHp - 1e-9) / taken) * (m.attackIntervalMs / 1000.0);
    }
    return d;
}

} // namespace

TEST_CASE("Stats: tuning table - starting spreads match v4, sensible level-10 builds beat the elite warden",
          "[game][stats][tuning]")
{
    vg::ItemCatalog items;
    vg::MonsterData monsters;
    std::string error;
    REQUIRE(vg::loadItemCatalog(*vg::locateItemsFile(), items, error));
    REQUIRE(vg::loadMonsterData(*vg::locateMonstersFile(), monsters, error));
    const vg::MonsterTemplate* thrall = nullptr;
    const vg::MonsterTemplate* warden = nullptr;
    for (const auto& m : monsters.templates())
    {
        if (m.model == "ash_thrall" && m.level == 1)
            thrall = &m;
        if (m.name == "Kor Bekçisi")
            warden = &m;
    }
    REQUIRE(thrall);
    REQUIRE(warden);
    CHECK(warden->hp == 900);
    CHECK(warden->attack == 38);

    // v4 numbers at level 1 with the starter kit (PROTOCOL_STATS.md §2.2).
    struct V4
    {
        std::uint8_t cls;
        std::uint32_t hp, mp;
        std::uint16_t attack, defense, critPermille;
    };
    for (const V4 v : {V4{1, 90, 34, 18, 7, 50}, V4{2, 98, 35, 18, 2, 150}, V4{3, 90, 70, 18, 2, 50},
                       V4{4, 95, 64, 16, 3, 50}})
    {
        INFO("class " << int(v.cls));
        const vg::StarterKit* kit = items.kitFor(v.cls);
        REQUIRE(kit);
        std::vector<std::uint16_t> equipped;
        for (const auto id : kit->equip)
            if (id != 0)
                equipped.push_back(id);
        const vg::Stats s = vg::deriveStats(v.cls, 1, {}, bonusOf(items, equipped));
        CHECK(s.maxHp == v.hp);
        CHECK(s.maxMp == v.mp);
        CHECK(s.attack == v.attack);
        CHECK(s.defense == v.defense);
        CHECK(s.critPermille == v.critPermille);
        // Level 10, nothing spent: the v4 level-10 numbers (same per-level rates), +-1.
        const vg::ClassTable& t = *vg::classTable(v.cls);
        const vg::Stats s10 = vg::deriveStats(v.cls, 10, {}, bonusOf(items, equipped));
        CHECK(s10.maxHp == v.hp + 9 * static_cast<std::uint32_t>(t.hpPerLevel));
        CHECK(s10.maxMp == v.mp + 9 * static_cast<std::uint32_t>(t.mpPerLevel));
        CHECK(std::abs(static_cast<int>(s10.attack) - (v.attack + 18)) <= 1);
        CHECK(std::abs(static_cast<int>(s10.defense) - (v.defense + 9)) <= 1);
    }

    // Level-10 characters, unspent vs a sensible build, against the Kor Bekçisi. Gear: what drops
    // before it (Kömürlü Kılıç, Çömez Bıçağı, Cüruf Topuzu) and the kits (Meşe Kalkan, Kırağı Asası).
    const std::vector<Build> builds = {
        {"warrior L10 unspent", 1, 10, {}, {13, 2}},
        {"warrior L10 18 Güç 9 Dayanıklılık", 1, 10, pts(18, 0, 9, 0, 0), {13, 2}},
        {"rogue L10 unspent", 2, 10, {}, {12, 3}},
        {"rogue L10 18 Çeviklik 9 Dayanıklılık", 2, 10, pts(0, 18, 9, 0, 0), {12, 3}},
        {"mage L10 unspent", 3, 10, {}, {8}},
        {"mage L10 18 Zekâ 9 Dayanıklılık", 3, 10, pts(0, 0, 9, 18, 0), {8}},
        {"priest L10 unspent", 4, 10, {}, {14}},
        {"priest L10 9 Güç 9 Dayanıklılık 9 İrade", 4, 10, pts(9, 0, 9, 0, 9), {14}},
    };
    // VO_TUNING_TABLE=1 prints the table (docs/PROTOCOL_STATS.md §2.2, DEVELOPMENT_PLAN.md §20).
    char* env = nullptr;
    std::size_t envLength = 0;
    const bool print = _dupenv_s(&env, &envLength, "VO_TUNING_TABLE") == 0 && env != nullptr;
    std::free(env);
    if (print)
        std::printf("%-42s %4s %4s %4s %4s %5s %5s %7s %7s %6s %s\n", "build", "hp", "mp", "atk", "def", "crit",
                    "dodge", "kill_s", "die_s", "dps", "vs Kor Bekçisi");
    std::array<Duel, 8> results{};
    std::array<vg::Stats, 8> stats{};
    for (std::size_t i = 0; i < builds.size(); ++i)
    {
        const Build& b = builds[i];
        stats[i] = vg::deriveStats(b.charClass, b.level, b.allocated, bonusOf(items, b.items));
        results[i] = duel(stats[i], b.charClass, *warden);
        if (print)
            std::printf("%-42s %4u %4u %4u %4u %4.1f%% %4.1f%% %7.1f %7.1f %6.1f %s\n", b.name, stats[i].maxHp,
                        stats[i].maxMp, stats[i].attack, stats[i].defense, stats[i].critChance * 100.0f,
                        stats[i].dodgeChance * 100.0f, results[i].secondsToKill, results[i].secondsToDie,
                        results[i].damagePerSecond, results[i].won ? "wins" : "dies");
    }
    for (std::size_t i = 0; i < builds.size(); i += 2)
    {
        INFO(builds[i + 1].name);
        const Duel& unspent = results[i];
        const Duel& built = results[i + 1];
        // Investing makes a level-10 character clearly stronger: +25 % or more damage per second
        // or survivability (the builds above give roughly +30-50 %).
        const double dpsGain = built.damagePerSecond / unspent.damagePerSecond;
        const double hpGain = static_cast<double>(stats[i + 1].maxHp) / stats[i].maxHp;
        CHECK((dpsGain >= 1.25 || hpGain >= 1.25));
        CHECK(dpsGain <= 1.8); // ... but not absurdly
    }
    // The warden: beaten by a well-built level-10 Warrior, Rogue or Mage in a straight fight (the
    // Priest heals itself through a long fight), deadly for every unspent level-10 melee character.
    CHECK(results[1].won);
    CHECK(results[3].won);
    CHECK(results[5].won);
    CHECK(results[7].won);
    CHECK_FALSE(results[0].won);
    // ... and for low levels whatever they spend.
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
    {
        INFO("class " << int(cls));
        const vg::Attributes all = cls == 3 ? pts(0, 0, 0, 12, 0) : cls == 2 ? pts(0, 6, 6, 0, 0) : pts(6, 0, 6, 0, 0);
        const std::array<std::vector<std::uint16_t>, 5> gear{{{}, {13, 2}, {12, 3}, {8}, {14}}};
        const vg::Stats low = vg::deriveStats(cls, 5, all, bonusOf(items, gear[cls]));
        CHECK_FALSE(duel(low, cls, *warden).won);
    }
    // The level-1 thrall target with the starting spread (above) is unchanged by M11; a level-10
    // build kills a level-1 thrall in one or two hits.
    CHECK(duel(stats[1], 1, *thrall).secondsToKill <= 1.0);
}

// ---- Monster data loader ----

namespace {

const char* kMonster = R"(
[[monster]]
id = 1
name = "Kül Çömezi"
model = "ash_thrall"
level = 1
hp = 80
attack = 11
defense = 4
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.2
wander_speed = 1.3
aggro_radius = 6.0
leash_radius = 25.0
xp = 25
respawn_ms = 15000
)";

bool parse(const std::string& text, std::string& error, vg::MonsterData* out = nullptr)
{
    vg::MonsterData data;
    const bool ok = vg::parseMonsterData(text, "t.toml", data, error);
    if (out)
        *out = data;
    return ok;
}

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

} // namespace

TEST_CASE("Monsters: the shipped gamedata/monsters.toml is valid and has the M9 roster", "[game][monsters]")
{
    vg::MonsterData data;
    std::string error;
    const auto path = vg::locateMonstersFile();
    REQUIRE(path);
    const bool loaded = vg::loadMonsterData(*path, data, error);
    INFO(error);
    REQUIRE(loaded);
    std::set<std::string> models;
    for (const auto& m : data.templates())
    {
        models.insert(m.model);
        CHECK(m.moveSpeed < 5.5f); // a player can always run away
    }
    // M9's three Külden Doğanlar plus M13's two kinds per homeland (docs/WORLD_ZONES_ART.md). M15's new
    // kinds borrow these models until their own land (docs/DEVELOPMENT_PLAN.md §35.2).
    CHECK(models == std::set<std::string>{"ash_thrall", "slag_walker", "ember_warden", "stubble_thrall", "burnt_sentry",
                                          "rime_rock", "frozen_hearthguard"});
    // Zone 1: the village's oath ground first, then the two M13 gate arrival points outside it;
    // zones 2 and 3: the town and the gate arrival point each.
    REQUIRE(data.safeZones().size() == 7);
    CHECK(data.safeZones()[0].zone == 1);
    CHECK(data.safeZones()[0].radius == 40.0f);
    CHECK(data.inSafeZone(1, 256.0f, 256.0f));
    CHECK_FALSE(data.inSafeZone(4, 256.0f, 256.0f));
    // M15: every spawn is a slot member. Zone 1: Kül Çömezi 45-80 m out, Cüruf Yürüyen 90-140 m, the levels
    // 11-19 beyond 140 m, one Kor Bekçisi (boss, 1.25x).
    int thralls = 0, walkers = 0, wardens = 0;
    for (const auto& s : data.spawns())
    {
        CHECK(s.slot != 0);
        if (s.zone != 1)
            continue;
        const auto* t = data.find(s.monster);
        REQUIRE(t);
        const float d = std::hypot(s.x - 256.0f, s.z - 256.0f);
        if (t->name == "Kül Çömezi")
        {
            thralls += s.count;
            CHECK((d >= 45.0f && d <= 80.0f));
            CHECK((t->level >= 1 && t->level <= 2));
        }
        else if (t->name == "Cüruf Yürüyen")
        {
            walkers += s.count;
            CHECK((d >= 90.0f && d <= 140.0f));
            CHECK((t->level >= 3 && t->level <= 4));
        }
        else if (t->name == "Kor Bekçisi")
        {
            wardens += s.count;
            CHECK(t->elite);
            CHECK(t->level == 6);
            CHECK(t->scale == 1.25f);
        }
        else
        {
            CHECK(t->level >= 11);
            CHECK(d > 140.0f);
        }
        // The village's idle players are never in reach of an aggro radius.
        CHECK(d - s.radius - t->aggroRadius > data.safeZones()[0].radius);
    }
    CHECK(thralls >= 18);
    CHECK(walkers >= 12);
    CHECK(wardens == 1);
}

TEST_CASE("Monsters: the loader is strict", "[game][monsters]")
{
    std::string error;
    vg::MonsterData data;
    REQUIRE(parse(kMonster, error, &data));
    REQUIRE(data.find(1));
    CHECK(data.find(1)->name == "Kül Çömezi");
    CHECK(data.find(1)->scale == 1.0f);
    CHECK_FALSE(data.find(1)->elite);
    CHECK(data.find(2) == nullptr);
    CHECK(data.find(0) == nullptr);

    const auto fails = [&](const std::string& text, const std::string& expected) {
        INFO(text);
        std::string e;
        CHECK_FALSE(parse(text, e));
        CHECK_THAT(e, ContainsSubstring(expected));
        CHECK_THAT(e, ContainsSubstring("t.toml"));
    };
    fails("", "no [[monster]]");
    fails("[[monster]\n", "t.toml");
    fails(std::string(kMonster) + kMonster, "duplicate monster id 1");
    fails(replaced(kMonster, "xp = 25", "xp = 25\ncolour = 3"), "unknown key 'colour'");
    fails(replaced(kMonster, "name = \"Kül Çömezi\"", "name = \"\""), "must not be empty");
    fails(replaced(kMonster, "id = 1", "id = 0"), "out of range");
    fails(replaced(kMonster, "level = 1", "level = 21"), "out of range"); // M15: levels 1..20
    fails(replaced(kMonster, "hp = 80", "hp = 0"), "out of range");
    fails(replaced(kMonster, "hp = 80", "hp = \"80\""), "must be an integer");
    fails(replaced(kMonster, "move_speed = 4.2", "move_speed = -1.0"), "out of range");
    fails(replaced(kMonster, "move_speed = 4.2", "move_speed = nan"), "out of range");
    fails(replaced(kMonster, "leash_radius = 25.0", "leash_radius = 5.0"), "leash_radius must be larger");
    fails(replaced(kMonster, "respawn_ms = 15000", "respawn_ms = 1000"), "respawn_ms must be at least");
    fails(replaced(kMonster, "attack_range = 2.0", ""), "missing 'attack_range'");
    fails(std::string(kMonster) + "\n[[spawn]]\nzone = 1\nmonster = 7\nx = 1.0\nz = 1.0\nradius = 1.0\n",
          "unknown monster id 7");
    fails(std::string(kMonster) + "\n[[spawn]]\nzone = 0\nmonster = 1\nx = 1.0\nz = 1.0\nradius = 1.0\n",
          "out of range");
    fails(std::string(kMonster) + "\n[[spawn]]\nzone = 1\nmonster = 1\nx = 1.0\nz = 1.0\nradius = 1.0\ncount = 0\n",
          "out of range");
    fails(std::string(kMonster) + "\n[[spawn]]\nzone = 1\nmonster = 1\nx = 1.0\nz = 1.0\nradius = 1.0\nkind = 2\n",
          "unknown key 'kind'");
    fails(std::string(kMonster) + "\n[[safe_zone]]\nzone = 1\nx = 10.0\nz = 10.0\nradius = 0.5\n", "out of range");
    // A wander circle may not reach into a safe zone (monsters never enter one).
    fails(std::string(kMonster) +
              "\n[[safe_zone]]\nzone = 1\nx = 100.0\nz = 100.0\nradius = 20.0\n"
              "[[spawn]]\nzone = 1\nmonster = 1\nx = 123.0\nz = 100.0\nradius = 4.0\n",
          "reaches into the safe zone");
    // ... but the same circle in another zone is fine, and one just outside too.
    REQUIRE(parse(std::string(kMonster) +
                      "\n[[safe_zone]]\nzone = 1\nx = 100.0\nz = 100.0\nradius = 20.0\n"
                      "[[spawn]]\nzone = 2\nmonster = 1\nx = 123.0\nz = 100.0\nradius = 4.0\n"
                      "[[spawn]]\nzone = 1\nmonster = 1\nx = 124.5\nz = 100.0\nradius = 4.0\ncount = 3\n",
                  error, &data));
    REQUIRE(data.spawns().size() == 2);
    CHECK(data.spawns()[1].count == 3);
    CHECK(data.inSafeZone(1, 119.9f, 100.0f));
    CHECK_FALSE(data.inSafeZone(1, 120.1f, 100.0f));
    // A failed parse leaves the output unchanged.
    vg::MonsterData kept = data;
    std::string e;
    CHECK_FALSE(vg::parseMonsterData("[[monster]]\nid = 3\n", "t.toml", kept, e));
    CHECK(kept.spawns().size() == 2);
}
