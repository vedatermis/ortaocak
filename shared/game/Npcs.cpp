#include "shared/game/Npcs.h"

#include "shared/assets/Terrain.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <mutex>
#include <numbers>
#include <sstream>

namespace vo::game {

namespace {

// Collects the first validation error with "<source>:<line>: " in front (as ZoneData.cpp).
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
                                     std::size_t maxBytes)
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        p.add(&t, std::format("{}: missing '{}'", what, key));
        return std::nullopt;
    }
    const auto* s = n->as_string();
    if (!s)
    {
        p.add(n, std::format("{}: '{}' must be a string", what, key));
        return std::nullopt;
    }
    if (s->get().empty())
    {
        p.add(n, std::format("{}: '{}' must not be empty", what, key));
        return std::nullopt;
    }
    if (s->get().size() > maxBytes)
    {
        p.add(n, std::format("{}: '{}' is longer than {} bytes", what, key, maxBytes));
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

std::optional<float> getNumber(const toml::table& t, std::string_view key, std::string_view what, Problems& p,
                               double min, double max)
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        p.add(&t, std::format("{}: missing '{}'", what, key));
        return std::nullopt;
    }
    double v = 0.0;
    if (const auto* f = n->as_floating_point())
        v = f->get();
    else if (const auto* i = n->as_integer())
        v = static_cast<double>(i->get());
    else
    {
        p.add(n, std::format("{}: '{}' must be a number", what, key));
        return std::nullopt;
    }
    if (!std::isfinite(v) || v < min || v > max)
    {
        p.add(n, std::format("{}: '{}' = {} is out of range ({}..{})", what, key, v, min, max));
        return std::nullopt;
    }
    return static_cast<float>(v);
}

bool validModelKey(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 48)
        return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool parseNpc(const toml::table& t, const ItemCatalog* items, NpcDef& out, Problems& p)
{
    const std::string what = "[[npc]]";
    if (!onlyKeys(t, {"id", "name", "title", "greeting", "model", "zone", "x", "z", "yaw", "kind", "stock", "upgrades"},
                  what, p))
        return false;
    const auto id = getInt(t, "id", what, p, 1, kMaxNpcId);
    if (!id)
        return false;
    const std::string where = std::format("[[npc]] {}", *id);
    const auto name = getString(t, "name", where, p, 48);
    const auto title = getString(t, "title", where, p, 48);
    const auto greeting = getString(t, "greeting", where, p, 200);
    const auto model = getString(t, "model", where, p, 48);
    const auto zone = getInt(t, "zone", where, p, 1, 65535);
    const auto x = getNumber(t, "x", where, p, 0.0, 4096.0);
    const auto z = getNumber(t, "z", where, p, 0.0, 4096.0);
    const auto yaw = getNumber(t, "yaw", where, p, -2.0 * std::numbers::pi, 2.0 * std::numbers::pi);
    const auto kind = getString(t, "kind", where, p, 32);
    if (!name || !title || !greeting || !model || !zone || !x || !z || !yaw || !kind)
        return false;
    if (!validModelKey(*model))
    {
        p.add(t.get("model"), std::format("{}: 'model' = '{}' must be [a-z0-9_], 1..48 characters", where, *model));
        return false;
    }
    NpcDef n;
    n.id = static_cast<std::uint16_t>(*id);
    n.name = *name;
    n.title = *title;
    n.greeting = *greeting;
    n.model = *model;
    n.zone = static_cast<std::uint16_t>(*zone);
    n.x = *x;
    n.z = *z;
    n.yaw = *yaw;
    if (*kind == "merchant")
        n.kind = NpcKind::Merchant;
    else
    {
        p.add(t.get("kind"), std::format("{}: unknown kind '{}' (\"merchant\")", where, *kind));
        return false;
    }
    const toml::node* stockNode = t.get("stock");
    if (n.kind == NpcKind::Merchant)
    {
        const toml::array* list = stockNode ? stockNode->as_array() : nullptr;
        if (!stockNode)
        {
            p.add(&t, std::format("{}: a merchant needs a 'stock'", where));
            return false;
        }
        if (!list || list->empty())
        {
            p.add(stockNode, std::format("{}: 'stock' must be a non-empty array of item ids", where));
            return false;
        }
        if (list->size() > kMaxStockEntries)
        {
            p.add(stockNode, std::format("{}: more than {} stock entries", where, kMaxStockEntries));
            return false;
        }
        for (const toml::node& e : *list)
        {
            const auto* i = e.as_integer();
            if (!i || i->get() < 1 || i->get() > 65535)
            {
                p.add(&e, std::format("{}: stock entries must be item ids 1..65535", where));
                return false;
            }
            const auto tid = static_cast<ItemTemplateId>(i->get());
            if (std::find(n.stock.begin(), n.stock.end(), tid) != n.stock.end())
            {
                p.add(&e, std::format("{}: item {} is in the stock twice", where, tid));
                return false;
            }
            if (items)
            {
                const ItemTemplate* it = items->find(tid);
                if (!it)
                {
                    p.add(&e, std::format("{}: stock item {} is not in items.toml", where, tid));
                    return false;
                }
                if (it->value == 0)
                {
                    p.add(&e, std::format("{}: stock item {} ({}) has no value, so no price", where, tid, it->name));
                    return false;
                }
            }
            n.stock.push_back(tid);
        }
    }
    // M16 (docs/PROTOCOL_UPGRADE.md §1): a smith also upgrades items.
    if (const toml::node* u = t.get("upgrades"))
    {
        const auto* b = u->as_boolean();
        if (!b)
        {
            p.add(u, std::format("{}: 'upgrades' must be true or false", where));
            return false;
        }
        n.upgrades = b->get();
    }
    out = std::move(n);
    return true;
}

