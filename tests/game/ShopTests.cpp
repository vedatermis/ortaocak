// Merchants and consumables, shared code (docs/PROTOCOL_SHOP.md, v8): the shipped gamedata/npcs.toml
// and its strict loader, the startup spot check, consumables in items.toml, prices, the purchase /
// sale planning, the consumable checks, bar item slots, the loot lines and the v8 messages.

#include "client/world/SkillsView.h"
#include "shared/assets/Terrain.h"
#include "shared/game/Character.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Npcs.h"
#include "shared/game/Shop.h"
#include "shared/game/Skills.h"
#include "shared/net/Messages.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vg = vo::game;
namespace vn = vo::net;
using Catch::Matchers::ContainsSubstring;
using SR = vn::ShopResult;
using UR = vn::ItemUseResult;

namespace {

const vg::ItemCatalog& items()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        std::string error;
        const auto file = vg::locateItemsFile();
        REQUIRE(file);
        const bool ok = vg::loadItemCatalog(*file, out, error);
        INFO(error);
        REQUIRE(ok);
        return out;
    }();
    return c;
}

const vg::NpcCatalog& shippedNpcs()
{
    static const vg::NpcCatalog c = [] {
        vg::NpcCatalog out;
        std::string error;
        const auto file = vg::locateNpcsFile();
        REQUIRE(file);
        const bool ok = vg::loadNpcCatalog(*file, &items(), out, error);
        INFO(error);
        REQUIRE(ok);
        return out;
    }();
    return c;
}

// One merchant; `replace` swaps one exact substring for another.
std::string npcSample(std::vector<std::pair<std::string, std::string>> replace = {})
{
    std::string s = R"([[npc]]
id = 1
name = "Ad"
title = "Unvan"
greeting = "Merhaba"
model = "npc_test"
zone = 1
x = 100.0
z = 120.0
yaw = 0.5
kind = "merchant"
stock = [16, 1]
)";
    for (const auto& [from, to] : replace)
    {
        const auto at = s.find(from);
        REQUIRE(at != std::string::npos);
        s.replace(at, from.size(), to);
    }
    return s;
}

std::string npcError(const std::string& text, const vg::ItemCatalog* catalog = &items())
{
    vg::NpcCatalog c;
    std::string error;
    CHECK_FALSE(vg::parseNpcCatalog(text, "npcs.toml", catalog, c, error));
    CHECK(c.empty());
    return error;
}

vg::Inventory inventoryOf(std::vector<vg::StoredItem> rows)
{
    vg::Inventory inv;
    std::string error;
    INFO(error);
    REQUIRE(vg::Inventory::fromItems(rows, inv, error));
    return inv;
}

std::string itemsWith(const std::string& extra)
{
    // A minimal items.toml with every class's kit (the loader requires one per class).
    return R"([[item]]
id = 1
name = "S"
kind = "weapon"
slot = "main_hand"
classes = ["warrior", "rogue", "mage", "priest"]
level = 1
attack = 1
defense = 0
model = "m"
icon = "i"
grip = "sword"

[[starter_kit]]
class = "warrior"
equip = { main_hand = 1 }
bag = []
[[starter_kit]]
class = "rogue"
equip = { main_hand = 1 }
bag = []
[[starter_kit]]
class = "mage"
equip = { main_hand = 1 }
bag = []
[[starter_kit]]
class = "priest"
equip = { main_hand = 1 }
bag = []
)" + extra;
}

} // namespace

