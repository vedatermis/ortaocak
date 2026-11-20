// M15 "Avlaklar" (docs/PROTOCOL_LEVELS.md, docs/DEVELOPMENT_PLAN.md §35): hunting slots (data, parse rules,
// terrain rules), the shipped world read slot by slot, the bosses and their uniques, armour and the item
// pool (every item reachable), the forced unique drop hook.

#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/game/Npcs.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <random>
#include <set>
#include <string>

namespace vg = vo::game;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string kTemplates = R"(
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
move_speed = 4.0
wander_speed = 1.3
aggro_radius = 6.0
leash_radius = 25.0
xp = 25
respawn_ms = 15000

[[monster]]
id = 2
name = "Kül Çömezi"
model = "ash_thrall"
level = 2
hp = 105
attack = 14
defense = 5
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.3
aggro_radius = 6.0
leash_radius = 25.0
xp = 32
respawn_ms = 15000

[[monster]]
id = 3
name = "Cüruf Yürüyen"
model = "slag_walker"
level = 3
hp = 220
attack = 18
defense = 9
attack_range = 2.2
attack_interval_ms = 2200
move_speed = 2.6
wander_speed = 0.9
aggro_radius = 7.0
leash_radius = 25.0
xp = 60
respawn_ms = 30000

[[monster]]
id = 5
name = "Kor Bekçisi"
model = "ember_warden"
level = 6
hp = 900
attack = 38
defense = 14
attack_range = 2.6
attack_interval_ms = 2000
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 10.0
leash_radius = 22.0
xp = 300
respawn_ms = 90000
elite = true

[[safe_zone]]
zone = 1
x = 100.0
z = 100.0
radius = 20.0
)";

std::string slot(const std::string& key, float x, float z, const std::string& extra = "",
                 const std::string& monsters = "[1, 2]", int count = 6, float ring = 6.5f)
{
    return std::format("\n[[slot]]\nkey = \"{}\"\nname = \"Yuva {}\"\nzone = 1\nx = {}\nz = {}\nmonsters = {}\ncount = {}\n"
                       "ring = {}\n{}",
                       key, key, x, z, monsters, count, ring, extra);
}

std::string parseError(const std::string& text)
{
    vg::MonsterData d;
    std::string e;
    if (vg::parseMonsterData(kTemplates + text, "test.toml", d, e))
        return {};
    return e;
}

vg::MonsterData parsed(const std::string& text)
{
    vg::MonsterData d;
    std::string e;
    INFO(e);
    const bool ok = vg::parseMonsterData(kTemplates + text, "test.toml", d, e);
    INFO(e);
    REQUIRE(ok);
    return d;
}

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

} // namespace

// ---- Slots: data and parse rules ----

TEST_CASE("Slots: a ring of homes, members taking the templates in turn, the area", "[game][slots]")
{
    const auto d = parsed(slot("a", 200.0f, 200.0f, "respawn_ms = 12000\nleash = 18.0\n"));
    REQUIRE(d.slots().size() == 1);
    const vg::MonsterSlot& s = d.slots()[0];
    CHECK(s.key == "a");
    CHECK(s.name == "Yuva a");
    CHECK(s.homes.size() == 6);
    CHECK(s.minLevel == 1);
    CHECK(s.maxLevel == 2);
    CHECK(s.aggro == 6.0f);
    CHECK(s.wander == 1.0f);
    CHECK(s.radius == 7.5f); // ring 6.5 + wander 1
    CHECK_FALSE(s.elite);
    // Six on the ring (no centre below 7), the first at +z, alternating levels.
    CHECK(s.homes[0].x == 200.0f);
    CHECK(s.homes[0].z == 206.5f);
    for (std::size_t k = 0; k < s.homes.size(); ++k)
    {
        CHECK(std::abs(std::hypot(s.homes[k].x - 200.0f, s.homes[k].z - 200.0f) - 6.5f) < 1e-3f);
        CHECK(s.homes[k].monster == (k % 2 == 0 ? 1 : 2));
    }
    // Every member is a spawn: count 1, wander radius, the slot's overrides.
    REQUIRE(d.spawns().size() == 6);
    for (const auto& sp : d.spawns())
    {
        CHECK(sp.slot == 1);
        CHECK(sp.count == 1);
        CHECK(sp.radius == 1.0f);
        CHECK(sp.respawnMs == 12000);
        CHECK(sp.leash == 18.0f);
        CHECK(d.slotOf(sp) == &s);
    }
    // Seven or more: one in the centre.
    const auto d7 = parsed(slot("b", 200.0f, 200.0f, "", "[1, 2]", 7, 6.0f));
    CHECK(d7.slots()[0].homes[0].x == 200.0f);
    CHECK(d7.slots()[0].homes[0].z == 200.0f);
    CHECK(d7.slots()[0].homes.size() == 7);
    // Explicit homes.
    const auto dh = parsed("\n[[slot]]\nkey = \"h\"\nname = \"H\"\nzone = 1\nx = 300\nz = 300\nmonsters = [3]\ncount = 2\n"
                           "homes = [[-3, 0], [3, 0]]\nwander = 0.5\n");
    CHECK(dh.slots()[0].homes[1].x == 303.0f);
    CHECK(dh.slots()[0].radius == 3.5f);
}

