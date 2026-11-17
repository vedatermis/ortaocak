#include "shared/game/ZoneData.h"

#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/game/Character.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>

namespace vo::game {

namespace {

// Collects the first validation error with "<source>:<line>: " in front (as Skills.cpp).
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

std::optional<std::string> getString(const toml::table& t, std::string_view key, std::string_view what, Problems& p)
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

std::optional<std::uint8_t> parseRace(std::string_view s) noexcept
{
    if (s == "aydor")
        return static_cast<std::uint8_t>(Race::Aydor);
    if (s == "kargan")
        return static_cast<std::uint8_t>(Race::Kargan);
    return std::nullopt;
}

bool validKey(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 32)
        return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// A marker name: [A-Za-z0-9_]{1,32}.
bool validMarker(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 32)
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// Relative, forward slashes, no "..", ends in .vterrain.
bool validTerrainPath(std::string_view s) noexcept
{
    if (s.size() < 10 || s.size() > 200 || s.front() == '/' || s.find('\\') != std::string_view::npos ||
        s.find(':') != std::string_view::npos || s.find("..") != std::string_view::npos)
        return false;
    return s.ends_with(".vterrain");
}

bool parseGate(const toml::table& t, GateDef& out, Problems& p)
{
    const std::string what = "[[zone.gate]]";
    if (!onlyKeys(t, {"marker", "radius", "to_zone", "arrive"}, what, p))
        return false;
    const auto marker = getString(t, "marker", what, p);
    const auto radius = getNumber(t, "radius", what, p, kMinGateRadius, kMaxGateRadius);
    const auto toZone = getInt(t, "to_zone", what, p, 1, 65535);
    const auto arrive = getString(t, "arrive", what, p);
    if (!marker || !radius || !toZone || !arrive)
        return false;
    if (!validMarker(*marker))
    {
        p.add(t.get("marker"), std::format("{}: 'marker' = '{}' is not a marker name ([A-Za-z0-9_], 1..32)", what, *marker));
        return false;
    }
    if (!validMarker(*arrive))
    {
        p.add(t.get("arrive"), std::format("{}: 'arrive' = '{}' is not a marker name ([A-Za-z0-9_], 1..32)", what, *arrive));
        return false;
    }
    out = {*marker, *radius, static_cast<std::uint16_t>(*toZone), *arrive};
    return true;
}

bool parseZone(const toml::table& t, ZoneDef& out, Problems& p)
{
    const std::string what = "[[zone]]";
    if (!onlyKeys(t, {"id", "key", "name", "tagline", "terrain", "races", "gate"}, what, p))
        return false;
    const auto id = getInt(t, "id", what, p, 1, 65535);
    const auto key = getString(t, "key", what, p);
    const auto name = getString(t, "name", what, p);
    const auto tagline = getString(t, "tagline", what, p);
    const auto terrain = getString(t, "terrain", what, p);
    if (!id || !key || !name || !tagline || !terrain)
        return false;
    const std::string where = std::format("[[zone]] {}", *id);
    if (!validKey(*key))
    {
        p.add(t.get("key"), std::format("{}: 'key' = '{}' must be [a-z0-9_], 1..32 characters", where, *key));
        return false;
    }
    if (name->size() > 48 || tagline->size() > 96)
    {
        p.add(t.get(name->size() > 48 ? "name" : "tagline"),
              std::format("{}: 'name' (≤ 48 bytes) or 'tagline' (≤ 96 bytes) too long", where));
        return false;
    }
    if (!validTerrainPath(*terrain))
    {
        p.add(t.get("terrain"),
              std::format("{}: 'terrain' = '{}' must be a relative path with '/' ending in .vterrain", where, *terrain));
        return false;
    }
    std::uint8_t mask = 0;
    {
        const toml::node* n = t.get("races");
        const toml::array* list = n ? n->as_array() : nullptr;
        if (!n)
        {
            p.add(&t, std::format("{}: missing 'races'", where));
            return false;
        }
        if (!list || list->empty())
        {
            p.add(n, std::format("{}: 'races' must be a non-empty array of race names", where));
            return false;
        }
        for (const toml::node& r : *list)
        {
            const auto* s = r.as_string();
            const auto race = s ? parseRace(s->get()) : std::nullopt;
            if (!race)
            {
                p.add(&r, std::format("{}: unknown race in 'races' (\"aydor\" or \"kargan\")", where));
                return false;
            }
            if ((mask & raceBit(*race)) != 0)
            {
                p.add(&r, std::format("{}: race '{}' listed twice", where, s->get()));
                return false;
            }
            mask |= raceBit(*race);
        }
    }
    ZoneDef z;
    z.id = static_cast<std::uint16_t>(*id);
    z.key = *key;
    z.name = *name;
    z.tagline = *tagline;
    z.terrain = *terrain;
    z.raceMask = mask;
    if (const toml::node* g = t.get("gate"))
    {
        const toml::array* list = g->as_array();
        if (!list || !list->is_array_of_tables())
        {
            p.add(g, std::format("{}: 'gate' must be an array of tables ([[zone.gate]])", where));
            return false;
        }
        if (list->size() > kMaxGatesPerZone)
        {
            p.add(g, std::format("{}: more than {} gates", where, kMaxGatesPerZone));
            return false;
        }
        for (const toml::node& node : *list)
        {
            GateDef gate;
            if (!parseGate(*node.as_table(), gate, p))
                return false;
            if (gate.toZone == z.id)
            {
                p.add(&node, std::format("{}: gate '{}' leads to its own zone", where, gate.marker));
                return false;
            }
            for (const GateDef& other : z.gates)
            {
                if (other.marker == gate.marker)
                {
                    p.add(&node, std::format("{}: gate marker '{}' used twice", where, gate.marker));
                    return false;
                }
            }
            z.gates.push_back(std::move(gate));
        }
    }
    out = std::move(z);
    return true;
}

// ---- Process-wide catalog ----

struct Global
{
    std::mutex mutex;
    std::vector<std::shared_ptr<const ZoneCatalog>> installed; // kept alive forever
    std::atomic<const ZoneCatalog*> current{nullptr};
};

Global& global()
{
    static Global g;
    return g;
}

const ZoneCatalog& defaultCatalog()
{
    static const ZoneCatalog catalog = [] {
        ZoneCatalog c;
        std::string error;
        if (const auto file = locateZonesFile())
        {
            if (!loadZoneCatalog(*file, c, error))
                c = {};
        }
        else
            error = std::format("{} not found (searched upwards from the working directory and the executable directory)",
                                kZonesFile);
        if (!error.empty())
            VO_LOG_ERROR("zones: {}", error);
        return c;
    }();
    return catalog;
}

} // namespace

std::string_view raceSpawnMarker(std::uint8_t race) noexcept
{
    if (race == static_cast<std::uint8_t>(Race::Aydor))
        return "spawn_aydor";
    if (race == static_cast<std::uint8_t>(Race::Kargan))
        return "spawn_kargan";
    return {};
}

const ZoneDef* ZoneCatalog::find(std::uint16_t id) const noexcept
{
    const auto it = std::lower_bound(m_zones.begin(), m_zones.end(), id,
                                     [](const ZoneDef& z, std::uint16_t v) { return z.id < v; });
    return it != m_zones.end() && it->id == id ? &*it : nullptr;
}

const ZoneDef* ZoneCatalog::findKey(std::string_view key) const noexcept
{
    for (const ZoneDef& z : m_zones)
        if (z.key == key)
            return &z;
    return nullptr;
}

const ZoneDef* ZoneCatalog::findByIdOrKey(std::string_view text) const noexcept
{
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec == std::errc{} && ptr == text.data() + text.size() && value <= 65535)
        return find(static_cast<std::uint16_t>(value));
    return findKey(text);
}

