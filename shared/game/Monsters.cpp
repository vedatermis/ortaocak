#include "shared/game/Monsters.h"

#include "shared/core/Paths.h"
#include "shared/game/Combat.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
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
                                   std::int64_t min, std::int64_t max, std::optional<std::int64_t> fallback = {})
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        if (fallback)
            return fallback;
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

// A number (integer or float), finite and within [min, max].
std::optional<float> getNumber(const toml::table& t, std::string_view key, std::string_view what, Problems& p,
                               double min, double max, std::optional<float> fallback = {})
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        if (fallback)
            return fallback;
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

std::optional<bool> getBool(const toml::table& t, std::string_view key, std::string_view what, Problems& p, bool fallback)
{
    const toml::node* n = t.get(key);
    if (!n)
        return fallback;
    const auto* b = n->as_boolean();
    if (!b)
    {
        p.add(n, std::format("{}: '{}' must be true or false", what, key));
        return std::nullopt;
    }
    return b->get();
}

constexpr double kMaxCoordinate = 65536.0;

bool parseMonster(const toml::table& t, MonsterTemplate& out, Problems& p)
{
    std::string what = "[[monster]]";
    if (const auto id = getInt(t, "id", what, p, 1, 65535))
        out.id = static_cast<MonsterTemplateId>(*id);
    else
        return false;
    what = std::format("monster {}", out.id);
    if (!onlyKeys(t,
                  {"id", "name", "model", "scale", "level", "hp", "attack", "defense", "attack_range",
                   "attack_interval_ms", "move_speed", "wander_speed", "aggro_radius", "leash_radius", "xp",
                   "respawn_ms", "elite"},
                  what, p))
        return false;
    const auto name = getString(t, "name", what, p);
    const auto model = getString(t, "model", what, p);
    if (!name || !model)
        return false;
    out.name = *name;
    out.model = *model;
    const auto scale = getNumber(t, "scale", what, p, 0.25, 4.0, 1.0f);
    const auto level = getInt(t, "level", what, p, 1, kLevelCap);
    const auto hp = getInt(t, "hp", what, p, 1, 1'000'000);
    const auto attack = getInt(t, "attack", what, p, 0, 100'000);
    const auto defense = getInt(t, "defense", what, p, 0, 100'000);
    const auto range = getNumber(t, "attack_range", what, p, 0.5, 30.0);
    const auto interval = getInt(t, "attack_interval_ms", what, p, 200, 60'000);
    const auto speed = getNumber(t, "move_speed", what, p, 0.1, 20.0);
    const auto wander = getNumber(t, "wander_speed", what, p, 0.1, 20.0);
    const auto aggro = getNumber(t, "aggro_radius", what, p, 0.0, 60.0);
    const auto leash = getNumber(t, "leash_radius", what, p, 1.0, 200.0);
    const auto xp = getInt(t, "xp", what, p, 0, 1'000'000);
    const auto respawn = getInt(t, "respawn_ms", what, p, 0, 24 * 3600 * 1000);
    const auto elite = getBool(t, "elite", what, p, false);
    if (!scale || !level || !hp || !attack || !defense || !range || !interval || !speed || !wander || !aggro ||
        !leash || !xp || !respawn || !elite)
        return false;
    out.scale = *scale;
    out.level = static_cast<std::uint16_t>(*level);
    out.hp = static_cast<std::uint32_t>(*hp);
    out.attack = static_cast<std::uint32_t>(*attack);
    out.defense = static_cast<std::uint32_t>(*defense);
    out.attackRange = *range;
    out.attackIntervalMs = static_cast<std::uint32_t>(*interval);
    out.moveSpeed = *speed;
    out.wanderSpeed = *wander;
    out.aggroRadius = *aggro;
    out.leashRadius = *leash;
    out.xp = static_cast<std::uint32_t>(*xp);
    out.respawnMs = static_cast<std::uint32_t>(*respawn);
    out.elite = *elite;
    if (out.leashRadius <= out.aggroRadius)
    {
        p.add(t.get("leash_radius"), std::format("{}: leash_radius must be larger than aggro_radius", what));
        return false;
    }
    const auto corpse = static_cast<std::uint32_t>(kMonsterCorpseTime.count());
    if (out.respawnMs < corpse)
    {
        p.add(t.get("respawn_ms"), std::format("{}: respawn_ms must be at least the {} ms a corpse stays", what, corpse));
        return false;
    }
    return true;
}

bool parseSafeZone(const toml::table& t, SafeZone& out, Problems& p)
{
    const std::string what = "[[safe_zone]]";
    if (!onlyKeys(t, {"zone", "x", "z", "radius"}, what, p))
        return false;
    const auto zone = getInt(t, "zone", what, p, 1, 65535);
    const auto x = getNumber(t, "x", what, p, 0.0, kMaxCoordinate);
    const auto z = getNumber(t, "z", what, p, 0.0, kMaxCoordinate);
    const auto r = getNumber(t, "radius", what, p, 1.0, 10'000.0);
    if (!zone || !x || !z || !r)
        return false;
    out = {static_cast<std::uint16_t>(*zone), *x, *z, *r};
    return true;
}

bool parseSpawn(const toml::table& t, const MonsterData& data, MonsterSpawn& out, Problems& p)
{
    const std::string what = "[[spawn]]";
    if (!onlyKeys(t, {"zone", "monster", "x", "z", "radius", "count"}, what, p))
        return false;
    const auto zone = getInt(t, "zone", what, p, 1, 65535);
    const auto monster = getInt(t, "monster", what, p, 1, 65535);
    const auto x = getNumber(t, "x", what, p, 0.0, kMaxCoordinate);
    const auto z = getNumber(t, "z", what, p, 0.0, kMaxCoordinate);
    const auto r = getNumber(t, "radius", what, p, 0.0, 100.0);
    const auto count = getInt(t, "count", what, p, 1, 64, 1);
    if (!zone || !monster || !x || !z || !r || !count)
        return false;
    out = {static_cast<std::uint16_t>(*zone), static_cast<MonsterTemplateId>(*monster), *x, *z, *r,
           static_cast<std::uint16_t>(*count)};
    if (!data.find(out.monster))
    {
        p.add(t.get("monster"), std::format("{}: unknown monster id {}", what, out.monster));
        return false;
    }
    return true;
}

} // namespace

const MonsterTemplate* MonsterData::find(MonsterTemplateId id) const noexcept
{
    if (id == 0)
        return nullptr;
    const auto it = std::lower_bound(m_templates.begin(), m_templates.end(), id,
                                     [](const MonsterTemplate& t, MonsterTemplateId v) { return t.id < v; });
    return it != m_templates.end() && it->id == id ? &*it : nullptr;
}

bool MonsterData::inSafeZone(std::uint16_t zone, float x, float z) const noexcept
{
    for (const SafeZone& s : m_safeZones)
    {
        if (s.zone == zone && std::hypot(x - s.x, z - s.z) <= s.radius)
            return true;
    }
    return false;
}

bool parseMonsterData(std::string_view tomlText, std::string_view sourceName, MonsterData& out, std::string& error)
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
    MonsterData data;
    if (!onlyKeys(root, {"monster", "safe_zone", "spawn"}, "top level", p))
    {
        error = p.error();
        return false;
    }
    const auto arrayOf = [&](std::string_view key, bool required) -> const toml::array* {
        const toml::node* n = root.get(key);
        if (!n)
        {
            if (required)
                p.add(nullptr, std::format("no [[{}]] entries", key));
            return nullptr;
        }
        const auto* a = n->as_array();
        if (!a || (required && a->empty()))
        {
            p.add(n, std::format("'{}' must be a non-empty array of tables ([[{}]])", key, key));
            return nullptr;
        }
        return a;
    };

    if (const toml::array* list = arrayOf("monster", true))
    {
        for (const auto& node : *list)
        {
            const auto* t = node.as_table();
            if (!t)
            {
                p.add(&node, "'monster' must be an array of tables ([[monster]])");
                break;
            }
            MonsterTemplate m;
            if (!parseMonster(*t, m, p))
                break;
            if (std::any_of(data.m_templates.begin(), data.m_templates.end(),
                            [&](const MonsterTemplate& o) { return o.id == m.id; }))
            {
                p.add(t, std::format("duplicate monster id {}", m.id));
                break;
            }
            data.m_templates.push_back(std::move(m));
        }
    }
    std::sort(data.m_templates.begin(), data.m_templates.end(),
              [](const MonsterTemplate& a, const MonsterTemplate& b) { return a.id < b.id; });

    if (!p.any())
    {
        if (const toml::array* list = arrayOf("safe_zone", false))
        {
            for (const auto& node : *list)
            {
                const auto* t = node.as_table();
                SafeZone s;
                if (!t)
                {
                    p.add(&node, "'safe_zone' must be an array of tables ([[safe_zone]])");
                    break;
                }
                if (!parseSafeZone(*t, s, p))
                    break;
                data.m_safeZones.push_back(s);
            }
        }
    }
    if (!p.any())
    {
        if (const toml::array* list = arrayOf("spawn", false))
        {
            for (const auto& node : *list)
            {
                const auto* t = node.as_table();
                MonsterSpawn s;
                if (!t)
                {
                    p.add(&node, "'spawn' must be an array of tables ([[spawn]])");
                    break;
                }
                if (!parseSpawn(*t, data, s, p))
                    break;
                // Monsters never enter a safe zone, so no part of a wander circle may lie in one.
                for (const SafeZone& z : data.m_safeZones)
                {
                    if (z.zone == s.zone && std::hypot(s.x - z.x, s.z - z.z) <= z.radius + s.radius)
                    {
                        p.add(t, std::format("[[spawn]] at ({:.1f}, {:.1f}) radius {:.1f} reaches into the safe zone "
                                             "at ({:.1f}, {:.1f}) radius {:.1f}",
                                             s.x, s.z, s.radius, z.x, z.z, z.radius));
                        break;
                    }
                }
                if (p.any())
                    break;
                data.m_spawns.push_back(s);
            }
        }
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    out = std::move(data);
    return true;
}

bool loadMonsterData(const std::filesystem::path& file, MonsterData& out, std::string& error)
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
    return parseMonsterData(ss.str(), core::pathToUtf8(file), out, error);
}

std::optional<std::filesystem::path> locateMonstersFile()
{
    return core::locateDataFile(std::filesystem::path(kMonstersFile));
}

} // namespace vo::game
