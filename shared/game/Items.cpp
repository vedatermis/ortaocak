#include "shared/game/Items.h"

#include "shared/core/Paths.h"
#include "shared/game/Character.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>

namespace vo::game {

namespace {

constexpr std::array<std::string_view, 4> kClassKeys = {"warrior", "rogue", "mage", "priest"}; // class 1..4
constexpr std::array<std::string_view, kEquipmentSlots> kEquipSlotNames = {"main_hand", "off_hand", "head",  "chest",
                                                                         "legs",      "hands",    "feet",  "reserved"};

std::optional<std::uint8_t> parseClass(std::string_view s)
{
    for (std::size_t i = 0; i < kClassKeys.size(); ++i)
        if (kClassKeys[i] == s)
            return static_cast<std::uint8_t>(i + 1);
    return std::nullopt;
}

std::optional<SlotRule> parseSlotRule(std::string_view s)
{
    if (s == "main_hand")
        return SlotRule::MainHand;
    if (s == "off_hand")
        return SlotRule::OffHand;
    if (s == "either_hand")
        return SlotRule::EitherHand;
    if (s == "two_hand")
        return SlotRule::TwoHand;
    // M15: armour slots.
    if (s == "head")
        return SlotRule::Head;
    if (s == "chest")
        return SlotRule::Chest;
    if (s == "legs")
        return SlotRule::Legs;
    if (s == "hands")
        return SlotRule::Hands;
    if (s == "feet")
        return SlotRule::Feet;
    return std::nullopt;
}

std::optional<ItemKind> parseKind(std::string_view s)
{
    if (s == "weapon")
        return ItemKind::Weapon;
    if (s == "shield")
        return ItemKind::Shield;
    if (s == "material")
        return ItemKind::Material;
    if (s == "consumable")
        return ItemKind::Consumable;
    if (s == "armor")
        return ItemKind::Armor;
    return std::nullopt;
}

std::optional<Rarity> parseRarity(std::string_view s)
{
    if (s == "common")
        return Rarity::Common;
    if (s == "uncommon")
        return Rarity::Uncommon;
    if (s == "rare")
        return Rarity::Rare;
    if (s == "unique")
        return Rarity::Unique;
    return std::nullopt;
}

std::optional<Grip> parseGrip(std::string_view s)
{
    if (s == "sword")
        return Grip::Sword;
    if (s == "dagger")
        return Grip::Dagger;
    if (s == "mace")
        return Grip::Mace;
    if (s == "staff")
        return Grip::Staff;
    if (s == "axe2h")
        return Grip::Axe2h;
    if (s == "shield")
        return Grip::Shield;
    return std::nullopt;
}

std::optional<EquipSlot> parseEquipSlot(std::string_view s)
{
    // "reserved" is not a name a file may use.
    for (std::size_t i = 0; i + 1 < kEquipSlotNames.size(); ++i)
        if (kEquipSlotNames[i] == s)
            return static_cast<EquipSlot>(i);
    return std::nullopt;
}

// Collects the first validation error with "<source>:<line>: " in front.
class Problems
{
public:
    explicit Problems(std::string_view source) : m_source(source) {}

    void add(const toml::node* at, const std::string& what)
    {
        if (!m_error.empty())
            return;
        if (at && at->source().begin.line > 0)
            m_error = std::format("{}:{}: {}", m_source, at->source().begin.line, what);
        else
            m_error = std::format("{}: {}", m_source, what);
    }
    [[nodiscard]] bool any() const noexcept { return !m_error.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

private:
    std::string m_source;
    std::string m_error;
};

bool onlyKeys(const toml::table& t, std::initializer_list<std::string_view> allowed, std::string_view what,
              Problems& p)
{
    for (auto&& [key, value] : t)
    {
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end())
        {
            p.add(&value, std::format("{}: unknown key '{}'", what, key.str()));
            return false;
        }
    }
    return true;
}

std::optional<std::string> getString(const toml::table& t, std::string_view key, std::string_view what, Problems& p,
                                     bool required = true)
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        if (required)
            p.add(&t, std::format("{}: missing '{}'", what, key));
        return std::nullopt;
    }
    const auto* s = n->as_string();
    if (!s)
    {
        p.add(n, std::format("{}: '{}' must be a string", what, key));
        return std::nullopt;
    }
    return s->get();
}

std::optional<std::int64_t> getInt(const toml::table& t, std::string_view key, std::string_view what, Problems& p,
                                   std::int64_t min, std::int64_t max)
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        p.add(&t, std::format("{}: missing '{}'", what, key));
        return std::nullopt;
    }
    const auto* i = n->as_integer();
    if (!i)
    {
        p.add(n, std::format("{}: '{}' must be an integer", what, key));
        return std::nullopt;
    }
    const std::int64_t v = i->get();
    if (v < min || v > max)
    {
        p.add(n, std::format("{}: '{}' = {} is out of range ({}..{})", what, key, v, min, max));
        return std::nullopt;
    }
    return v;
}

// Template id from a kit list/table value.
std::optional<ItemTemplateId> kitItemId(const toml::node& n, const std::vector<std::int32_t>& index,
                                        std::string_view what, Problems& p)
{
    const auto* i = n.as_integer();
    if (!i)
    {
        p.add(&n, std::format("{}: item ids must be integers", what));
        return std::nullopt;
    }
    const std::int64_t v = i->get();
    if (v < 1 || v > 65535 || index[static_cast<std::size_t>(v)] < 0)
    {
        p.add(&n, std::format("{}: unknown item id {}", what, v));
        return std::nullopt;
    }
    return static_cast<ItemTemplateId>(v);
}