TEST_CASE("Shop data: the shipped npcs.toml has the six merchants of the contract", "[game][shop]")
{
    const vg::NpcCatalog& c = shippedNpcs();
    REQUIRE(c.size() == 6);
    const std::vector<std::pair<std::uint16_t, std::string>> models = {
        {1, "npc_potion_esik"},   {2, "npc_smith_esik"},   {3, "npc_potion_aydor"},
        {4, "npc_smith_aydor"},   {5, "npc_potion_kargan"}, {6, "npc_smith_kargan"}};
    for (const auto& [id, model] : models)
    {
        const vg::NpcDef* n = c.find(id);
        REQUIRE(n);
        INFO(n->name);
        CHECK(n->model == model);
        CHECK(n->kind == vg::NpcKind::Merchant);
        CHECK(!n->title.empty());
        CHECK(!n->greeting.empty());
        CHECK(n->zone == (id + 1) / 2); // 1-2 Ortaocak, 3-4 Tanyeri, 5-6 Demirağız
        const bool potion = model.starts_with("npc_potion");
        for (const auto tid : n->stock)
        {
            const vg::ItemTemplate* t = items().find(tid);
            REQUIRE(t);
            CHECK(t->value >= 1);
            CHECK(t->isConsumable() == potion);
        }
        if (potion)
            CHECK(n->stock == std::vector<vg::ItemTemplateId>{16, 17, 18, 19});
        else
        {
            // Every smith sells every class's starter weapon (a replacement) and something for the rogue.
            for (const auto starter : std::initializer_list<vg::ItemTemplateId>{1, 3, 4, 5, 12})
                CHECK(std::find(n->stock.begin(), n->stock.end(), starter) != n->stock.end());
        }
    }
    CHECK(c.find(7) == nullptr);
    CHECK(c.find(0) == nullptr);
    // The upgrades of the contract's §2.5 table are on sale somewhere.
    std::set<vg::ItemTemplateId> sold;
    for (const auto& n : c.npcs())
        sold.insert(n.stock.begin(), n.stock.end());
    for (const auto upgrade : std::initializer_list<vg::ItemTemplateId>{6, 7, 8, 12, 14})
        CHECK(sold.contains(upgrade));
    CHECK_FALSE(sold.contains(15)); // Bekçi Kalkanı stays an elite drop
    CHECK_FALSE(sold.contains(13));
}

TEST_CASE("Shop data: the npcs.toml loader refuses broken files with file and line", "[game][shop]")
{
    {
        vg::NpcCatalog c;
        std::string error;
        INFO(error);
        REQUIRE(vg::parseNpcCatalog(npcSample(), "npcs.toml", &items(), c, error));
        REQUIRE(c.size() == 1);
        CHECK(c.find(1)->stock == std::vector<vg::ItemTemplateId>{16, 1});
        CHECK(std::abs(c.find(1)->yaw - 0.5f) < 1e-6f);
        // Without an item catalog the stock is not checked.
        REQUIRE(vg::parseNpcCatalog(npcSample({{"[16, 1]", "[9999]"}}), "npcs.toml", nullptr, c, error));
    }
    CHECK_THAT(npcError(""), ContainsSubstring("no [[npc]] entries"));
    CHECK_THAT(npcError("x = 1\n"), ContainsSubstring("unknown key 'x'"));
    CHECK_THAT(npcError(npcSample({{"kind = ", "colour = \"red\"\nkind = "}})), ContainsSubstring("npcs.toml:11: [[npc]]: unknown key 'colour'"));
    CHECK_THAT(npcError(npcSample({{"id = 1", "id = 0"}})), ContainsSubstring("'id' = 0 is out of range"));
    CHECK_THAT(npcError(npcSample({{"id = 1", "id = 1000"}})), ContainsSubstring("out of range (1..999)"));
    CHECK_THAT(npcError(npcSample({{"name = \"Ad\"\n", ""}})), ContainsSubstring("missing 'name'"));
    CHECK_THAT(npcError(npcSample({{"title = \"Unvan\"", "title = \"\""}})), ContainsSubstring("'title' must not be empty"));
    CHECK_THAT(npcError(npcSample({{"name = \"Ad\"", "name = \"" + std::string(49, 'a') + "\""}})),
               ContainsSubstring("longer than 48 bytes"));
    CHECK_THAT(npcError(npcSample({{"npc_test", "Npc-Test"}})), ContainsSubstring("must be [a-z0-9_]"));
    CHECK_THAT(npcError(npcSample({{"zone = 1", "zone = 0"}})), ContainsSubstring("'zone' = 0 is out of range"));
    CHECK_THAT(npcError(npcSample({{"x = 100.0", "x = -1.0"}})), ContainsSubstring("'x' = -1 is out of range"));
    CHECK_THAT(npcError(npcSample({{"z = 120.0", "z = \"far\""}})), ContainsSubstring("'z' must be a number"));
    CHECK_THAT(npcError(npcSample({{"yaw = 0.5", "yaw = 7.0"}})), ContainsSubstring("'yaw' = 7 is out of range"));
    CHECK_THAT(npcError(npcSample({{"\"merchant\"", "\"guard\""}})), ContainsSubstring("unknown kind 'guard'"));
    CHECK_THAT(npcError(npcSample({{"stock = [16, 1]\n", ""}})), ContainsSubstring("a merchant needs a 'stock'"));
    CHECK_THAT(npcError(npcSample({{"[16, 1]", "[]"}})), ContainsSubstring("non-empty array"));
    CHECK_THAT(npcError(npcSample({{"[16, 1]", "[16, 16]"}})), ContainsSubstring("item 16 is in the stock twice"));
    CHECK_THAT(npcError(npcSample({{"[16, 1]", "[9999]"}})), ContainsSubstring("stock item 9999 is not in items.toml"));
    CHECK_THAT(npcError(npcSample({{"[16, 1]", "[\"16\"]"}})), ContainsSubstring("stock entries must be item ids"));
    std::string many = "[";
    for (int i = 0; i < 25; ++i)
        many += std::to_string(i + 1) + (i < 24 ? ", " : "]");
    CHECK_THAT(npcError(npcSample({{"[16, 1]", many}}), nullptr), ContainsSubstring("more than 24 stock entries"));
    CHECK_THAT(npcError(npcSample() + npcSample({{"npc_test", "npc_other"}})), ContainsSubstring("id 1 used twice"));
    CHECK_THAT(npcError(npcSample() + npcSample({{"id = 1", "id = 2"}})), ContainsSubstring("model 'npc_test' used twice"));
    CHECK_THAT(npcError("[[npc]\nid = 1\n"), ContainsSubstring("npcs.toml:1:"));
    // An item without a value has no price.
    vg::ItemCatalog cheap;
    std::string error;
    INFO(error);
    REQUIRE(vg::parseItemCatalog(itemsWith(""), "items.toml", cheap, error));
    CHECK_THAT(npcError(npcSample({{"[16, 1]", "[1]"}}), &cheap), ContainsSubstring("has no value, so no price"));
}