TEST_CASE("Slots: slotAt, findSlot and the client's show margin", "[game][slots]")
{
    const auto d = parsed(slot("a", 200.0f, 200.0f) + slot("b", 240.0f, 200.0f));
    REQUIRE(d.findSlot("b"));
    CHECK(d.findSlot("c") == nullptr);
    CHECK(d.slotAt(1, 200.0f, 200.0f) == d.findSlot("a"));
    CHECK(d.slotAt(1, 207.0f, 200.0f) == d.findSlot("a"));
    CHECK(d.slotAt(1, 212.0f, 200.0f) == nullptr);
    CHECK(d.slotAt(1, 212.0f, 200.0f, vg::kSlotShowMargin) == d.findSlot("a"));
    CHECK(d.slotAt(1, 238.0f, 200.0f, vg::kSlotShowMargin) == d.findSlot("b"));
    CHECK(d.slotAt(2, 200.0f, 200.0f) == nullptr); // another zone
}

TEST_CASE("Slots: the loader is strict", "[game][slots]")
{
    CHECK(parseError(slot("a", 200.0f, 200.0f)).empty());
    // One kind per slot.
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[1, 3]")), ContainsSubstring("is another kind"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[1, 1]")), ContainsSubstring("named twice"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[9]")), ContainsSubstring("unknown monster id"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[]")), ContainsSubstring("1..4 monster ids"));
    // Neighbours: areas 7.5 + 7.5 + aggro 6 + 4 m = 25 m.
    CHECK(parseError(slot("a", 200.0f, 200.0f) + slot("b", 225.0f, 200.0f)).empty());
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f) + slot("b", 224.0f, 200.0f)),
               ContainsSubstring("slot 'b' is 24.0 m from slot 'a'"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f) + slot("a", 300.0f, 300.0f)), ContainsSubstring("duplicate slot key"));
    // Safe zones: area + aggro clear.
    CHECK_THAT(parseError(slot("a", 130.0f, 100.0f)), ContainsSubstring("reaches into the safe zone"));
    CHECK(parseError(slot("a", 134.0f, 100.0f)).empty());
    // Area, ring, homes.
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[1, 2]", 6, 8.5f)), ContainsSubstring("is larger than 9 m"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[1, 2]", 6, 2.0f)), ContainsSubstring("at least 2.5 m"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[1, 2]", 12, 3.0f)), ContainsSubstring("closer than 2.5 m"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[1, 2]", 13)), ContainsSubstring("out of range"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "homes = [[0, 0]]\n")), ContainsSubstring("'homes' must hold"));
    CHECK_THAT(parseError(slot("A", 200.0f, 200.0f)), ContainsSubstring("must be 1..32 of [a-z0-9_]"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "respawn_ms = 2000\n")), ContainsSubstring("at least the 3000 ms"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "leash = 5.0\n")), ContainsSubstring("larger than the aggro"));
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "colour = 1\n")), ContainsSubstring("unknown key 'colour'"));
    // An elite stands alone.
    CHECK_THAT(parseError(slot("a", 200.0f, 200.0f, "", "[5]")), ContainsSubstring("an elite stands alone"));
    const auto boss = parsed(slot("a", 200.0f, 200.0f, "wander = 4.0\n", "[5]", 1, 0.0f));
    CHECK(boss.slots()[0].elite);
    CHECK(boss.slots()[0].radius == 4.0f);
    // A loose spawn too close to a slot.
    CHECK_THAT(parseError("\n[[spawn]]\nzone = 1\nmonster = 1\nx = 210\nz = 200\nradius = 1\n" + slot("a", 200.0f, 200.0f)),
               ContainsSubstring("of the [[spawn]] at"));
}