bool parseItem(const toml::table& t, ItemTemplate& out, Problems& p)
{
    std::string what = "[[item]]";
    if (const auto id = getInt(t, "id", what, p, 1, 65535))
        out.id = static_cast<ItemTemplateId>(*id);
    else
        return false;
    what = std::format("item {}", out.id);
    if (!onlyKeys(t,
                  {"id", "name", "description", "kind", "slot", "classes", "level", "attack", "defense", "model", "icon",
                   "grip", "max_stack", "value", "rarity", "strength", "dexterity", "vitality", "intelligence", "spirit",
                   "req_strength", "req_dexterity", "req_vitality", "req_intelligence", "req_spirit", "restore_hp",
                   "restore_mp", "tint", "upgrade_stone"},
                  what, p))
        return false;

    const auto name = getString(t, "name", what, p);
    if (!name)
        return false;
    if (name->empty())
    {
        p.add(t.get("name"), what + ": empty name");
        return false;
    }
    out.name = *name;
    if (t.get("description"))
    {
        const auto d = getString(t, "description", what, p);
        if (!d)
            return false;
        out.description = *d;
    }

    const auto kind = getString(t, "kind", what, p);
    if (!kind)
        return false;
    if (const auto k = parseKind(*kind))
        out.kind = *k;
    else
    {
        p.add(t.get("kind"), std::format("{}: unknown kind '{}'", what, *kind));
        return false;
    }

    // M10 keys (docs/PROTOCOL_LOOT.md §7), optional for every kind.
    if (t.get("max_stack"))
    {
        const auto v = getInt(t, "max_stack", what, p, 1, kMaxStackLimit);
        if (!v)
            return false;
        out.maxStack = static_cast<std::uint16_t>(*v);
    }
    if (t.get("value"))
    {
        const auto v = getInt(t, "value", what, p, 0, 1'000'000);
        if (!v)
            return false;
        out.value = static_cast<std::uint32_t>(*v);
    }
    if (t.get("rarity"))
    {
        const auto r = getString(t, "rarity", what, p);
        if (!r)
            return false;
        if (const auto v = parseRarity(*r))
            out.rarity = *v;
        else
        {
            p.add(t.get("rarity"), std::format("{}: unknown rarity '{}'", what, *r));
            return false;
        }
    }

    // M15: optional tint [r, g, b] (0..255 each) for the icon and the weapon mesh.
    if (const toml::node* tn = t.get("tint"))
    {
        const auto* a = tn->as_array();
        bool ok = a && a->size() == 3;
        for (std::size_t i = 0; ok && i < 3; ++i)
        {
            const auto* v = a->get(i)->as_integer();
            ok = v && v->get() >= 0 && v->get() <= 255;
            if (ok)
                out.tint[i] = static_cast<std::uint8_t>(v->get());
        }
        if (!ok)
        {
            p.add(tn, std::format("{}: 'tint' must be [r, g, b] with integers 0..255", what));
            return false;
        }
    }
    // M16 (docs/PROTOCOL_UPGRADE.md §2): an upgrade stone is a stackable material of grade 1..3.
    if (t.get("upgrade_stone"))
    {
        if (out.kind != ItemKind::Material)
        {
            p.add(t.get("upgrade_stone"), std::format("{}: 'upgrade_stone' is only allowed for a material", what));
            return false;
        }
        const auto v = getInt(t, "upgrade_stone", what, p, 1, 3);
        if (!v)
            return false;
        out.upgradeStone = static_cast<std::uint8_t>(*v);
    }
    if (out.kind != ItemKind::Consumable)
    {
        for (const std::string_view key : {"restore_hp", "restore_mp"})
        {
            if (t.get(key))
            {
                p.add(t.get(key), std::format("{}: '{}' is only allowed for a consumable", what, key));
                return false;
            }
        }
    }
    if (out.kind == ItemKind::Material || out.kind == ItemKind::Consumable)
    {
        // A material / consumable has no equipment slot and no combat numbers: those keys must be absent.
        const std::string_view kindName = out.kind == ItemKind::Material ? "a material" : "a consumable";
        for (const std::string_view key :
             {"slot", "classes", "level", "attack", "defense", "model", "grip", "strength", "dexterity", "vitality",
              "intelligence", "spirit", "req_strength", "req_dexterity", "req_vitality", "req_intelligence",
              "req_spirit"})
        {
            if (t.get(key))
            {
                p.add(t.get(key), std::format("{}: '{}' is not allowed for {}", what, key, kindName));
                return false;
            }
        }
        const auto icon = getString(t, "icon", what, p);
        if (!icon)
            return false;
        out.icon = *icon;
        out.slot = SlotRule::None;
        out.classMask = 0;
        out.level = 1;
        out.attack = 0;
        out.defense = 0;
        if (out.kind == ItemKind::Consumable)
        {
            // PROTOCOL_SHOP.md §2.3: restores something, and its id fits a skill bar slot.
            if (out.id > kMaxConsumableTemplateId)
            {
                p.add(t.get("id"), std::format("{}: a consumable's id must be at most {}", what, kMaxConsumableTemplateId));
                return false;
            }
            for (const auto& [key, field] : {std::pair<std::string_view, std::uint32_t*>{"restore_hp", &out.restoreHp},
                                             std::pair<std::string_view, std::uint32_t*>{"restore_mp", &out.restoreMp}})
            {
                if (!t.get(key))
                    continue;
                const auto v = getInt(t, key, what, p, 0, kMaxRestoreAmount);
                if (!v)
                    return false;
                *field = static_cast<std::uint32_t>(*v);
            }
            if (out.restoreHp == 0 && out.restoreMp == 0)
            {
                p.add(&t, std::format("{}: a consumable must restore something (restore_hp / restore_mp)", what));
                return false;
            }
        }
        return true;
    }
    if (out.maxStack != 1)
    {
        p.add(t.get("max_stack"), std::format("{}: weapons and shields cannot stack (max_stack must be 1)", what));
        return false;
    }

    const auto slot = getString(t, "slot", what, p);
    if (!slot)
        return false;
    if (const auto s = parseSlotRule(*slot))
        out.slot = *s;
    else
    {
        p.add(t.get("slot"), std::format("{}: unknown slot '{}'", what, *slot));
        return false;
    }
    // M15: armour takes exactly one armour slot, weapons and shields only the hands.
    const bool armorSlot = out.slot == SlotRule::Head || out.slot == SlotRule::Chest || out.slot == SlotRule::Legs ||
                           out.slot == SlotRule::Hands || out.slot == SlotRule::Feet;
    if (armorSlot != (out.kind == ItemKind::Armor))
    {
        p.add(t.get("slot"), std::format("{}: slot '{}' does not fit kind '{}'", what, *slot, *kind));
        return false;
    }

    const toml::node* classes = t.get("classes");
    const toml::array* list = classes ? classes->as_array() : nullptr;
    if (!list || list->empty())
    {
        p.add(classes ? classes : &t, what + ": 'classes' must be a non-empty list of class names");
        return false;
    }
    for (const auto& c : *list)
    {
        const auto* s = c.as_string();
        const auto cls = s ? parseClass(s->get()) : std::nullopt;
        if (!cls)
        {
            p.add(&c, std::format("{}: unknown class {}", what,
                                  s ? std::format("'{}'", s->get()) : std::string("(not a string)")));
            return false;
        }
        if (out.classMask & (1u << *cls))
        {
            p.add(&c, std::format("{}: class '{}' listed twice", what, s->get()));
            return false;
        }
        out.classMask = static_cast<std::uint8_t>(out.classMask | (1u << *cls));
    }

    const auto level = getInt(t, "level", what, p, 1, 65535);
    const auto attack = getInt(t, "attack", what, p, 0, 1'000'000);
    const auto defense = getInt(t, "defense", what, p, 0, 1'000'000);
    if (!level || !attack || !defense)
        return false;
    out.level = static_cast<std::uint16_t>(*level);
    out.attack = static_cast<std::int32_t>(*attack);
    out.defense = static_cast<std::int32_t>(*defense);

    // M11 (docs/PROTOCOL_STATS.md §5): optional attribute bonuses and requirements.
    for (std::size_t i = 0; i < kAttributeCount; ++i)
    {
        const std::string key(attributeKey(static_cast<Attribute>(i)));
        if (t.get(key))
        {
            const auto v = getInt(t, key, what, p, 0, kMaxItemAttributeBonus);
            if (!v)
                return false;
            out.attributes[i] = static_cast<std::uint16_t>(*v);
        }
        const std::string req = "req_" + key;
        if (t.get(req))
        {
            const auto v = getInt(t, req, what, p, 0, kAttributeCap);
            if (!v)
                return false;
            out.requirements[i] = static_cast<std::uint16_t>(*v);
        }
    }

    if (out.kind == ItemKind::Armor)
    {
        // M15: armour is not drawn on the character yet - no model, no grip; an icon.
        for (const std::string_view key : {"model", "grip"})
            if (t.get(key))
            {
                p.add(t.get(key), std::format("{}: '{}' is not allowed for armour", what, key));
                return false;
            }
        const auto icon = getString(t, "icon", what, p);
        if (!icon)
            return false;
        out.icon = *icon;
        return true;
    }
    const auto model = getString(t, "model", what, p);
    const auto icon = getString(t, "icon", what, p);
    const auto grip = getString(t, "grip", what, p);
    if (!model || !icon || !grip)
        return false;
    out.model = *model;
    out.icon = *icon;
    if (const auto g = parseGrip(*grip))
        out.grip = *g;
    else
    {
        p.add(t.get("grip"), std::format("{}: unknown grip '{}'", what, *grip));
        return false;
    }
    return true;
}