std::uint16_t ZoneCatalog::homeZone(std::uint8_t race) const noexcept
{
    if (race == static_cast<std::uint8_t>(Race::Aydor))
        return m_homeAydor;
    if (race == static_cast<std::uint8_t>(Race::Kargan))
        return m_homeKargan;
    return 0;
}

ZoneCatalog ZoneCatalog::singleZone(std::uint16_t id, std::string terrain)
{
    ZoneCatalog c;
    ZoneDef z;
    z.id = id;
    z.key = std::format("zone{}", id);
    z.name = std::format("Bölge {}", id);
    z.tagline = "";
    z.terrain = std::move(terrain);
    z.raceMask = kAllRaces;
    c.m_zones.push_back(std::move(z));
    c.m_homeAydor = id;
    c.m_homeKargan = id;
    return c;
}

bool parseZoneCatalog(std::string_view tomlText, std::string_view sourceName, ZoneCatalog& out, std::string& error)
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
    ZoneCatalog c;
    if (!onlyKeys(root, {"start", "zone"}, "top level", p))
    {
        error = p.error();
        return false;
    }
    const toml::node* zonesNode = root.get("zone");
    const toml::array* list = zonesNode ? zonesNode->as_array() : nullptr;
    if (!zonesNode)
        p.add(nullptr, "no [[zone]] entries");
    else if (!list || !list->is_array_of_tables())
        p.add(zonesNode, "'zone' must be an array of tables ([[zone]])");
    else if (list->empty())
        p.add(zonesNode, "no [[zone]] entries");
    else if (list->size() > kMaxZones)
        p.add(zonesNode, std::format("more than {} zones", kMaxZones));
    std::vector<const toml::node*> zoneNodes;
    if (!p.any())
    {
        for (const toml::node& node : *list)
        {
            ZoneDef z;
            if (!parseZone(*node.as_table(), z, p))
                break;
            for (const ZoneDef& other : c.m_zones)
            {
                if (other.id == z.id)
                    p.add(&node, std::format("[[zone]]: id {} used twice", z.id));
                else if (other.key == z.key)
                    p.add(&node, std::format("[[zone]]: key '{}' used twice", z.key));
                else if (other.terrain == z.terrain)
                    p.add(&node, std::format("[[zone]]: terrain '{}' used twice", z.terrain));
            }
            if (p.any())
                break;
            c.m_zones.push_back(std::move(z));
            zoneNodes.push_back(&node);
        }
    }
    if (!p.any())
    {
        // Gates: the target exists and somebody allowed here may pass.
        for (std::size_t i = 0; i < c.m_zones.size() && !p.any(); ++i)
        {
            const ZoneDef& z = c.m_zones[i];
            for (const GateDef& g : z.gates)
            {
                const auto target = std::find_if(c.m_zones.begin(), c.m_zones.end(),
                                                 [&](const ZoneDef& o) { return o.id == g.toZone; });
                if (target == c.m_zones.end())
                {
                    p.add(zoneNodes[i], std::format("[[zone]] {}: gate '{}' leads to unknown zone {}", z.id, g.marker, g.toZone));
                    break;
                }
                if ((target->raceMask & z.raceMask) == 0)
                {
                    p.add(zoneNodes[i], std::format("[[zone]] {}: nobody allowed here may pass gate '{}' (zone {} allows "
                                                    "no race of this zone)",
                                                    z.id, g.marker, g.toZone));
                    break;
                }
            }
        }
    }
    if (!p.any())
    {
        const toml::node* startNode = root.get("start");
        const toml::table* start = startNode ? startNode->as_table() : nullptr;
        if (!startNode)
            p.add(nullptr, "missing [start]");
        else if (!start)
            p.add(startNode, "'start' must be a table ([start])");
        else if (onlyKeys(*start, {"aydor", "kargan"}, "[start]", p))
        {
            const auto aydor = getInt(*start, "aydor", "[start]", p, 1, 65535);
            const auto kargan = getInt(*start, "kargan", "[start]", p, 1, 65535);
            const auto check = [&](std::optional<std::int64_t> id, std::string_view key, Race race) -> std::uint16_t {
                if (!id || p.any())
                    return 0;
                const auto zid = static_cast<std::uint16_t>(*id);
                const auto it = std::find_if(c.m_zones.begin(), c.m_zones.end(),
                                             [&](const ZoneDef& z) { return z.id == zid; });
                if (it == c.m_zones.end())
                    p.add(start->get(key), std::format("[start]: '{}' = {} is not a listed zone", key, zid));
                else if (!it->allows(static_cast<std::uint8_t>(race)))
                    p.add(start->get(key), std::format("[start]: zone {} does not allow race '{}'", zid, key));
                return zid;
            };
            c.m_homeAydor = check(aydor, "aydor", Race::Aydor);
            c.m_homeKargan = check(kargan, "kargan", Race::Kargan);
        }
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    std::sort(c.m_zones.begin(), c.m_zones.end(), [](const ZoneDef& a, const ZoneDef& b) { return a.id < b.id; });
    out = std::move(c);
    return true;
}