TEST_CASE("Slots: the terrain rules - walkable, a line to the centre, reachable, clear of gates and markers",
          "[game][slots]")
{
    vo::assets::TerrainData t = vo::tests::makeTestTerrain();
    const auto d = parsed(slot("a", 200.0f, 200.0f));
    const vg::MonsterSlot& s = d.slots()[0];
    std::string why;
    auto reach = vg::reachableCells(t, 100.0f, 100.0f);
    CHECK(vg::checkSlotTerrain(t, s, {}, reach, why));
    // A keep-out: area 7.5 + aggro 6 + 14.5 = 28 m from the gate's centre.
    CHECK(vg::checkSlotTerrain(t, s, {{"gate 'g'", 228.0f, 200.0f, 14.5f}}, reach, why));
    CHECK_FALSE(vg::checkSlotTerrain(t, s, {{"gate 'g'", 227.0f, 200.0f, 14.5f}}, reach, why));
    CHECK_THAT(why, ContainsSubstring("gate 'g'"));
    // A blocked cell on a home's wander circle.
    {
        auto b = t;
        vo::tests::blockCell(b, 200.0f, 207.3f);
        CHECK_FALSE(vg::checkSlotTerrain(b, s, {}, {}, why));
        CHECK_THAT(why, ContainsSubstring("is not walkable"));
    }
    // A wall between a home and the centre.
    {
        auto b = t;
        vo::tests::blockCell(b, 200.0f, 203.0f);
        CHECK_FALSE(vg::checkSlotTerrain(b, s, {}, {}, why));
        CHECK_THAT(why, ContainsSubstring("no straight walkable line"));
    }
    // A ring of walls round the slot: walkable inside, but not reachable from the spawn.
    {
        auto b = t;
        for (int k = 0; k < 720; ++k)
        {
            const double a = k * 3.14159265358979 / 360.0;
            vo::tests::blockCell(b, 200.0f + 12.0f * static_cast<float>(std::sin(a)), 200.0f + 12.0f * static_cast<float>(std::cos(a)));
        }
        const auto r = vg::reachableCells(b, 100.0f, 100.0f);
        CHECK(vg::checkSlotTerrain(b, s, {}, {}, why));
        CHECK_FALSE(vg::checkSlotTerrain(b, s, {}, r, why));
        CHECK_THAT(why, ContainsSubstring("cannot be reached on foot"));
    }
}

// ---- The shipped world, slot by slot ----