bool parseKit(const toml::table& t, const std::vector<ItemTemplate>& templates, const std::vector<std::int32_t>& index,
              StarterKit& out, Problems& p)
{
    std::string what = "[[starter_kit]]";
    if (!onlyKeys(t, {"class", "equip", "bag"}, what, p))
        return false;
    const auto cls = getString(t, "class", what, p);
    if (!cls)
        return false;
    const auto clsValue = parseClass(*cls);
    if (!clsValue)
    {
        p.add(t.get("class"), std::format("{}: unknown class '{}'", what, *cls));
        return false;
    }
    out.charClass = *clsValue;
    what = std::format("starter kit '{}'", *cls);
    const ItemUser user{out.charClass, kStartLevel};
    const auto tmpl = [&](ItemTemplateId id) -> const ItemTemplate& {
        return templates[static_cast<std::size_t>(index[id])];
    };

    std::size_t count = 0;
    if (const toml::node* equip = t.get("equip"))
    {
        const auto* table = equip->as_table();
        if (!table)
        {
            p.add(equip, what + ": 'equip' must be a table of slot = item id");
            return false;
        }
        for (auto&& [key, value] : *table)
        {
            const auto slot = parseEquipSlot(key.str());
            if (!slot)
            {
                p.add(&value, std::format("{}: unknown equipment slot '{}'", what, key.str()));
                return false;
            }
            const auto id = kitItemId(value, index, what, p);
            if (!id)
                return false;
            const ItemMoveResult r = checkEquip(tmpl(*id), *slot, user);
            if (r != ItemMoveResult::Ok)
            {
                p.add(&value, std::format("{}: item {} cannot be equipped in {} ({})", what, *id, key.str(),
                                          net::itemMoveResultName(r)));
                return false;
            }
            out.equip[static_cast<std::size_t>(*slot)] = *id;
            ++count;
        }
        const ItemTemplateId main = out.equip[static_cast<std::size_t>(EquipSlot::MainHand)];
        if (main != 0 && tmpl(main).isTwoHanded() && out.equip[static_cast<std::size_t>(EquipSlot::OffHand)] != 0)
        {
            p.add(equip, what + ": off_hand must be empty when main_hand is two-handed");
            return false;
        }
    }
    if (const toml::node* bag = t.get("bag"))
    {
        const auto* list = bag->as_array();
        if (!list)
        {
            p.add(bag, what + ": 'bag' must be a list of item ids");
            return false;
        }
        for (const auto& v : *list)
        {
            const auto id = kitItemId(v, index, what, p);
            if (!id)
                return false;
            out.bag.push_back(*id);
        }
        if (out.bag.size() > kBagSlots)
        {
            p.add(bag, std::format("{}: {} bag items do not fit into {} bag slots", what, out.bag.size(), kBagSlots));
            return false;
        }
    }
    (void)count;
    return true;
}

} // namespace