TEST_CASE("Shop data: an NPC spot must be walkable, off the road and clear of gates and markers", "[game][shop]")
{
    vo::assets::TerrainData t = vo::tests::makeTestTerrain();
    std::string why;
    const std::vector<vg::KeepOut> keep = {{"gate 'g'", 200.0f, 200.0f, 4.5f}, {"marker 'spawn_aydor'", 100.0f, 100.0f, 4.0f}};
    CHECK(vg::checkNpcSpot(t, 150.0f, 150.0f, keep, why));
    CHECK_FALSE(vg::checkNpcSpot(t, -1.0f, 150.0f, keep, why));
    CHECK_THAT(why, ContainsSubstring("outside the zone"));
    CHECK_FALSE(vg::checkNpcSpot(t, 202.0f, 203.0f, keep, why));
    CHECK_THAT(why, ContainsSubstring("from gate 'g'"));
    CHECK_FALSE(vg::checkNpcSpot(t, 103.0f, 101.0f, keep, why));
    CHECK_THAT(why, ContainsSubstring("marker 'spawn_aydor'"));
    CHECK(vg::checkNpcSpot(t, 104.5f, 100.0f, keep, why));
    vo::tests::blockCell(t, 150.0f, 150.0f);
    CHECK_FALSE(vg::checkNpcSpot(t, 150.0f, 150.0f, keep, why));
    CHECK_THAT(why, ContainsSubstring("is not walkable"));
    // A blocked cell within 1 m: not enough room round it.
    CHECK_FALSE(vg::checkNpcSpot(t, 150.5f, 151.4f, keep, why));
    CHECK_THAT(why, ContainsSubstring("not all walkable"));
    // The road layer: splat G above 50 % of the four weights.
    t.splatResolution = 2;
    t.splat = {255, 0, 0, 0, /**/ 100, 155, 0, 0, /**/ 0, 0, 0, 0, /**/ 0, 0, 0, 0};
    CHECK(vg::roadWeightAt(t, 100.0f, 100.0f) == 0.0f);
    CHECK(std::abs(vg::roadWeightAt(t, 300.0f, 100.0f) - 155.0f / 255.0f) < 1e-4f);
    CHECK_FALSE(vg::checkNpcSpot(t, 300.0f, 100.0f, {}, why));
    CHECK_THAT(why, ContainsSubstring("is on a road"));
    CHECK(vg::checkNpcSpot(t, 300.0f, 300.0f, {}, why)); // no weights at all: not a road
}