TEST_CASE("Slots: every shipped monster stands in a slot of 6-8 of one kind, bosses alone", "[game][slots][content]")
{
    const auto& d = monsters();
    CHECK(d.slots().size() == 46);
    for (const auto& sp : d.spawns())
        CHECK(sp.slot != 0);
    std::map<std::string, int> slotsPerKind;
    for (const auto& s : d.slots())
    {
        INFO(s.key);
        const vg::MonsterTemplate* t = d.find(s.monsters.front());
        REQUIRE(t);
        if (s.elite)
        {
            CHECK(s.homes.size() == 1);
            CHECK(t->respawnMs >= 10u * 60u * 1000u); // tens of minutes
        }
        else
        {
            CHECK((s.homes.size() >= 6 && s.homes.size() <= 8));
            CHECK((2.0f * s.radius >= 12.0f && 2.0f * s.radius <= 16.5f)); // 12-16 m across
            CHECK((s.respawnMs >= 12000 && s.respawnMs <= 25000));         // quick respawn
            CHECK(s.leash == 20.0f);
            ++slotsPerKind[t->name];
        }
    }
    // At least three slots per kind (the two oldest kinds of each zone: three or four).
    CHECK(slotsPerKind.size() == 13);
    for (const auto& [kind, n] : slotsPerKind)
    {
        INFO(kind);
        CHECK(n >= 3);
    }
}

TEST_CASE("Slots: the new kinds and bosses, their keys, names, levels and zones", "[game][slots][content]")
{
    struct Kind
    {
        std::string key, name;
        std::uint16_t zone, lo, hi;
        bool boss;
    };
    const Kind kinds[] = {
        {"scorched_reaper", "Kavruk Tırpancı", 2, 5, 7, false},  {"threshing_brute", "Harman Devi", 2, 8, 10, false},
        {"glacier_bellowsman", "Buzul Körükçü", 3, 5, 7, false}, {"anvil_breaker", "Örs Kıran", 3, 8, 10, false},
        {"soot_stalker", "İs Avcısı", 1, 11, 13, false},          {"slag_bulwark", "Cüruf Zırhlısı", 1, 14, 16, false},
        {"ember_warlock", "Kor Büyücüsü", 1, 17, 19, false},      {"hearth_lord", "Ocak Efendisi", 1, 20, 20, true},
        {"stubble_lord", "Anız Ağası", 2, 10, 10, true},          {"bellows_khan", "Körük Hanı", 3, 10, 10, true},
        {"slag_matron", "Cüruf Anası", 1, 15, 15, true},          {"", "Kor Bekçisi", 1, 6, 6, true},
    };
    const auto& d = monsters();
    for (const Kind& k : kinds)
    {
        INFO(k.name);
        std::set<std::uint16_t> levels;
        int slots = 0;
        for (const auto& s : d.slots())
        {
            const vg::MonsterTemplate* t = d.find(s.monsters.front());
            if (t->name != k.name)
                continue;
            ++slots;
            CHECK(s.zone == k.zone);
            CHECK(s.elite == k.boss);
            for (const auto id : s.monsters)
                levels.insert(d.find(id)->level);
        }
        CHECK(slots == (k.boss ? 1 : 3));
        REQUIRE_FALSE(levels.empty());
        CHECK(*levels.begin() == k.lo);
        CHECK(*levels.rbegin() == k.hi);
    }
    // Bosses: far more hp and attack than the slot monsters of their level.
    for (const auto& b : d.templates())
    {
        if (!b.elite)
            continue;
        for (const auto& m : d.templates())
            if (!m.elite && m.level == b.level)
            {
                CHECK(b.hp >= 3 * m.hp);
                CHECK(b.attack > m.attack);
            }
    }
}

TEST_CASE("Slots: higher levels lie farther from the town of each zone", "[game][slots][content]")
{
    const std::map<std::uint16_t, std::pair<float, float>> towns = {{1, {256.0f, 256.0f}}, {2, {250.0f, 296.0f}},
                                                                     {3, {256.0f, 344.0f}}};
    const auto& d = monsters();
    for (const auto& [zone, town] : towns)
    {
        // Mean distance of each level band's slots (bands by the slot's lowest level).
        std::map<std::uint16_t, std::pair<double, int>> band;
        for (const auto& s : d.slots())
            if (s.zone == zone && !s.elite)
            {
                auto& b = band[s.minLevel];
                b.first += std::hypot(s.x - town.first, s.z - town.second);
                ++b.second;
            }
        double last = 0.0;
        for (const auto& [lvl, b] : band)
        {
            INFO("zone " << zone << " level " << lvl);
            const double mean = b.first / b.second;
            CHECK(mean > last);
            last = mean;
        }
    }
}