// ---- Catalog ----

const ItemTemplate* ItemCatalog::find(ItemTemplateId id) const noexcept
{
    if (id == 0 || id >= m_index.size() || m_index[id] < 0)
        return nullptr;
    return &m_templates[static_cast<std::size_t>(m_index[id])];
}

const StarterKit* ItemCatalog::kitFor(std::uint8_t charClass) const noexcept
{
    if (charClass >= m_kits.size() || !m_kits[charClass])
        return nullptr;
    return &*m_kits[charClass];
}

bool parseItemCatalog(std::string_view tomlText, std::string_view sourceName, ItemCatalog& out, std::string& error)
{
    toml::table root;
    try
    {
        root = toml::parse(tomlText, sourceName);
    }
    catch (const toml::parse_error& e)
    {
        const auto& b = e.source().begin;
        error = std::format("{}:{}:{}: {}", sourceName, b.line, b.column, e.description());
        return false;
    }

    Problems p(sourceName);
    ItemCatalog catalog;
    catalog.m_index.assign(65536, -1);
    if (!onlyKeys(root, {"item", "starter_kit"}, "top level", p))
    {
        error = p.error();
        return false;
    }

    const toml::node* items = root.get("item");
    const toml::array* itemList = items ? items->as_array() : nullptr;
    if (!itemList || itemList->empty())
    {
        p.add(items, "no [[item]] entries");
        error = p.error();
        return false;
    }
    for (const auto& node : *itemList)
    {
        const auto* t = node.as_table();
        if (!t)
        {
            p.add(&node, "'item' must be an array of tables ([[item]])");
            break;
        }
        ItemTemplate tmpl;
        if (!parseItem(*t, tmpl, p))
            break;
        if (catalog.m_index[tmpl.id] >= 0)
        {
            p.add(t, std::format("duplicate item id {}", tmpl.id));
            break;
        }
        catalog.m_index[tmpl.id] = static_cast<std::int32_t>(catalog.m_templates.size());
        catalog.m_templates.push_back(std::move(tmpl));
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    // Sort by id and rebuild the index.
    std::sort(catalog.m_templates.begin(), catalog.m_templates.end(),
              [](const ItemTemplate& a, const ItemTemplate& b) { return a.id < b.id; });
    catalog.m_index.assign(65536, -1);
    for (std::size_t i = 0; i < catalog.m_templates.size(); ++i)
        catalog.m_index[catalog.m_templates[i].id] = static_cast<std::int32_t>(i);

    const toml::node* kits = root.get("starter_kit");
    const toml::array* kitList = kits ? kits->as_array() : nullptr;
    if (kits && !kitList)
    {
        p.add(kits, "'starter_kit' must be an array of tables ([[starter_kit]])");
        error = p.error();
        return false;
    }
    if (kitList)
    {
        for (const auto& node : *kitList)
        {
            const auto* t = node.as_table();
            if (!t)
            {
                p.add(&node, "'starter_kit' must be an array of tables ([[starter_kit]])");
                break;
            }
            StarterKit kit;
            if (!parseKit(*t, catalog.m_templates, catalog.m_index, kit, p))
                break;
            if (catalog.m_kits[kit.charClass])
            {
                p.add(t, std::format("duplicate starter kit for class '{}'", classKey(kit.charClass)));
                break;
            }
            catalog.m_kits[kit.charClass] = std::move(kit);
        }
    }
    for (std::uint8_t cls = static_cast<std::uint8_t>(CharClass::Warrior);
         !p.any() && cls <= static_cast<std::uint8_t>(CharClass::Priest); ++cls)
    {
        if (!catalog.m_kits[cls])
            p.add(nullptr, std::format("no [[starter_kit]] for class '{}'", classKey(cls)));
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    out = std::move(catalog);
    return true;
}

bool loadItemCatalog(const std::filesystem::path& file, ItemCatalog& out, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        error = std::format("cannot open {}", core::pathToUtf8(file));
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
    {
        error = std::format("cannot read {}", core::pathToUtf8(file));
        return false;
    }
    return parseItemCatalog(ss.str(), core::pathToUtf8(file), out, error);
}

std::optional<std::filesystem::path> locateItemsFile()
{
    return core::locateDataFile(std::filesystem::path(kItemsFile));
}

std::string_view slotRuleName(SlotRule r) noexcept
{
    switch (r)
    {
    case SlotRule::MainHand: return "main_hand";
    case SlotRule::OffHand: return "off_hand";
    case SlotRule::EitherHand: return "either_hand";
    case SlotRule::TwoHand: return "two_hand";
    case SlotRule::None: return "none";
    case SlotRule::Head: return "head";
    case SlotRule::Chest: return "chest";
    case SlotRule::Legs: return "legs";
    case SlotRule::Hands: return "hands";
    case SlotRule::Feet: return "feet";
    }
    return "?";
}

std::string_view itemKindName(ItemKind k) noexcept
{
    switch (k)
    {
    case ItemKind::Weapon: return "weapon";
    case ItemKind::Shield: return "shield";
    case ItemKind::Material: return "material";
    case ItemKind::Consumable: return "consumable";
    case ItemKind::Armor: return "armor";
    }
    return "?";
}

std::string_view rarityName(Rarity r) noexcept
{
    switch (r)
    {
    case Rarity::Common: return "common";
    case Rarity::Uncommon: return "uncommon";
    case Rarity::Rare: return "rare";
    case Rarity::Unique: return "unique";
    }
    return "?";
}

std::string_view rarityDisplayName(Rarity r) noexcept
{
    switch (r)
    {
    case Rarity::Common: return "Sıradan";
    case Rarity::Uncommon: return "Seyrek";
    case Rarity::Rare: return "Nadir";
    case Rarity::Unique: return "Eşsiz";
    }
    return "?";
}

std::string_view gripName(Grip g) noexcept
{
    switch (g)
    {
    case Grip::Sword: return "sword";
    case Grip::Dagger: return "dagger";
    case Grip::Mace: return "mace";
    case Grip::Staff: return "staff";
    case Grip::Axe2h: return "axe2h";
    case Grip::Shield: return "shield";
    }
    return "?";
}

std::string_view equipSlotName(EquipSlot s) noexcept
{
    const auto i = static_cast<std::size_t>(s);
    return i < kEquipSlotNames.size() ? kEquipSlotNames[i] : std::string_view("?");
}

std::string_view classKey(std::uint8_t charClass) noexcept
{
    if (charClass >= 1 && charClass <= kClassKeys.size())
        return kClassKeys[charClass - 1u];
    return "?";
}

std::string_view itemKindDisplayName(ItemKind k) noexcept
{
    switch (k)
    {
    case ItemKind::Weapon: return "Silah";
    case ItemKind::Shield: return "Kalkan";
    case ItemKind::Material: return "Malzeme";
    case ItemKind::Consumable: return "Tüketilebilir";
    case ItemKind::Armor: return "Zırh";
    }
    return "?";
}

std::string_view slotRuleDisplayName(SlotRule r) noexcept
{
    switch (r)
    {
    case SlotRule::MainHand: return "Sağ el";
    case SlotRule::OffHand: return "Sol el";
    case SlotRule::EitherHand: return "Tek el";
    case SlotRule::TwoHand: return "İki el";
    case SlotRule::None: return "";
    case SlotRule::Head: return "Baş";
    case SlotRule::Chest: return "Gövde";
    case SlotRule::Legs: return "Bacak";
    case SlotRule::Hands: return "Eller";
    case SlotRule::Feet: return "Ayak";
    }
    return "?";
}

// ---- Inventory ----

bool Inventory::fromItems(const std::vector<StoredItem>& items, Inventory& out, std::string& error)
{
    Inventory inv;
    for (const auto& item : items)
    {
        if (!isValidPlace(item.place))
        {
            error = std::format("item {} has an invalid place ({}, {})", item.id, item.place.container, item.place.slot);
            return false;
        }
        if (item.id == 0 || item.quantity == 0)
        {
            error = std::format("item {} has a zero id or quantity", item.id);
            return false;
        }
        if (item.upgrade > kMaxUpgradeLevel || (item.upgrade > 0 && item.quantity != 1))
        {
            error = std::format("item {} has an invalid upgrade level {} (quantity {})", item.id, item.upgrade,
                                item.quantity);
            return false;
        }
        ItemStack& s = inv.at(item.place);
        if (!s.empty())
        {
            error = std::format("items {} and {} share the place ({}, {})", s.id, item.id, item.place.container,
                                item.place.slot);
            return false;
        }
        s = {item.id, item.templateId, item.quantity, item.upgrade};
    }
    out = inv;
    return true;
}

const ItemStack& Inventory::at(ItemPlace p) const noexcept
{
    return p.isEquipment() ? m_equipment[p.slot] : m_bag[p.slot];
}

ItemStack& Inventory::at(ItemPlace p) noexcept
{
    return p.isEquipment() ? m_equipment[p.slot] : m_bag[p.slot];
}

std::optional<std::uint8_t> Inventory::firstFreeBagSlot() const noexcept
{
    for (std::size_t i = 0; i < m_bag.size(); ++i)
        if (m_bag[i].empty())
            return static_cast<std::uint8_t>(i);
    return std::nullopt;
}

std::vector<StoredItem> Inventory::items() const
{
    std::vector<StoredItem> out;
    for (std::size_t i = 0; i < m_equipment.size(); ++i)
        if (const auto& s = m_equipment[i]; !s.empty())
            out.push_back({s.id, ItemPlace::equipment(static_cast<EquipSlot>(i)), s.templateId, s.quantity, s.upgrade});
    for (std::size_t i = 0; i < m_bag.size(); ++i)
        if (const auto& s = m_bag[i]; !s.empty())
            out.push_back({s.id, ItemPlace::bag(static_cast<std::uint8_t>(i)), s.templateId, s.quantity, s.upgrade});
    return out;
}

std::size_t Inventory::count() const noexcept
{
    const auto occupied = [](const ItemStack& s) { return !s.empty(); };
    return static_cast<std::size_t>(std::count_if(m_equipment.begin(), m_equipment.end(), occupied) +
                                    std::count_if(m_bag.begin(), m_bag.end(), occupied));
}

VisibleEquipment visibleEquipment(const ItemCatalog& catalog, const Inventory& inv) noexcept
{
    const auto shown = [&](EquipSlot s) -> ItemTemplateId {
        const ItemStack& st = inv.equipped(s);
        return !st.empty() && catalog.find(st.templateId) ? st.templateId : ItemTemplateId{0};
    };
    return {shown(EquipSlot::MainHand), shown(EquipSlot::OffHand)};
}

VisibleEquipment starterKitVisible(const ItemCatalog& catalog, std::uint8_t charClass) noexcept
{
    const StarterKit* kit = catalog.kitFor(charClass);
    if (!kit)
        return {};
    return {kit->equip[static_cast<std::size_t>(EquipSlot::MainHand)],
            kit->equip[static_cast<std::size_t>(EquipSlot::OffHand)]};
}

// ---- Rules ----

ItemMoveResult checkEquip(const ItemTemplate& t, EquipSlot slot, const ItemUser& user) noexcept
{
    bool slotOk = false;
    switch (slot)
    {
    case EquipSlot::MainHand:
        slotOk = t.slot == SlotRule::MainHand || t.slot == SlotRule::EitherHand || t.slot == SlotRule::TwoHand;
        break;
    case EquipSlot::OffHand: slotOk = t.slot == SlotRule::OffHand || t.slot == SlotRule::EitherHand; break;
    case EquipSlot::Head: slotOk = t.slot == SlotRule::Head; break; // M15: armour
    case EquipSlot::Chest: slotOk = t.slot == SlotRule::Chest; break;
    case EquipSlot::Legs: slotOk = t.slot == SlotRule::Legs; break;
    case EquipSlot::Hands: slotOk = t.slot == SlotRule::Hands; break;
    case EquipSlot::Feet: slotOk = t.slot == SlotRule::Feet; break;
    default: slotOk = false; break; // Reserved: no templates
    }
    if (!slotOk)
        return ItemMoveResult::WrongSlot;
    if (!t.allowsClass(user.charClass))
        return ItemMoveResult::WrongClass;
    if (user.level < t.level)
        return ItemMoveResult::LevelTooLow;
    for (std::size_t i = 0; i < kAttributeCount; ++i)
        if (user.attributes[i] < t.requirements[i])
            return ItemMoveResult::AttributeTooLow;
    return ItemMoveResult::Ok;
}

std::vector<ItemMovePlan::Relocation> ItemMovePlan::relocations(const Inventory& before) const
{
    std::vector<Relocation> out;
    if (result != ItemMoveResult::Ok || isMerge())
        return out;
    // Where each item was before the move, found by id among the changed slots.
    for (const auto& c : changes)
    {
        if (c.content.empty())
            continue;
        for (const auto& old : changes)
        {
            if (before.at(old.place).id == c.content.id)
            {
                out.push_back({c.content.id, old.place, c.place});
                break;
            }
        }
    }
    return out;
}

ItemMovePlan planItemMove(const ItemCatalog& catalog, const Inventory& inv, const ItemUser& user, ItemPlace from,
                          ItemPlace to)
{
    ItemMovePlan plan;
    const auto fail = [&](ItemMoveResult r) {
        plan.result = r;
        plan.changes.clear();
        return plan;
    };
    if (!isValidPlace(from) || !isValidPlace(to) || from == to)
        return fail(ItemMoveResult::BadSlot);

    const ItemStack moved = inv.at(from);
    const ItemTemplate* movedT = moved.empty() ? nullptr : catalog.find(moved.templateId);
    if (!movedT)
        return fail(ItemMoveResult::NoItem); // empty, or hidden (looks empty to the client)
    const ItemStack displaced = inv.at(to);
    const ItemTemplate* displacedT = displaced.empty() ? nullptr : catalog.find(displaced.templateId);
    if (!displaced.empty() && !displacedT)
        return fail(ItemMoveResult::SlotBlocked); // a hidden item occupies the target

    // PROTOCOL_LOOT.md §1.2: bag -> bag onto a non-full stack of the same stackable template merges.
    if (from.isBag() && to.isBag() && displacedT == movedT && movedT->isStackable() &&
        displaced.quantity < movedT->maxStack)
    {
        const auto n = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(moved.quantity, static_cast<std::uint32_t>(movedT->maxStack - displaced.quantity)));
        const auto toAfter = static_cast<std::uint16_t>(displaced.quantity + n);
        const auto fromAfter = static_cast<std::uint16_t>(moved.quantity - n);
        plan.result = ItemMoveResult::Ok;
        plan.changes.push_back({to, {displaced.id, displaced.templateId, toAfter}});
        plan.changes.push_back({from, fromAfter == 0 ? ItemStack{} : ItemStack{moved.id, moved.templateId, fromAfter}});
        plan.merge.push_back({displaced.id, to, displaced.quantity, toAfter});
        plan.merge.push_back({moved.id, from, moved.quantity, fromAfter});
        return plan;
    }

    if (to.isEquipment())
    {
        const ItemMoveResult r = checkEquip(*movedT, static_cast<EquipSlot>(to.slot), user);
        if (r != ItemMoveResult::Ok)
            return fail(r);
    }
    if (displacedT && from.isEquipment())
    {
        const ItemMoveResult r = checkEquip(*displacedT, static_cast<EquipSlot>(from.slot), user);
        if (r != ItemMoveResult::Ok)
            return fail(r);
    }

    Inventory after = inv;
    after.at(to) = moved;
    after.at(from) = displaced;

    const ItemPlace mainPlace = ItemPlace::equipment(EquipSlot::MainHand);
    const ItemPlace offPlace = ItemPlace::equipment(EquipSlot::OffHand);
    const ItemStack& main = after.at(mainPlace);
    const ItemTemplate* mainT = main.empty() ? nullptr : catalog.find(main.templateId);
    std::optional<ItemPlace> displacedOffTo;
    if (mainT && mainT->isTwoHanded() && !after.at(offPlace).empty())
    {
        const bool offChanged = from == offPlace || to == offPlace;
        if (offChanged)
            return fail(ItemMoveResult::SlotBlocked); // something lands in OffHand next to a two-hander
        const ItemStack off = after.at(offPlace);
        if (!catalog.find(off.templateId))
            return fail(ItemMoveResult::SlotBlocked); // hidden items are never moved implicitly
        const auto free = after.firstFreeBagSlot();
        if (!free)
            return fail(ItemMoveResult::BagFull);
        displacedOffTo = ItemPlace::bag(*free);
        after.at(*displacedOffTo) = off;
        after.at(offPlace) = {};
    }

    plan.result = ItemMoveResult::Ok;
    plan.changes.push_back({to, after.at(to)});
    plan.changes.push_back({from, after.at(from)});
    if (displacedOffTo)
    {
        plan.changes.push_back({offPlace, after.at(offPlace)});
        plan.changes.push_back({*displacedOffTo, after.at(*displacedOffTo)});
    }
    return plan;
}

void applyItemMove(Inventory& inv, const ItemMovePlan& plan)
{
    if (plan.result != ItemMoveResult::Ok)
        return;
    for (const auto& c : plan.changes)
        inv.at(c.place) = c.content;
}

std::optional<ItemPlace> quickMoveTarget(const ItemCatalog& catalog, const Inventory& inv, ItemPlace from)
{
    if (!isValidPlace(from))
        return std::nullopt;
    const ItemStack& s = inv.at(from);
    const ItemTemplate* t = s.empty() ? nullptr : catalog.find(s.templateId);
    if (!t)
        return std::nullopt;
    if (from.isEquipment())
    {
        const auto free = inv.firstFreeBagSlot();
        return free ? std::optional<ItemPlace>(ItemPlace::bag(*free)) : std::nullopt;
    }
    switch (t->slot)
    {
    case SlotRule::MainHand:
    case SlotRule::TwoHand: return ItemPlace::equipment(EquipSlot::MainHand);
    case SlotRule::OffHand: return ItemPlace::equipment(EquipSlot::OffHand);
    case SlotRule::EitherHand:
        if (!inv.equipped(EquipSlot::MainHand).empty() && inv.equipped(EquipSlot::OffHand).empty())
            return ItemPlace::equipment(EquipSlot::OffHand);
        return ItemPlace::equipment(EquipSlot::MainHand);
    case SlotRule::None: return std::nullopt; // materials cannot be equipped
    case SlotRule::Head: return ItemPlace::equipment(EquipSlot::Head);
    case SlotRule::Chest: return ItemPlace::equipment(EquipSlot::Chest);
    case SlotRule::Legs: return ItemPlace::equipment(EquipSlot::Legs);
    case SlotRule::Hands: return ItemPlace::equipment(EquipSlot::Hands);
    case SlotRule::Feet: return ItemPlace::equipment(EquipSlot::Feet);
    }
    return std::nullopt;
}

// ---- Adding units ----

std::uint32_t ItemAddPlan::totalPlaced() const noexcept
{
    std::uint32_t n = 0;
    for (const std::uint32_t p : placed)
        n += p;
    return n;
}

std::vector<ItemSlotChange> ItemAddPlan::changes(const Inventory& before,
                                                 const std::vector<ItemInstanceId>& createdIds) const
{
    std::vector<ItemSlotChange> out;
    for (const auto& g : grown)
    {
        const ItemStack& s = before.at(g.place);
        out.push_back({g.place, {s.id, s.templateId, g.after}});
    }
    for (std::size_t i = 0; i < created.size(); ++i)
    {
        const NewItem& n = created[i];
        out.push_back({n.place, {i < createdIds.size() ? createdIds[i] : ItemInstanceId{0}, n.templateId, n.quantity}});
    }
    return out;
}

ItemAddPlan planItemAdd(const ItemCatalog& catalog, const Inventory& inv, const std::vector<ItemAddRequest>& requests)
{
    ItemAddPlan plan;
    // Working view of the bag: the quantity per slot after the plan so far, and which slots the plan
    // creates (index into plan.created) or grows (index into plan.grown).
    struct Slot
    {
        ItemTemplateId templateId = 0;
        std::uint32_t quantity = 0;
        bool occupied = false;
        int created = -1;
        int grown = -1;
    };
    std::array<Slot, kBagSlots> bag{};
    for (std::size_t i = 0; i < kBagSlots; ++i)
    {
        const ItemStack& s = inv.at(ItemPlace::bag(static_cast<std::uint8_t>(i)));
        if (!s.empty())
            bag[i] = {s.templateId, s.quantity, true, -1, -1};
    }
    for (const ItemAddRequest& req : requests)
    {
        std::uint32_t left = req.count;
        const ItemTemplate* t = catalog.find(req.templateId);
        if (!t || left == 0)
        {
            plan.placed.push_back(0);
            continue;
        }
        const std::uint32_t max = t->maxStack;
        // 1. Existing (or newly created) stacks of the template that are not full, lowest slot first.
        for (std::size_t i = 0; i < kBagSlots && left > 0; ++i)
        {
            Slot& s = bag[i];
            if (!s.occupied || s.templateId != req.templateId || s.quantity >= max)
                continue;
            const std::uint32_t n = std::min(left, max - s.quantity);
            s.quantity += n;
            left -= n;
            const auto place = ItemPlace::bag(static_cast<std::uint8_t>(i));
            if (s.created >= 0)
            {
                plan.created[static_cast<std::size_t>(s.created)].quantity = static_cast<std::uint16_t>(s.quantity);
            }
            else if (s.grown >= 0)
            {
                plan.grown[static_cast<std::size_t>(s.grown)].after = static_cast<std::uint16_t>(s.quantity);
            }
            else
            {
                const ItemStack& old = inv.at(place);
                s.grown = static_cast<int>(plan.grown.size());
                plan.grown.push_back({old.id, place, old.quantity, static_cast<std::uint16_t>(s.quantity)});
            }
        }
        // 2. The lowest free bag slots.
        for (std::size_t i = 0; i < kBagSlots && left > 0; ++i)
        {
            Slot& s = bag[i];
            if (s.occupied)
                continue;
            const std::uint32_t n = std::min(left, max);
            s = {req.templateId, n, true, static_cast<int>(plan.created.size()), -1};
            left -= n;
            plan.created.push_back({ItemPlace::bag(static_cast<std::uint8_t>(i)), req.templateId,
                                    static_cast<std::uint16_t>(n)});
        }
        plan.placed.push_back(req.count - left);
    }
    return plan;
}

void applyItemAdd(Inventory& inv, const ItemAddPlan& plan, const std::vector<ItemInstanceId>& createdIds)
{
    for (const auto& c : plan.changes(inv, createdIds))
        inv.at(c.place) = c.content;
}

// ---- Starter kit ----

std::vector<NewItem> starterKitItems(const StarterKit& kit, const std::vector<StoredItem>& existing,
                                     std::size_t* dropped)
{
    std::array<bool, kEquipmentSlots> equipUsed{};
    std::array<bool, kBagSlots> bagUsed{};
    for (const auto& e : existing)
    {
        if (!isValidPlace(e.place))
            continue;
        if (e.place.isEquipment())
            equipUsed[e.place.slot] = true;
        else
            bagUsed[e.place.slot] = true;
    }
    std::vector<NewItem> out;
    std::size_t lost = 0;
    const auto toBag = [&](ItemTemplateId id) {
        for (std::size_t i = 0; i < kBagSlots; ++i)
        {
            if (!bagUsed[i])
            {
                bagUsed[i] = true;
                out.push_back({ItemPlace::bag(static_cast<std::uint8_t>(i)), id, 1});
                return;
            }
        }
        ++lost;
    };
    for (std::size_t s = 0; s < kEquipmentSlots; ++s)
    {
        const ItemTemplateId id = kit.equip[s];
        if (id == 0)
            continue;
        if (!equipUsed[s])
        {
            equipUsed[s] = true;
            out.push_back({ItemPlace::equipment(static_cast<EquipSlot>(s)), id, 1});
        }
        else
        {
            toBag(id);
        }
    }
    for (const ItemTemplateId id : kit.bag)
        toBag(id);
    if (dropped)
        *dropped = lost;
    return out;
}

} // namespace vo::game