TEST_CASE("Shop data: consumables in items.toml", "[game][shop]")
{
    // The shipped potions (contract §2.4).
    const std::vector<std::tuple<vg::ItemTemplateId, std::string, std::uint32_t, std::uint32_t, std::uint32_t>> potions = {
        {16, "potion_hp_small", 70, 0, 8},
        {17, "potion_hp_medium", 180, 0, 25},
        {18, "potion_mp_small", 0, 40, 8},
        {19, "potion_mp_medium", 0, 100, 25}};
    for (const auto& [id, icon, hp, mp, value] : potions)
    {
        const vg::ItemTemplate* t = items().find(id);
        REQUIRE(t);
        CHECK(t->kind == vg::ItemKind::Consumable);
        CHECK(t->isConsumable());
        CHECK(t->icon == icon);
        CHECK(t->restoreHp == hp);
        CHECK(t->restoreMp == mp);
        CHECK(t->value == value);
        CHECK(t->maxStack == 20);
        CHECK(t->slot == vg::SlotRule::None);
        CHECK(t->classMask == 0);
        CHECK(vg::itemKindName(t->kind) == "consumable");
        CHECK(vg::itemKindDisplayName(t->kind) == "Tüketilebilir");
        CHECK_FALSE(vg::quickMoveTarget(items(), inventoryOf({{1, vg::ItemPlace::bag(0), id, 3}}), vg::ItemPlace::bag(0)));
    }
    // Every shipped item has a value (a price) and every consumable fits a bar slot.
    for (const auto& t : items().templates())
    {
        INFO(t.id << " " << t.name);
        CHECK(t.value >= 1);
        if (t.isConsumable())
            CHECK(t.id <= vg::kMaxConsumableTemplateId);
    }
    // Strict keys.
    const auto parse = [](const std::string& item, std::string& error) {
        vg::ItemCatalog c;
        return vg::parseItemCatalog(itemsWith(item), "items.toml", c, error);
    };
    std::string error;
    const std::string potion = "\n[[item]]\nid = 30\nname = \"P\"\nkind = \"consumable\"\nicon = \"p\"\nmax_stack = 20\n"
                               "value = 3\nrestore_hp = 10\n";
    CHECK(parse(potion, error));
    CHECK_FALSE(parse(potion + "slot = \"main_hand\"\n", error));
    CHECK_THAT(error, ContainsSubstring("'slot' is not allowed for a consumable"));
    CHECK_FALSE(parse(potion + "level = 2\n", error));
    CHECK_THAT(error, ContainsSubstring("'level' is not allowed for a consumable"));
    std::string none = potion;
    none.replace(none.find("restore_hp = 10"), 15, "restore_mp = 0");
    CHECK_FALSE(parse(none, error));
    CHECK_THAT(error, ContainsSubstring("must restore something"));
    CHECK_FALSE(parse(potion + "restore_mp = 100001\n", error));
    CHECK_THAT(error, ContainsSubstring("'restore_mp' = 100001 is out of range"));
    std::string big = potion;
    big.replace(big.find("id = 30"), 7, "id = 40000");
    CHECK_FALSE(parse(big, error));
    CHECK_THAT(error, ContainsSubstring("a consumable's id must be at most 32767"));
    CHECK_FALSE(parse("\n[[item]]\nid = 31\nname = \"M\"\nkind = \"material\"\nicon = \"m\"\nrestore_hp = 5\n", error));
    CHECK_THAT(error, ContainsSubstring("'restore_hp' is only allowed for a consumable"));
}