struct Global
{
    std::mutex mutex;
    std::vector<std::shared_ptr<const NpcCatalog>> installed; // kept alive forever
    std::atomic<const NpcCatalog*> current{nullptr};
};

Global& global()
{
    static Global g;
    return g;
}

const NpcCatalog& defaultCatalog()
{
    static const NpcCatalog catalog = [] {
        NpcCatalog c;
        std::string error;
        ItemCatalog items;
        const ItemCatalog* itemsPtr = nullptr;
        if (const auto itemsFile = locateItemsFile(); itemsFile && loadItemCatalog(*itemsFile, items, error))
            itemsPtr = &items;
        error.clear();
        if (const auto file = locateNpcsFile())
        {
            if (!loadNpcCatalog(*file, itemsPtr, c, error))
                c = {};
        }
        else
            error = std::format("{} not found (searched upwards from the working directory and the executable directory)",
                                kNpcsFile);
        if (!error.empty())
            VO_LOG_ERROR("npcs: {}", error);
        return c;
    }();
    return catalog;
}

} // namespace

std::string_view npcKindName(NpcKind k) noexcept
{
    switch (k)
    {
    case NpcKind::Merchant: return "merchant";
    }
    return "?";
}

const NpcDef* NpcCatalog::find(std::uint16_t id) const noexcept
{
    const auto it = std::lower_bound(m_npcs.begin(), m_npcs.end(), id,
                                     [](const NpcDef& n, std::uint16_t v) { return n.id < v; });
    return it != m_npcs.end() && it->id == id ? &*it : nullptr;
}

bool parseNpcCatalog(std::string_view tomlText, std::string_view sourceName, const ItemCatalog* items, NpcCatalog& out,
                     std::string& error)
{
    Problems p(sourceName);
    toml::table root;
    try
    {
        root = toml::parse(tomlText, sourceName);
    }
    catch (const toml::parse_error& e)
    {
        error = std::format("{}:{}: {}", sourceName, e.source().begin.line, e.description());
        return false;
    }
    NpcCatalog c;
    if (!onlyKeys(root, {"npc"}, "top level", p))
    {
        error = p.error();
        return false;
    }
    const toml::node* node = root.get("npc");
    const toml::array* list = node ? node->as_array() : nullptr;
    if (!node)
        p.add(nullptr, "no [[npc]] entries");
    else if (!list || !list->is_array_of_tables())
        p.add(node, "'npc' must be an array of tables ([[npc]])");
    else if (list->empty())
        p.add(node, "no [[npc]] entries");
    else if (list->size() > kMaxNpcs)
        p.add(node, std::format("more than {} NPCs", kMaxNpcs));
    if (!p.any())
    {
        for (const toml::node& n : *list)
        {
            NpcDef def;
            if (!parseNpc(*n.as_table(), items, def, p))
                break;
            for (const NpcDef& other : c.m_npcs)
            {
                if (other.id == def.id)
                    p.add(&n, std::format("[[npc]]: id {} used twice", def.id));
                else if (other.model == def.model)
                    p.add(&n, std::format("[[npc]]: model '{}' used twice", def.model));
            }
            if (p.any())
                break;
            c.m_npcs.push_back(std::move(def));
        }
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    std::sort(c.m_npcs.begin(), c.m_npcs.end(), [](const NpcDef& a, const NpcDef& b) { return a.id < b.id; });
    out = std::move(c);
    return true;
}

bool loadNpcCatalog(const std::filesystem::path& file, const ItemCatalog* items, NpcCatalog& out, std::string& error)
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
    return parseNpcCatalog(ss.str(), core::pathToUtf8(file), items, out, error);
}

