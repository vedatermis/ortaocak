#include "shared/game/Loot.h"

#include "shared/core/Paths.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>

namespace vo::game {

namespace {

// Collects the first validation error with "<source>:<line>: " in front (as Items.cpp).
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

std::optional<std::int64_t> intValue(const toml::node& n, std::string_view what, Problems& p, std::int64_t min,
                                     std::int64_t max)
{
    const auto* i = n.as_integer();
    if (!i)
    {
        p.add(&n, std::format("{}: expected an integer", what));
        return std::nullopt;
    }
    const std::int64_t v = i->get();
    if (v < min || v > max)
    {
        p.add(&n, std::format("{}: {} is out of range ({}..{})", what, v, min, max));
        return std::nullopt;
    }
    return v;
}

// `key = [min, max]` with min <= max, both in [lo, hi]; `fallback` if the key is absent.
std::optional<std::pair<std::int64_t, std::int64_t>> getRange(const toml::table& t, std::string_view key,
                                                              std::string_view what, Problems& p, std::int64_t lo,
                                                              std::int64_t hi, std::pair<std::int64_t, std::int64_t> fallback)
{
    const toml::node* n = t.get(key);
    if (!n)
        return fallback;
    const auto* a = n->as_array();
    if (!a || a->size() != 2)
    {
        p.add(n, std::format("{}: '{}' must be [min, max]", what, key));
        return std::nullopt;
    }
    const std::string label = std::format("{}: '{}'", what, key);
    const auto min = intValue(*a->get(0), label, p, lo, hi);
    const auto max = intValue(*a->get(1), label, p, lo, hi);
    if (!min || !max)
        return std::nullopt;
    if (*min > *max)
    {
        p.add(n, std::format("{}: '{}' min {} is above max {}", what, key, *min, *max));
        return std::nullopt;
    }
    return std::pair{*min, *max};
}

bool parseDrop(const toml::table& t, const ItemCatalog& items, std::string_view tableWhat, LootDrop& out, Problems& p)
{
    std::string what = std::format("{} drop", tableWhat);
    if (!onlyKeys(t, {"item", "chance", "count"}, what, p))
        return false;
    const toml::node* itemNode = t.get("item");
    if (!itemNode)
    {
        p.add(&t, what + ": missing 'item'");
        return false;
    }
    const auto id = intValue(*itemNode, what + ": 'item'", p, 1, 65535);
    if (!id)
        return false;
    const ItemTemplate* tmpl = items.find(static_cast<ItemTemplateId>(*id));
    if (!tmpl)
    {
        p.add(itemNode, std::format("{}: unknown item id {}", what, *id));
        return false;
    }
    out.item = tmpl->id;
    what = std::format("{} drop of item {}", tableWhat, out.item);

    const toml::node* chanceNode = t.get("chance");
    if (!chanceNode)
    {
        p.add(&t, what + ": missing 'chance'");
        return false;
    }
    double chance = 0.0;
    if (const auto* f = chanceNode->as_floating_point())
        chance = f->get();
    else if (const auto* i = chanceNode->as_integer())
        chance = static_cast<double>(i->get());
    else
    {
        p.add(chanceNode, what + ": 'chance' must be a number");
        return false;
    }
    if (!std::isfinite(chance) || chance <= 0.0 || chance > 1.0)
    {
        p.add(chanceNode, std::format("{}: 'chance' = {} must be in (0, 1]", what, chance));
        return false;
    }
    out.chance = chance;

    const auto count = getRange(t, "count", what, p, 1, tmpl->maxStack, {1, 1});
    if (!count)
        return false;
    out.minCount = static_cast<std::uint16_t>(count->first);
    out.maxCount = static_cast<std::uint16_t>(count->second);
    out.unique = tmpl->rarity == Rarity::Unique;
    return true;
}

bool parseTable(const toml::table& t, const ItemCatalog& items, const MonsterData* monsters,
                std::vector<std::int32_t>& byMonster, std::int32_t index, LootTable& out, Problems& p)
{
    std::string what = "[[table]]";
    if (!onlyKeys(t, {"monsters", "coins", "drop"}, what, p))
        return false;
    const toml::node* list = t.get("monsters");
    const toml::array* ids = list ? list->as_array() : nullptr;
    if (!ids || ids->empty())
    {
        p.add(list ? list : &t, what + ": 'monsters' must be a non-empty list of monster ids");
        return false;
    }
    for (const toml::node& n : *ids)
    {
        const auto id = intValue(n, what + ": 'monsters'", p, 1, 65535);
        if (!id)
            return false;
        const auto mid = static_cast<MonsterTemplateId>(*id);
        if (monsters && !monsters->find(mid))
        {
            p.add(&n, std::format("{}: unknown monster id {}", what, mid));
            return false;
        }
        if (byMonster[mid] >= 0)
        {
            p.add(&n, std::format("{}: monster {} is already in another table", what, mid));
            return false;
        }
        byMonster[mid] = index;
        out.monsters.push_back(mid);
    }
    what = std::format("loot table of monster {}", out.monsters.front());

    const auto coins = getRange(t, "coins", what, p, 0, kMaxLootCoins, {0, 0});
    if (!coins)
        return false;
    out.coinsMin = static_cast<std::uint32_t>(coins->first);
    out.coinsMax = static_cast<std::uint32_t>(coins->second);

    if (const toml::node* drops = t.get("drop"))
    {
        const toml::array* a = drops->as_array();
        if (!a)
        {
            p.add(drops, what + ": 'drop' must be an array of tables ([[table.drop]])");
            return false;
        }
        if (a->size() > kMaxLootLines)
        {
            p.add(drops, std::format("{}: {} drop lines, at most {}", what, a->size(), kMaxLootLines));
            return false;
        }
        for (const toml::node& n : *a)
        {
            const toml::table* d = n.as_table();
            if (!d)
            {
                p.add(&n, what + ": 'drop' must be an array of tables ([[table.drop]])");
                return false;
            }
            LootDrop drop;
            if (!parseDrop(*d, items, what, drop, p))
                return false;
            if (std::any_of(out.drops.begin(), out.drops.end(), [&](const LootDrop& o) { return o.item == drop.item; }))
            {
                p.add(d, std::format("{}: item {} is listed twice", what, drop.item));
                return false;
            }
            out.drops.push_back(drop);
        }
    }
    return true;
}

} // namespace

const LootTable* LootTables::forMonster(MonsterTemplateId monster) const noexcept
{
    if (monster >= m_byMonster.size() || m_byMonster[monster] < 0)
        return nullptr;
    return &m_tables[static_cast<std::size_t>(m_byMonster[monster])];
}

bool parseLootTables(std::string_view tomlText, std::string_view sourceName, const ItemCatalog& items,
                     const MonsterData* monsters, LootTables& out, std::string& error)
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
    LootTables tables;
    tables.m_byMonster.assign(65536, -1);
    if (!onlyKeys(root, {"table"}, "top level", p))
    {
        error = p.error();
        return false;
    }
    if (const toml::node* node = root.get("table"))
    {
        const toml::array* list = node->as_array();
        if (!list)
            p.add(node, "'table' must be an array of tables ([[table]])");
        for (std::size_t i = 0; list && i < list->size() && !p.any(); ++i)
        {
            const toml::table* t = list->get(i)->as_table();
            if (!t)
            {
                p.add(list->get(i), "'table' must be an array of tables ([[table]])");
                break;
            }
            LootTable table;
            if (!parseTable(*t, items, monsters, tables.m_byMonster, static_cast<std::int32_t>(tables.m_tables.size()),
                            table, p))
                break;
            tables.m_tables.push_back(std::move(table));
        }
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    out = std::move(tables);
    return true;
}

bool loadLootTables(const std::filesystem::path& file, const ItemCatalog& items, const MonsterData* monsters,
                    LootTables& out, std::string& error)
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
    return parseLootTables(ss.str(), core::pathToUtf8(file), items, monsters, out, error);
}