TEST_CASE("Shop rules: prices, buying against the bag and the balance", "[game][shop]")
{
    const vg::ItemTemplate& hp = *items().find(16);
    const vg::ItemTemplate& staff = *items().find(8);
    CHECK(vg::buyPrice(hp) == 8);
    CHECK(vg::sellPrice(hp) == 4);
    CHECK(vg::buyPrice(staff) == 60);
    CHECK(vg::sellPrice(staff) == 30);
    CHECK(vg::sellPrice(*items().find(9)) == 1); // Kül Tozu, value 2
    for (const auto& t : items().templates())
        CHECK(vg::sellPrice(t) < vg::buyPrice(t)); // never more back than it cost

    // A stack of 3 potions in slot 2, a sword in slot 0.
    const vg::Inventory inv = inventoryOf({{1, vg::ItemPlace::bag(0), 1, 1}, {2, vg::ItemPlace::bag(2), 16, 3}});
    auto p = vg::planShopBuy(items(), inv, 100, 16, 5, 8);
    REQUIRE(p.result == SR::Ok);
    CHECK(p.cost == 40);
    REQUIRE(p.add.grown.size() == 1); // stacks first
    CHECK(p.add.grown[0] == vg::ItemQuantityChange{2, vg::ItemPlace::bag(2), 3, 8});
    CHECK(p.add.created.empty());
    p = vg::planShopBuy(items(), inv, 200, 16, 20, 8); // 17 onto the stack, 3 into slot 1
    REQUIRE(p.result == SR::Ok);
    CHECK(p.cost == 160);
    CHECK(p.add.grown[0].after == 20);
    REQUIRE(p.add.created.size() == 1);
    CHECK(p.add.created[0] == vg::NewItem{vg::ItemPlace::bag(1), 16, 3});
    p = vg::planShopBuy(items(), inv, 100, 4, 1, 12); // gear: a new slot
    REQUIRE(p.result == SR::Ok);
    CHECK(p.add.created[0] == vg::NewItem{vg::ItemPlace::bag(1), 4, 1});
    CHECK(vg::planShopBuy(items(), inv, 100, 16, 0, 8).result == SR::BadQuantity);
    CHECK(vg::planShopBuy(items(), inv, 100, 16, 21, 8).result == SR::BadQuantity);
    CHECK(vg::planShopBuy(items(), inv, 100, 4, 2, 12).result == SR::BadQuantity);
    CHECK(vg::planShopBuy(items(), inv, 100, 9999, 1, 12).result == SR::BadQuantity);
    CHECK(vg::planShopBuy(items(), inv, 39, 16, 5, 8).result == SR::NotEnoughCoins);
    CHECK(vg::planShopBuy(items(), inv, 40, 16, 5, 8).result == SR::Ok);
    CHECK(vg::planShopBuy(items(), inv, 0xFFFFFFFFu, 16, 20, 0xFFFFFFFFu).result == SR::NotEnoughCoins); // no overflow
    // A full bag: no slot for gear, only the existing stack's room for potions.
    std::vector<vg::StoredItem> full;
    for (std::uint8_t s = 0; s < vg::kBagSlots; ++s)
        full.push_back({static_cast<vg::ItemInstanceId>(s + 1), vg::ItemPlace::bag(s), s == 5 ? vg::ItemTemplateId{16} : vg::ItemTemplateId{1},
                        static_cast<std::uint16_t>(s == 5 ? 18 : 1)});
    const vg::Inventory fullInv = inventoryOf(full);
    CHECK(vg::planShopBuy(items(), fullInv, 1000, 4, 1, 12).result == SR::BagFull);
    CHECK(vg::planShopBuy(items(), fullInv, 1000, 16, 2, 8).result == SR::Ok);
    CHECK(vg::planShopBuy(items(), fullInv, 1000, 16, 3, 8).result == SR::BagFull); // nothing bought partly
}

TEST_CASE("Shop rules: selling from the bag", "[game][shop]")
{
    // Slot 0: the frost staff, slot 3: 5 potions, slot 4: a hidden item (template 9999).
    const vg::Inventory inv = inventoryOf({{1, vg::ItemPlace::bag(0), 8, 1},
                                           {2, vg::ItemPlace::bag(3), 16, 5},
                                           {3, vg::ItemPlace::bag(4), 9999, 1},
                                           {4, vg::ItemPlace::equipment(vg::EquipSlot::MainHand), 4, 1}});
    auto s = vg::planShopSell(items(), inv, 10, 0, 1);
    REQUIRE(s.result == SR::Ok);
    CHECK(s.templateId == 8);
    CHECK(s.gained == 30);
    CHECK(s.change == vg::ItemQuantityChange{1, vg::ItemPlace::bag(0), 1, 0});
    s = vg::planShopSell(items(), inv, 10, 3, 2);
    REQUIRE(s.result == SR::Ok);
    CHECK(s.gained == 8);
    CHECK(s.change == vg::ItemQuantityChange{2, vg::ItemPlace::bag(3), 5, 3});
    CHECK(vg::planShopSell(items(), inv, 10, 3, 5).change.after == 0);
    CHECK(vg::planShopSell(items(), inv, 10, 3, 0).result == SR::BadQuantity);
    CHECK(vg::planShopSell(items(), inv, 10, 3, 6).result == SR::BadQuantity);
    CHECK(vg::planShopSell(items(), inv, 10, 3, 6).templateId == 16);
    CHECK(vg::planShopSell(items(), inv, 10, 1, 1).result == SR::NoItem);  // empty
    CHECK(vg::planShopSell(items(), inv, 10, 4, 1).result == SR::NoItem);  // hidden
    CHECK(vg::planShopSell(items(), inv, 10, 28, 1).result == SR::NoItem); // out of range
    CHECK(vg::planShopSell(items(), inv, vn::kMaxCoins - 29, 0, 1).result == SR::PurseFull);
    CHECK(vg::planShopSell(items(), inv, vn::kMaxCoins - 30, 0, 1).result == SR::Ok);
    // An item worth 1 sells for 0: CannotSell.
    vg::ItemCatalog cheap;
    std::string error;
    INFO(error);
    REQUIRE(vg::parseItemCatalog(itemsWith("\n[[item]]\nid = 2\nname = \"C\"\nkind = \"material\"\nicon = \"c\"\nvalue = 1\n"),
                                 "items.toml", cheap, error));
    CHECK(vg::planShopSell(cheap, inventoryOf({{1, vg::ItemPlace::bag(0), 2, 1}}), 0, 0, 1).result == SR::CannotSell);
}