// ---- Items: armour, uniques, tints, the pool ----

TEST_CASE("Items: armour goes only into its own slot and adds defense and attributes", "[game][items][armor]")
{
    const auto& c = items();
    const vg::ItemTemplate* chest = c.find(86); // Sağlam Göğüs Zırhı: heavy, level 5
    REQUIRE(chest);
    CHECK(chest->kind == vg::ItemKind::Armor);
    CHECK(chest->slot == vg::SlotRule::Chest);
    CHECK(chest->model == "heavy_a_chest"); // M17: worn mesh armor/<look>/heavy_a_chest.vmesh
    CHECK(chest->icon == "armor_heavy_chest");
    CHECK(chest->isTinted());
    const vg::ItemUser warrior{1, 5};
    CHECK(vg::checkEquip(*chest, vg::EquipSlot::Chest, warrior) == vg::ItemMoveResult::Ok);
    CHECK(vg::checkEquip(*chest, vg::EquipSlot::Head, warrior) == vg::ItemMoveResult::WrongSlot);
    CHECK(vg::checkEquip(*chest, vg::EquipSlot::MainHand, warrior) == vg::ItemMoveResult::WrongSlot);
    CHECK(vg::checkEquip(*chest, vg::EquipSlot::Chest, vg::ItemUser{3, 5}) == vg::ItemMoveResult::WrongClass); // heavy
    CHECK(vg::checkEquip(*chest, vg::EquipSlot::Chest, vg::ItemUser{1, 4}) == vg::ItemMoveResult::LevelTooLow);
    // A weapon never goes into an armour slot.
    CHECK(vg::checkEquip(*c.find(1), vg::EquipSlot::Head, warrior) == vg::ItemMoveResult::WrongSlot);
    // Quick move from the bag puts armour into its slot; the bonus counts like a weapon's.
    vg::Inventory inv;
    std::string error;
    REQUIRE(vg::Inventory::fromItems({{1, vg::ItemPlace::bag(0), 86, 1}, {2, vg::ItemPlace::bag(1), 85, 1}}, inv, error));
    CHECK(vg::quickMoveTarget(c, inv, vg::ItemPlace::bag(0)) == vg::ItemPlace::equipment(vg::EquipSlot::Chest));
    CHECK(vg::quickMoveTarget(c, inv, vg::ItemPlace::bag(1)) == vg::ItemPlace::equipment(vg::EquipSlot::Head));
    const auto plan = vg::planItemMove(c, inv, warrior, vg::ItemPlace::bag(0), vg::ItemPlace::equipment(vg::EquipSlot::Chest));
    REQUIRE(plan.result == vg::ItemMoveResult::Ok);
    vg::applyItemMove(inv, plan);
    const vg::EquipBonus b = vg::equipmentBonus(c, inv);
    CHECK(b.defense == chest->defense);
    CHECK(b.attributes[static_cast<std::size_t>(vg::Attribute::Vitality)] == 1);
    // Parse rules.
    CHECK(vg::slotRuleName(vg::SlotRule::Feet) == "feet");
    CHECK(vg::slotRuleDisplayName(vg::SlotRule::Hands) == "Eller");
    CHECK(vg::itemKindDisplayName(vg::ItemKind::Armor) == "Zırh");
    CHECK(vg::rarityDisplayName(vg::Rarity::Unique) == "Eşsiz");
    CHECK(vg::rarityName(vg::Rarity::Unique) == "unique");
}