std::optional<std::filesystem::path> locateLootFile()
{
    return core::locateDataFile(std::filesystem::path(kLootFile));
}

LootRoll rollLoot(const LootTable& table, std::mt19937& rng)
{
    LootRoll roll;
    if (table.coinsMax > 0)
        roll.coins = std::uniform_int_distribution<std::uint32_t>(table.coinsMin, table.coinsMax)(rng);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (const LootDrop& d : table.drops)
    {
        // chance 1 always drops (unit() < 1 always holds).
        if (unit(rng) >= d.chance)
            continue;
        const auto n = static_cast<std::uint16_t>(
            d.minCount == d.maxCount ? d.minCount
                                     : std::uniform_int_distribution<std::uint32_t>(d.minCount, d.maxCount)(rng));
        if (roll.items.size() < kMaxLootEntries) // M15: tables may hold more lines than a container
            roll.items.push_back({d.item, n});
    }
    return roll;
}

void forceUniqueDrop(const LootTable& table, LootRoll& roll, std::mt19937& rng)
{
    std::vector<const LootDrop*> uniques;
    for (const LootDrop& d : table.drops)
    {
        if (!d.unique)
            continue;
        if (std::any_of(roll.items.begin(), roll.items.end(), [&](const LootItem& i) { return i.templateId == d.item; }))
            return; // already one
        uniques.push_back(&d);
    }
    if (uniques.empty())
        return;
    const LootDrop& pick = *uniques[std::uniform_int_distribution<std::size_t>(0, uniques.size() - 1)(rng)];
    if (roll.items.size() >= kMaxLootEntries)
        roll.items.pop_back();
    roll.items.push_back({pick.item, pick.minCount});
}

} // namespace vo::game