TEST_CASE("Shop rules: consumable checks", "[game][shop]")
{
    const vg::Inventory inv = inventoryOf({{1, vg::ItemPlace::bag(2), 16, 3}, {2, vg::ItemPlace::bag(5), 16, 1},
                                           {3, vg::ItemPlace::bag(6), 9, 4}});
    CHECK(vg::findUseStack(inv, 16, vn::kItemUseAnyStack) == std::optional<std::uint8_t>(2)); // the lowest slot
    CHECK(vg::findUseStack(inv, 16, 5) == std::optional<std::uint8_t>(5));
    CHECK_FALSE(vg::findUseStack(inv, 16, 6));  // another template there
    CHECK_FALSE(vg::findUseStack(inv, 16, 30)); // out of range
    CHECK_FALSE(vg::findUseStack(inv, 18, vn::kItemUseAnyStack));
    CHECK_FALSE(vg::findUseStack(inv, 0, vn::kItemUseAnyStack));
    CHECK(vg::checkItemUseStack(items(), inv, 16, 2) == UR::Ok);
    CHECK(vg::checkItemUseStack(items(), inv, 9, 6) == UR::NotUsable); // a material
    CHECK(vg::checkItemUseStack(items(), inv, 9999, 6) == UR::NotUsable);
    CHECK(vg::checkItemUseStack(items(), inv, 17, vn::kItemUseAnyStack) == UR::NoItem);

    vg::ItemUseState s;
    s.hp = 50;
    s.maxHp = 100;
    s.mp = 40;
    s.maxMp = 40;
    CHECK(vg::checkItemUse(70, 0, s) == UR::Ok);
    CHECK(vg::checkItemUse(0, 40, s) == UR::AlreadyFull); // mana full
    CHECK(vg::checkItemUse(10, 40, s) == UR::Ok);         // one of them helps
    s.onCooldown = true;
    CHECK(vg::checkItemUse(0, 40, s) == UR::OnCooldown); // the cooldown before "full"
    s.stunned = true;
    CHECK(vg::checkItemUse(70, 0, s) == UR::Stunned);
    s.dead = true;
    CHECK(vg::checkItemUse(70, 0, s) == UR::YouAreDead);
    s = {};
    s.hp = s.maxHp = 90;
    CHECK(vg::checkItemUse(70, 0, s) == UR::AlreadyFull);
}