std::optional<std::filesystem::path> locateNpcsFile()
{
    return core::locateDataFile(std::filesystem::path(kNpcsFile));
}

const NpcCatalog& npcs() noexcept
{
    if (const NpcCatalog* c = global().current.load(std::memory_order_acquire))
        return *c;
    return defaultCatalog();
}

void installNpcCatalog(std::shared_ptr<const NpcCatalog> catalog)
{
    if (!catalog)
        return;
    Global& g = global();
    const std::scoped_lock lock(g.mutex);
    g.installed.push_back(catalog);
    g.current.store(catalog.get(), std::memory_order_release);
}

float roadWeightAt(const assets::TerrainData& terrain, float x, float z) noexcept
{
    const std::uint32_t s = terrain.splatResolution;
    if (s == 0 || terrain.splat.size() < static_cast<std::size_t>(s) * s * 4 || !(terrain.worldSize > 0.0f))
        return 0.0f;
    const auto texel = [&](float v) {
        const float f = std::floor(v / terrain.worldSize * static_cast<float>(s));
        return static_cast<std::uint32_t>(std::clamp(f, 0.0f, static_cast<float>(s - 1)));
    };
    // The weights are not normalised in the file (the shader divides by their sum): do the same.
    const std::size_t i = (static_cast<std::size_t>(texel(z)) * s + texel(x)) * 4;
    const float sum = static_cast<float>(terrain.splat[i]) + static_cast<float>(terrain.splat[i + 1]) +
                      static_cast<float>(terrain.splat[i + 2]) + static_cast<float>(terrain.splat[i + 3]);
    return sum > 0.0f ? static_cast<float>(terrain.splat[i + 1]) / sum : 0.0f;
}

bool checkNpcSpot(const assets::TerrainData& terrain, float x, float z, std::span<const KeepOut> keepOut,
                  std::string& why)
{
    if (!std::isfinite(x) || !std::isfinite(z) || !terrain.contains(x, z))
    {
        why = std::format("({:.1f}, {:.1f}) is outside the zone", x, z);
        return false;
    }
    // Room for a customer within talk range: a place on the ring with its own clearance (fix1: the
    // merchant itself stands behind its counter, on cells the stall's footprint blocks).
    const auto roomy = [&](float cx, float cz) {
        if (!terrain.isWalkable(cx, cz))
            return false;
        for (int i = 0; i < 8; ++i)
        {
            const float a = static_cast<float>(i) * std::numbers::pi_v<float> / 4.0f;
            if (!terrain.isWalkable(cx + kNpcCustomerClearance * std::sin(a), cz + kNpcCustomerClearance * std::cos(a)))
                return false;
        }
        return true;
    };
    bool customer = false;
    for (int i = 0; i < 16 && !customer; ++i)
    {
        const float a = static_cast<float>(i) * std::numbers::pi_v<float> / 8.0f;
        customer = roomy(x + kNpcCustomerRing * std::sin(a), z + kNpcCustomerRing * std::cos(a));
    }
    if (!customer)
    {
        why = std::format("no room for a customer round ({:.1f}, {:.1f}): no point {:.1f} m from it is walkable with "
                          "{:.1f} m all round",
                          x, z, kNpcCustomerRing, kNpcCustomerClearance);
        return false;
    }
    if (const float road = roadWeightAt(terrain, x, z); road > kNpcMaxRoadWeight)
    {
        why = std::format("({:.1f}, {:.1f}) is on a road (road layer {:.0f} % > {:.0f} %)", x, z, road * 100.0f,
                          kNpcMaxRoadWeight * 100.0f);
        return false;
    }
    for (const KeepOut& k : keepOut)
    {
        const float d = std::hypot(x - k.x, z - k.z);
        if (d <= k.radius)
        {
            why = std::format("({:.1f}, {:.1f}) is {:.1f} m from {} (it must be more than {:.1f} m away)", x, z, d,
                              k.name, k.radius);
            return false;
        }
    }
    return true;
}

} // namespace vo::game