bool loadZoneCatalog(const std::filesystem::path& file, ZoneCatalog& out, std::string& error)
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
    return parseZoneCatalog(ss.str(), core::pathToUtf8(file), out, error);
}

std::optional<std::filesystem::path> locateZonesFile()
{
    return core::locateDataFile(std::filesystem::path(kZonesFile));
}

const ZoneCatalog& zones() noexcept
{
    if (const ZoneCatalog* c = global().current.load(std::memory_order_acquire))
        return *c;
    return defaultCatalog();
}

void installZoneCatalog(std::shared_ptr<const ZoneCatalog> catalog)
{
    if (!catalog)
        return;
    Global& g = global();
    const std::scoped_lock lock(g.mutex);
    g.installed.push_back(catalog);
    g.current.store(catalog.get(), std::memory_order_release);
}

GateCheck checkGate(std::uint8_t targetRaceMask, std::uint8_t race, const GateState& state) noexcept
{
    if (state.loading)
        return GateCheck::Loading;
    if (!state.alive)
        return GateCheck::Dead;
    if (!state.armedByTime || !state.beenOutside)
        return GateCheck::Disarmed;
    const std::uint8_t bit = raceBit(race);
    if (bit == 0 || (targetRaceMask & bit) == 0)
        return GateCheck::RaceForbidden;
    return GateCheck::Pass;
}

GateCheck checkGate(const ZoneCatalog& catalog, const GateDef& gate, std::uint8_t race, const GateState& state) noexcept
{
    const ZoneDef* target = catalog.find(gate.toZone);
    return checkGate(target ? target->raceMask : std::uint8_t{0}, race, state);
}

} // namespace vo::game