TEST_CASE("Shop rules: consumables on the skill bar", "[game][shop]")
{
    const vg::SkillCatalog& sk = vg::skills();
    REQUIRE_FALSE(sk.empty());
    const vg::SkillRanks r = vg::defaultSkillRanks(sk, 3); // a mage with Köz Oku
    CHECK(vg::barItemSlot(16) == 0x8010);
    CHECK(vg::isBarItem(0x8010));
    CHECK_FALSE(vg::isBarItem(0x8000));
    CHECK_FALSE(vg::isBarItem(16));
    CHECK(vg::barItemTemplate(0x8010) == 16);
    CHECK(vg::barItemTemplate(3) == 0);
    const vg::SkillBar bar{3, 0x8010, 0x8012, 0x8010, 0, 0, 0, 0}; // duplicates allowed
    CHECK(vg::validSkillBar(sk, 3, r, bar, &items()));
    CHECK(vg::validSkillBar(sk, 3, r, bar)); // without the item file: any item slot 1..0x7FFF
    CHECK_FALSE(vg::validSkillBar(sk, 3, r, {3, 0x8009, 0, 0, 0, 0, 0, 0}, &items())); // a material
    CHECK(vg::validSkillBar(sk, 3, r, {3, 0x8009, 0, 0, 0, 0, 0, 0}));
    CHECK_FALSE(vg::validSkillBar(sk, 3, r, {3, 0x8000 | 999, 0, 0, 0, 0, 0, 0}, &items()));
    CHECK_FALSE(vg::validSkillBar(sk, 3, r, {3, 0x8000, 0, 0, 0, 0, 0, 0}));
    CHECK(vg::sanitizeSkillBar(sk, 3, r, {3, 0x8009, 0x8010, 0x8000, 18, 0, 0, 0}, &items()) ==
          vg::SkillBar{3, 0, 0x8010, 0, 0, 0, 0, 0});
    CHECK(vg::sanitizeSkillBar(sk, 3, r, {3, 0x8009, 0x8010, 0x8000, 18, 0, 0, 0}) ==
          vg::SkillBar{3, 0x8009, 0x8010, 0, 0, 0, 0, 0});
}

TEST_CASE("Shop data: every loot table ends with sparse potion lines", "[game][shop]")
{
    vg::LootTables t;
    std::string error;
    const auto file = vg::locateLootFile();
    REQUIRE(file);
    INFO(error);
    REQUIRE(vg::loadLootTables(*file, items(), nullptr, t, error));
    for (const auto& table : t.tables())
    {
        bool potion = false, afterPotion = false;
        for (const auto& d : table.drops)
        {
            const bool isPotion = items().find(d.item)->isConsumable();
            if (isPotion)
            {
                potion = true;
                CHECK(d.chance <= 0.5);
            }
            else if (potion)
                afterPotion = true;
        }
        CHECK(potion);
        CHECK_FALSE(afterPotion);
    }
}