TEST_CASE("Items: the M15 pool - tiers, rarities, sources, every item reachable", "[game][items][content]")
{
    const auto& c = items();
    CHECK(c.size() == 303); // M16: + the three upgrade stones 166-168; M18: 169-303
    // Reachable: a starter kit, a merchant or a loot table names every item.
    std::set<vg::ItemTemplateId> sources;
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
        if (const auto* kit = c.kitFor(cls))
        {
            for (const auto id : kit->equip)
                sources.insert(id);
            for (const auto id : kit->bag)
                sources.insert(id);
        }
    for (const auto& n : vg::npcs().npcs())
        for (const auto id : n.stock)
            sources.insert(id);
    for (const auto& t : loot().tables())
        for (const auto& dr : t.drops)
            sources.insert(dr.item);
    for (const auto& t : c.templates())
    {
        INFO(t.id << " " << t.name);
        CHECK(sources.contains(t.id));
    }
    // Every slot of every class has more than one option in the level bands 1-4, 5-8, 9-12, 13-16, 17-20.
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
        for (const auto slot : {vg::SlotRule::Head, vg::SlotRule::Chest, vg::SlotRule::Legs, vg::SlotRule::Hands,
                                vg::SlotRule::Feet})
            for (std::uint16_t lo = 1; lo <= 17; lo += 4)
            {
                int n = 0;
                for (const auto& t : c.templates())
                    if (t.slot == slot && t.allowsClass(cls) && t.level >= lo && t.level < lo + 4)
                        ++n;
                INFO("class " << int(cls) << " slot " << vg::slotRuleName(slot) << " levels " << lo);
                CHECK(n >= 2);
            }
    // Several weapon types per class from level 5 on (different meshes: sword / axe / mace, dagger / pala,
    // ember staff / frost rod, mace / rod).
    for (std::uint8_t cls = 1; cls <= 4; ++cls)
    {
        std::set<std::string> types;
        for (const auto& t : c.templates())
            if (t.kind == vg::ItemKind::Weapon && t.allowsClass(cls) && t.level >= 5)
                types.insert(t.model);
        INFO("class " << int(cls));
        CHECK(types.size() >= 2);
    }

}

TEST_CASE("Items: every boss has its own uniques, unsellable, stronger than the gear of their level",
          "[game][items][bosses]")
{
    const auto& c = items();
    const auto& d = monsters();
    int bosses = 0;
    for (const auto& m : d.templates())
    {
        if (!m.elite)
            continue;
        ++bosses;
        INFO(m.name);
        const vg::LootTable* t = loot().forMonster(m.id);
        REQUIRE(t);
        std::set<std::uint8_t> classes;
        std::set<vg::SlotRule> slots;
        int uniques = 0;
        for (const auto& dr : t->drops)
        {
            const vg::ItemTemplate* it = c.find(dr.item);
            if (it->rarity != vg::Rarity::Unique)
                continue;
            ++uniques;
            CHECK(dr.unique);
            CHECK((dr.chance >= 0.03 && dr.chance <= 0.08)); // low, documented
            CHECK(it->value == 0);                           // cannot be sold
            for (std::uint8_t cls = 1; cls <= 4; ++cls)
                if (it->allowsClass(cls))
                    classes.insert(cls);
            slots.insert(it->slot);
            // Stronger than any common / uncommon / rare item of the same slot at or below its level.
            // M18: the bonus lines count too (10 hp / mp or 5 permille crit / dodge = one attribute point / 2).
            const auto power = [](const vg::ItemTemplate& x) {
                double p = x.attack + 2 * x.defense;
                for (const auto a : x.attributes)
                    p += 2 * a;
                p += (x.bonusHp + x.bonusMp) / 10.0 + (x.bonusCritPermille + x.bonusDodgePermille) / 5.0;
                return p;
            };
            for (const auto& o : c.templates())
                if (o.rarity != vg::Rarity::Unique && o.slot == it->slot && o.level <= it->level &&
                    (o.classMask & it->classMask) != 0)
                {
                    INFO(it->name << " vs " << o.name);
                    CHECK(power(*it) > power(o));
                }
        }
        CHECK(uniques >= 3);
        CHECK(classes.size() >= 3);
        CHECK(slots.size() >= 3);
    }
    CHECK(bosses == 5);
    // No unique item is dropped by anything but a boss, or sold.
    for (const auto& t : loot().tables())
        for (const auto& dr : t.drops)
            if (dr.unique)
                CHECK(monsters().find(t.monsters.front())->elite);
    for (const auto& n : vg::npcs().npcs())
        for (const auto id : n.stock)
            CHECK(c.find(id)->rarity != vg::Rarity::Unique);
}

