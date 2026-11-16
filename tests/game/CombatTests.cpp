// Shared combat rules (shared/game/Combat.h, docs/PROTOCOL_COMBAT.md §1.1-1.2) and the monster data
// loader (shared/game/Monsters.h, §1.3): formulas with injected rolls, the level curve, experience
// reduction, stats with equipment, the skill table, the tuning target against the shipped
// gamedata files, and the strict loader.

#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Monsters.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <random>
#include <set>
#include <string>

namespace vg = vo::game;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("Combat: level curve 80 * level^1.6 rounded to 10, 0 at the cap", "[game][combat]")
{
    CHECK(vg::xpToNext(1) == 80);
    CHECK(vg::xpToNext(2) == 240);
    CHECK(vg::xpToNext(3) == 460);
    CHECK(vg::xpToNext(4) == 740);
    CHECK(vg::xpToNext(5) == 1050);
    CHECK(vg::xpToNext(9) == 2690);
    CHECK(vg::xpToNext(10) == 0);
    CHECK(vg::xpToNext(0) == 0);
    for (std::uint16_t l = 1; l < vg::kLevelCap; ++l)
    {
        CHECK(vg::xpToNext(l) % 10 == 0);
        CHECK(std::abs(static_cast<double>(vg::xpToNext(l)) - 80.0 * std::pow(l, 1.6)) <= 5.0);
    }
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
    const auto capped = vg::addExperience({9, 2000}, 100000);
    CHECK(capped.level == vg::kLevelCap);
    CHECK(capped.xp == 0);
    CHECK(vg::addExperience({10, 0}, 50).xp == 0);
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
    CHECK(vg::healAmount(*sun, 90) == 23);
    CHECK(vg::healAmount(*sun, 1) == 1);
    CHECK(vg::findSkill(0) == nullptr);
    CHECK(vg::findSkill(5) == nullptr);
    for (std::uint8_t c = 1; c <= 4; ++c)
    {
        REQUIRE(vg::classSkill(c));
        CHECK(vg::classSkill(c)->charClass == c);
    }
    CHECK(vg::classSkill(9) == nullptr);
    CHECK(vg::hpRegenPerSecond(100, false) == 2.0f);
    CHECK(vg::hpRegenPerSecond(100, true) == 4.0f);
    CHECK(vg::mpRegenPerSecond(100, false) == 3.0f);
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
    INFO(error);
    REQUIRE(vg::loadMonsterData(*path, data, error));
    std::set<std::string> models;
    for (const auto& m : data.templates())
    {
        models.insert(m.model);
        CHECK(m.moveSpeed < 5.5f); // a player can always run away
    }
    CHECK(models == std::set<std::string>{"ash_thrall", "slag_walker", "ember_warden"});
    REQUIRE(data.safeZones().size() == 1);
    CHECK(data.safeZones()[0].zone == 1);
    CHECK(data.inSafeZone(1, 256.0f, 256.0f));
    CHECK_FALSE(data.inSafeZone(2, 256.0f, 256.0f));
    // Several Kül Çömezi 40-70 m out, Cüruf Yürüyen 90-140 m, one Kor Bekçisi (elite, 1.25x).
    int thralls = 0, walkers = 0, wardens = 0;
    for (const auto& s : data.spawns())
    {
        const auto* t = data.find(s.monster);
        REQUIRE(t);
        const float d = std::hypot(s.x - 256.0f, s.z - 256.0f);
        if (t->model == "ash_thrall")
        {
            thralls += s.count;
            CHECK((d >= 40.0f && d <= 70.0f));
            CHECK((t->level >= 1 && t->level <= 2));
        }
        else if (t->model == "slag_walker")
        {
            walkers += s.count;
            CHECK((d >= 90.0f && d <= 140.0f));
            CHECK((t->level >= 3 && t->level <= 4));
        }
        else
        {
            wardens += s.count;
            CHECK(t->elite);
            CHECK(t->level == 6);
            CHECK(t->scale == 1.25f);
        }
        // The village's idle players are never in reach of an aggro radius.
        CHECK(d - s.radius - t->aggroRadius > data.safeZones()[0].radius);
    }
    CHECK(thralls >= 6);
    CHECK(walkers >= 2);
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
    fails(replaced(kMonster, "level = 1", "level = 11"), "out of range");
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