TEST_CASE("Shop messages: v8 layouts, sizes and rejected values", "[net][shop]")
{
    CHECK(vn::kProtocolVersion == 8);
    CHECK(vn::kEntityTypeNpc == 4);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::ShopOpenReq) == 0x0900);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::ItemUsed) == 0x090B);
    for (std::uint16_t op = 0x0900; op <= 0x090B; ++op)
        CHECK(vn::isKnownOpcode(op));
    CHECK_FALSE(vn::isKnownOpcode(0x090C));
    CHECK(vn::opcodeName(vn::Opcode::ItemCooldown) == "ITEM_COOLDOWN");
    const auto roundTrip = [](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        const auto bytes = vn::encodeMessage(msg);
        REQUIRE(bytes);
        T back;
        REQUIRE(vn::decodeMessage(*bytes, back));
        CHECK(back == msg);
        return bytes->size();
    };
    CHECK(roundTrip(vn::ShopOpenReq{77}) == 4);
    CHECK(roundTrip(vn::ShopOpenRes{77, SR::Ok, {{16, 8}, {17, 25}}}) == 6 + 12);
    CHECK(roundTrip(vn::ShopOpenRes{77, SR::OutOfRange, {}}) == 6);
    CHECK(roundTrip(vn::ShopCloseReq{77}) == 4);
    CHECK(roundTrip(vn::ShopClosed{77, vn::ShopClosedReason::YouDied}) == 5);
    CHECK(roundTrip(vn::ShopBuyReq{1, 77, 16, 5}) == 12);
    CHECK(roundTrip(vn::ShopBuyRes{1, SR::Ok, 16, 5, 40}) == 13);
    CHECK(roundTrip(vn::ShopSellReq{2, 77, 3, 1}) == 11);
    CHECK(roundTrip(vn::ShopSellRes{2, SR::PurseFull, 8, 1, 0}) == 13);
    CHECK(roundTrip(vn::ItemUseReq{3, 16, vn::kItemUseAnyStack}) == 7);
    CHECK(roundTrip(vn::ItemUseRes{3, UR::OnCooldown, 16}) == 7);
    CHECK(roundTrip(vn::ItemCooldown{9000, 10000}) == 8);
    CHECK(roundTrip(vn::ItemUsed{5, 16, 70, 0}) == 14);
    // Writers refuse what readers refuse.
    CHECK_FALSE(vn::encodeMessage(vn::ShopOpenRes{1, SR::NotFound, {{16, 8}}}));
    CHECK_FALSE(vn::encodeMessage(vn::ShopOpenRes{1, SR::Ok, {{0, 8}}}));
    CHECK_FALSE(vn::encodeMessage(vn::ShopOpenRes{1, SR::Ok, {{16, 0}}}));
    CHECK_FALSE(vn::encodeMessage(vn::ShopOpenRes{1, SR::Ok, std::vector<vn::ShopEntry>(25, {16, 8})}));
    CHECK_FALSE(vn::encodeMessage(vn::ItemCooldown{0, 10000}));
    CHECK_FALSE(vn::encodeMessage(vn::ItemCooldown{10001, 10000}));
    const auto rejects = [](auto msg, std::size_t at, std::uint8_t value) {
        using T = decltype(msg);
        auto bytes = *vn::encodeMessage(msg);
        REQUIRE(at < bytes.size());
        bytes[at] = value;
        T back;
        return !vn::decodeMessage(bytes, back);
    };
    CHECK(rejects(vn::ShopOpenRes{1, SR::Ok, {}}, 4, 13));            // unknown result
    CHECK(rejects(vn::ShopOpenRes{1, SR::NotFound, {}}, 5, 1));       // entries on a failure (and none present)
    CHECK(rejects(vn::ShopClosed{1, vn::ShopClosedReason::OutOfRange}, 4, 0));
    CHECK(rejects(vn::ShopClosed{1, vn::ShopClosedReason::OutOfRange}, 4, 3));
    CHECK(rejects(vn::ShopBuyRes{1, SR::Ok, 16, 1, 8}, 4, 200));
    CHECK(rejects(vn::ShopSellRes{1, SR::Ok, 16, 1, 4}, 4, 13));
    CHECK(rejects(vn::ItemUseRes{1, UR::Ok, 16}, 4, 8));
    {
        auto bytes = *vn::encodeMessage(vn::ShopOpenRes{1, SR::Ok, {{16, 8}}});
        bytes[6] = 0; // template 0
        bytes[7] = 0;
        vn::ShopOpenRes back;
        CHECK_FALSE(vn::decodeMessage(bytes, back));
        bytes = *vn::encodeMessage(vn::ShopOpenRes{1, SR::Ok, {{16, 8}}});
        bytes.push_back(0); // trailing byte
        CHECK_FALSE(vn::decodeMessage(bytes, back));
        bytes = *vn::encodeMessage(vn::ShopOpenRes{1, SR::Ok, {{16, 8}}});
        bytes[5] = 25; // count above 24
        CHECK_FALSE(vn::decodeMessage(bytes, back));
    }
    {
        auto bytes = *vn::encodeMessage(vn::ItemCooldown{5, 10});
        bytes[0] = 11; // remaining above the total
        vn::ItemCooldown back;
        CHECK_FALSE(vn::decodeMessage(bytes, back));
    }
    CHECK(vn::shopResultName(SR::CannotSell) == "CannotSell");
    CHECK(vn::itemUseResultName(UR::AlreadyFull) == "AlreadyFull");
}

TEST_CASE("Shop client minimal: the refused ITEM_USE_RES lines", "[client][shop]")
{
    namespace cw = vo::client::world;
    const vg::ItemTemplate* hp = items().find(16);
    const vg::ItemTemplate* mp = items().find(18);
    CHECK(cw::itemUseResultText(UR::Ok, hp).empty());
    CHECK(cw::itemUseResultText(UR::NoItem, hp) == "Elinde Kızılca Şerbet kalmadı.");
    CHECK(cw::itemUseResultText(UR::NoItem, nullptr) == "Elinde bu eşya kalmadı.");
    CHECK(cw::itemUseResultText(UR::NotUsable, nullptr) == "Bu kullanılamaz.");
    CHECK(cw::itemUseResultText(UR::YouAreDead, hp) == "Ölüyken kullanamazsın.");
    CHECK(cw::itemUseResultText(UR::OnCooldown, hp) == "Henüz yeniden içemezsin.");
    CHECK(cw::itemUseResultText(UR::AlreadyFull, hp) == "Canın zaten dolu.");
    CHECK(cw::itemUseResultText(UR::AlreadyFull, mp) == "Manan zaten dolu.");
    CHECK(cw::itemUseResultText(UR::Stunned, hp) == "Sersemken kullanamazsın.");
    CHECK(cw::itemUseResultText(UR::ServerError, hp) == "Kullanılamadı, tekrar dene.");
}