TEST_CASE("Loot: a boss roll with the forced unique hook always holds one of its uniques", "[game][loot][bosses]")
{
    for (const auto& m : monsters().templates())
    {
        if (!m.elite)
            continue;
        INFO(m.name);
        const vg::LootTable& t = *loot().forMonster(m.id);
        std::mt19937 rng(77);
        std::map<vg::ItemTemplateId, int> seen;
        for (int i = 0; i < 400; ++i)
        {
            vg::LootRoll r = vg::rollLoot(t, rng);
            vg::forceUniqueDrop(t, r, rng);
            CHECK(r.items.size() <= vg::kMaxLootEntries);
            int uniques = 0;
            for (const auto& it : r.items)
                if (items().find(it.templateId)->rarity == vg::Rarity::Unique)
                {
                    ++uniques;
                    ++seen[it.templateId];
                }
            CHECK(uniques >= 1);
        }
        // Every unique of the table comes up.
        std::size_t lines = 0;
        for (const auto& dr : t.drops)
            lines += dr.unique ? 1 : 0;
        CHECK(seen.size() == lines);
    }
    // Without uniques in the table: nothing changes.
    vg::LootTable plain;
    plain.drops.push_back({9, 1.0, 1, 1});
    vg::LootRoll r;
    r.items.push_back({9, 1});
    std::mt19937 rng(1);
    vg::forceUniqueDrop(plain, r, rng);
    CHECK(r.items.size() == 1);
    // A full roll: the last item makes room.
    vg::LootTable full = *loot().forMonster(35);
    vg::LootRoll many;
    for (int i = 0; i < 8; ++i)
        many.items.push_back({static_cast<vg::ItemTemplateId>(70 + i), 1});
    vg::forceUniqueDrop(full, many, rng);
    CHECK(many.items.size() == 8);
    CHECK(items().find(many.items.back().templateId)->rarity == vg::Rarity::Unique);
}

TEST_CASE("Items: tint and the unique rarity parse strictly", "[game][items]")
{
    const std::string kits = R"(
[[starter_kit]]
class = "warrior"
equip = {}
bag = []
[[starter_kit]]
class = "rogue"
equip = {}
bag = []
[[starter_kit]]
class = "mage"
equip = {}
bag = []
[[starter_kit]]
class = "priest"
equip = {}
bag = []
)";
    const auto parse = [&](const std::string& item, std::string& error) {
        vg::ItemCatalog c;
        return vg::parseItemCatalog(item + kits, "items.toml", c, error);
    };
    const std::string armor = "[[item]]\nid = 1\nname = \"A\"\nkind = \"armor\"\nslot = \"legs\"\nclasses = [\"mage\"]\n"
                              "level = 3\nattack = 0\ndefense = 2\nicon = \"armor_legs\"\nrarity = \"unique\"\n";
    std::string error;
    CHECK(parse(armor, error));
    CHECK(parse(armor + "tint = [10, 20, 255]\n", error));
    CHECK_FALSE(parse(armor + "tint = [10, 20]\n", error));
    CHECK_THAT(error, ContainsSubstring("'tint' must be [r, g, b]"));
    CHECK_FALSE(parse(armor + "tint = [10, 20, 256]\n", error));
    // M17: armour may name the worn mesh (armor/<look>/<model>.vmesh); a grip stays refused.
    CHECK(parse(armor + "model = \"heavy_a_legs\"\n", error));
    CHECK_FALSE(parse(armor + "grip = \"sword\"\n", error));
    CHECK_THAT(error, ContainsSubstring("'grip' is not allowed for armour"));
    std::string wrong = armor;
    wrong.replace(wrong.find("slot = \"legs\""), 13, "slot = \"main_hand\"");
    CHECK_FALSE(parse(wrong, error));
    CHECK_THAT(error, ContainsSubstring("does not fit kind 'armor'"));
}
