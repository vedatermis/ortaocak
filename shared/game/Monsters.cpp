#include "shared/game/Monsters.h"

#include "shared/assets/Terrain.h"
#include "shared/core/Paths.h"
#include "shared/game/Combat.h"
#include "shared/game/TerrainWalk.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <numbers>
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

bool validSlotKey(std::string_view key)
{
    return !key.empty() && key.size() <= 32 &&
           std::all_of(key.begin(), key.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// [[slot]] (docs/PROTOCOL_LEVELS.md §3): key, name, zone, x, z, monsters, count, ring, centre,
// start_angle, wander, respawn_ms, leash, homes (explicit offsets instead of the ring).
bool parseSlot(const toml::table& t, const MonsterData& data, MonsterSlot& out, Problems& p)
{
    std::string what = "[[slot]]";
    if (!onlyKeys(t,
                  {"key", "name", "zone", "x", "z", "monsters", "count", "ring", "centre", "start_angle", "wander",
                   "respawn_ms", "leash", "homes"},
                  what, p))
        return false;
    const auto key = getString(t, "key", what, p);
    if (!key)
        return false;
    if (!validSlotKey(*key))
    {
        p.add(t.get("key"), std::format("{}: key '{}' must be 1..32 of [a-z0-9_]", what, *key));
        return false;
    }
    out.key = *key;
    what = std::format("slot '{}'", out.key);
    const auto name = getString(t, "name", what, p);
    const auto zone = getInt(t, "zone", what, p, 1, 65535);
    const auto x = getNumber(t, "x", what, p, 0.0, kMaxCoordinate);
    const auto z = getNumber(t, "z", what, p, 0.0, kMaxCoordinate);
    const auto count = getInt(t, "count", what, p, 1, static_cast<std::int64_t>(kMaxSlotMembers));
    const auto ring = getNumber(t, "ring", what, p, 0.0, kMaxSlotRadius, 0.0f);
    const auto startAngle = getNumber(t, "start_angle", what, p, -360.0, 360.0, 0.0f);
    const auto wander = getNumber(t, "wander", what, p, 0.0, 5.0, 1.0f);
    const auto respawn = getInt(t, "respawn_ms", what, p, 0, 24 * 3600 * 1000, 0);
    const auto leash = getNumber(t, "leash", what, p, 0.0, 200.0, 0.0f);
    if (!name || !zone || !x || !z || !count || !ring || !startAngle || !wander || !respawn || !leash)
        return false;
    if (name->size() > 48)
    {
        p.add(t.get("name"), std::format("{}: name is longer than 48 bytes", what));
        return false;
    }
    out.name = *name;
    out.zone = static_cast<std::uint16_t>(*zone);
    out.x = *x;
    out.z = *z;
    out.wander = *wander;
    out.respawnMs = static_cast<std::uint32_t>(*respawn);
    out.leash = *leash;

    // monsters: one kind, at least one template.
    const toml::node* mn = t.get("monsters");
    const auto* ma = mn ? mn->as_array() : nullptr;
    if (!ma || ma->empty() || ma->size() > 4)
    {
        p.add(mn ? mn : &t, std::format("{}: 'monsters' must be an array of 1..4 monster ids", what));
        return false;
    }
    const MonsterTemplate* first = nullptr;
    for (const auto& node : *ma)
    {
        const auto* i = node.as_integer();
        const MonsterTemplate* m = i && i->get() > 0 && i->get() <= 65535 ? data.find(static_cast<MonsterTemplateId>(i->get())) : nullptr;
        if (!m)
        {
            p.add(&node, std::format("{}: unknown monster id in 'monsters'", what));
            return false;
        }
        if (first && (m->name != first->name || m->model != first->model))
        {
            p.add(&node, std::format("{}: monster {} is another kind than {} (a slot holds one kind: same name and model)",
                                     what, m->id, first->id));
            return false;
        }
        if (std::find(out.monsters.begin(), out.monsters.end(), m->id) != out.monsters.end())
        {
            p.add(&node, std::format("{}: monster {} named twice", what, m->id));
            return false;
        }
        if (!first)
        {
            first = m;
            out.minLevel = out.maxLevel = m->level;
        }
        out.minLevel = std::min(out.minLevel, m->level);
        out.maxLevel = std::max(out.maxLevel, m->level);
        out.aggro = std::max(out.aggro, m->aggroRadius);
        out.elite = out.elite || m->elite;
        out.monsters.push_back(m->id);
    }
    const auto n = static_cast<std::size_t>(*count);
    if (out.elite && n != 1)
    {
        p.add(t.get("count"), std::format("{}: an elite stands alone (count = 1)", what));
        return false;
    }
    for (const MonsterTemplateId id : out.monsters)
    {
        const MonsterTemplate* m = data.find(id);
        if (out.leash > 0.0f && out.leash <= m->aggroRadius)
        {
            p.add(t.get("leash"), std::format("{}: leash must be larger than the aggro radius of monster {}", what, id));
            return false;
        }
    }
    const auto corpse = static_cast<std::uint32_t>(kMonsterCorpseTime.count());
    if (out.respawnMs != 0 && out.respawnMs < corpse)
    {
        p.add(t.get("respawn_ms"), std::format("{}: respawn_ms must be at least the {} ms a corpse stays", what, corpse));
        return false;
    }

    // Home points: explicit offsets, or a ring (with one member in the centre when `centre`, by
    // default for 7 or more members), the first ring member at start_angle (degrees, 0 = +z, 90 = +x).
    std::vector<std::pair<float, float>> offsets;
    if (const toml::node* hn = t.get("homes"))
    {
        const auto* ha = hn->as_array();
        if (!ha || ha->size() != n || t.get("ring") || t.get("centre") || t.get("start_angle"))
        {
            p.add(hn, std::format("{}: 'homes' must hold exactly 'count' [dx, dz] offsets (and no ring / centre / "
                                  "start_angle)", what));
            return false;
        }
        for (const auto& node : *ha)
        {
            const auto* pair = node.as_array();
            const auto num = [](const toml::node* v) -> std::optional<double> {
                if (!v)
                    return std::nullopt;
                if (const auto* f = v->as_floating_point())
                    return f->get();
                if (const auto* i = v->as_integer())
                    return static_cast<double>(i->get());
                return std::nullopt;
            };
            const auto dx = pair && pair->size() == 2 ? num(pair->get(0)) : std::nullopt;
            const auto dz = pair && pair->size() == 2 ? num(pair->get(1)) : std::nullopt;
            if (!dx || !dz || !std::isfinite(*dx) || !std::isfinite(*dz) || std::hypot(*dx, *dz) > kMaxSlotRadius)
            {
                p.add(&node, std::format("{}: a home offset must be [dx, dz] within {} m", what, kMaxSlotRadius));
                return false;
            }
            offsets.emplace_back(static_cast<float>(*dx), static_cast<float>(*dz));
        }
    }
    else
    {
        const auto centre = getBool(t, "centre", what, p, n >= 7);
        if (!centre)
            return false;
        if (n > 1 && *ring < kMinSlotHomeSpacing)
        {
            p.add(t.get("ring") ? t.get("ring") : &t, std::format("{}: 'ring' must be at least {} m for {} members", what,
                                                                   kMinSlotHomeSpacing, n));
            return false;
        }
        const std::size_t onRing = *centre ? n - 1 : n;
        if (*centre)
            offsets.emplace_back(0.0f, 0.0f);
        for (std::size_t k = 0; k < onRing; ++k)
        {
            const double a = (static_cast<double>(*startAngle) + 360.0 * static_cast<double>(k) / static_cast<double>(onRing)) *
                             std::numbers::pi / 180.0;
            offsets.emplace_back(static_cast<float>(*ring * std::sin(a)), static_cast<float>(*ring * std::cos(a)));
        }
    }
    float farthest = 0.0f;
    for (std::size_t k = 0; k < offsets.size(); ++k)
    {
        const MonsterTemplateId id = out.monsters[k % out.monsters.size()];
        const float hx = out.x + offsets[k].first, hz = out.z + offsets[k].second;
        if (hx < 0.0f || hz < 0.0f || hx > kMaxCoordinate || hz > kMaxCoordinate)
        {
            p.add(&t, std::format("{}: a home lies outside the map", what));
            return false;
        }
        for (const SlotHome& h : out.homes)
            if (std::hypot(h.x - hx, h.z - hz) < kMinSlotHomeSpacing - 1e-3f)
            {
                p.add(&t, std::format("{}: two homes closer than {} m", what, kMinSlotHomeSpacing));
                return false;
            }
        out.homes.push_back({hx, hz, id});
        farthest = std::max(farthest, std::hypot(offsets[k].first, offsets[k].second));
    }
    out.radius = farthest + out.wander;
    if (out.radius > kMaxSlotRadius + 1e-3f)
    {
        p.add(&t, std::format("{}: the area ({:.1f} m: farthest home + wander) is larger than {} m", what, out.radius,
                              kMaxSlotRadius));
        return false;
    }
    return true;
}

} // namespace

const MonsterSlot* MonsterData::slotOf(const MonsterSpawn& s) const noexcept
{
    return s.slot >= 1 && s.slot <= m_slots.size() ? &m_slots[s.slot - 1u] : nullptr;
}

const MonsterSlot* MonsterData::findSlot(std::string_view key) const noexcept
{
    for (const MonsterSlot& s : m_slots)
        if (s.key == key)
            return &s;
    return nullptr;
}

const MonsterSlot* MonsterData::slotAt(std::uint16_t zone, float x, float z, float margin) const noexcept
{
    const MonsterSlot* best = nullptr;
    float bestD = 0.0f;
    for (const MonsterSlot& s : m_slots)
    {
        if (s.zone != zone)
            continue;
        const float d = std::hypot(x - s.x, z - s.z);
        if (d <= s.radius + margin && (!best || d < bestD))
        {
            best = &s;
            bestD = d;
        }
    }
    return best;
}

bool checkSlotTerrain(const assets::TerrainData& terrain, const MonsterSlot& slot, const std::vector<SlotKeepOut>& keepOut,
                      const std::vector<std::uint8_t>& reachable, std::string& why)
{
    if (!terrain.isWalkable(slot.x, slot.z))
    {
        why = std::format("slot '{}': its centre ({:.1f}, {:.1f}) is not walkable", slot.key, slot.x, slot.z);
        return false;
    }
    const int m = static_cast<int>(terrain.maskResolution);
    const float cell = m > 0 ? terrain.worldSize / static_cast<float>(m) : 1.0f;
    for (const SlotHome& h : slot.homes)
    {
        for (int k = -1; k < 8; ++k)
        {
            const double a = k * std::numbers::pi / 4.0;
            const float px = k < 0 ? h.x : h.x + slot.wander * static_cast<float>(std::sin(a));
            const float pz = k < 0 ? h.z : h.z + slot.wander * static_cast<float>(std::cos(a));
            if (!terrain.isWalkable(px, pz))
            {
                why = std::format("slot '{}': ({:.1f}, {:.1f}) of the home ({:.1f}, {:.1f}) and its wander circle is not "
                                  "walkable",
                                  slot.key, px, pz, h.x, h.z);
                return false;
            }
        }
        if (!isSegmentWalkable(terrain, h.x, h.z, slot.x, slot.z))
        {
            why = std::format("slot '{}': no straight walkable line from the home ({:.1f}, {:.1f}) to its centre", slot.key,
                              h.x, h.z);
            return false;
        }
        if (!reachable.empty())
        {
            const int i = std::clamp(static_cast<int>(h.x / cell), 0, m - 1);
            const int j = std::clamp(static_cast<int>(h.z / cell), 0, m - 1);
            if (reachable[static_cast<std::size_t>(j) * static_cast<std::size_t>(m) + static_cast<std::size_t>(i)] == 0)
            {
                why = std::format("slot '{}': the home ({:.1f}, {:.1f}) cannot be reached on foot", slot.key, h.x, h.z);
                return false;
            }
        }
    }
    for (const SlotKeepOut& k : keepOut)
    {
        const float d = std::hypot(slot.x - k.x, slot.z - k.z);
        if (d < k.radius + slot.radius + slot.aggro)
        {
            why = std::format("slot '{}': its area + aggro ({:.1f} m) comes within {:.1f} m of {} (needs {:.1f})", slot.key,
                              slot.radius + slot.aggro, d - slot.radius - slot.aggro, k.name, k.radius);
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> reachableCells(const assets::TerrainData& terrain, float x, float z)
{
    const int m = static_cast<int>(terrain.maskResolution);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(m) * static_cast<std::size_t>(m), 0);
    if (m <= 0 || terrain.walkMask.size() < out.size() || !terrain.isWalkable(x, z))
        return out;
    const float cell = terrain.worldSize / static_cast<float>(m);
    const auto idx = [m](int i, int j) { return static_cast<std::size_t>(j) * static_cast<std::size_t>(m) + static_cast<std::size_t>(i); };
    std::vector<std::pair<int, int>> stack;
    const int i0 = std::clamp(static_cast<int>(x / cell), 0, m - 1), j0 = std::clamp(static_cast<int>(z / cell), 0, m - 1);
    out[idx(i0, j0)] = 1;
    stack.emplace_back(i0, j0);
    while (!stack.empty())
    {
        const auto [i, j] = stack.back();
        stack.pop_back();
        constexpr int kD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : kD)
        {
            const int ni = i + d[0], nj = j + d[1];
            if (ni < 0 || nj < 0 || ni >= m || nj >= m || out[idx(ni, nj)] != 0 || terrain.walkMask[idx(ni, nj)] == 0)
                continue;
            out[idx(ni, nj)] = 1;
            stack.emplace_back(ni, nj);
        }
    }
    return out;
}

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
    if (!onlyKeys(root, {"monster", "safe_zone", "spawn", "slot"}, "top level", p))
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
    // M15: hunting slots; checked against the safe zones, the loose spawns and each other, then their
    // members appended to the spawns.
    if (!p.any())
    {
        if (const toml::array* list = arrayOf("slot", false))
        {
            for (const auto& node : *list)
            {
                const auto* t = node.as_table();
                MonsterSlot s;
                if (!t)
                {
                    p.add(&node, "'slot' must be an array of tables ([[slot]])");
                    break;
                }
                if (!parseSlot(*t, data, s, p))
                    break;
                if (data.findSlot(s.key))
                {
                    p.add(t, std::format("duplicate slot key '{}'", s.key));
                    break;
                }
                for (const SafeZone& z : data.m_safeZones)
                    if (z.zone == s.zone && std::hypot(s.x - z.x, s.z - z.z) <= z.radius + s.radius + s.aggro)
                    {
                        p.add(t, std::format("slot '{}': its area + aggro ({:.1f} m) reaches into the safe zone at "
                                             "({:.1f}, {:.1f}) radius {:.1f}",
                                             s.key, s.radius + s.aggro, z.x, z.z, z.radius));
                        break;
                    }
                for (const MonsterSpawn& o : data.m_spawns)
                {
                    if (p.any())
                        break;
                    const MonsterTemplate* ot = data.find(o.monster);
                    const float gap = std::max(s.aggro, ot ? ot->aggroRadius : 0.0f) + kSlotGap;
                    if (o.zone == s.zone && std::hypot(s.x - o.x, s.z - o.z) < s.radius + o.radius + gap)
                        p.add(t, std::format("slot '{}' lies within {:.1f} m (+ {:.1f} m gap) of the [[spawn]] at "
                                             "({:.1f}, {:.1f})",
                                             s.key, s.radius + o.radius, gap, o.x, o.z));
                }
                for (const MonsterSlot& o : data.m_slots)
                {
                    if (p.any())
                        break;
                    const float gap = std::max(s.aggro, o.aggro) + kSlotGap;
                    const float d = std::hypot(s.x - o.x, s.z - o.z);
                    if (o.zone == s.zone && d < s.radius + o.radius + gap)
                        p.add(t, std::format("slot '{}' is {:.1f} m from slot '{}': their areas ({:.1f} + {:.1f} m) need "
                                             "{:.1f} m between them (larger aggro + {} m)",
                                             s.key, d, o.key, s.radius, o.radius, gap, kSlotGap));
                }
                if (p.any())
                    break;
                data.m_slots.push_back(std::move(s));
            }
        }
        for (std::size_t i = 0; i < data.m_slots.size(); ++i)
            for (const SlotHome& h : data.m_slots[i].homes)
                data.m_spawns.push_back({data.m_slots[i].zone, h.monster, h.x, h.z, data.m_slots[i].wander, 1,
                                         static_cast<std::uint16_t>(i + 1), data.m_slots[i].respawnMs,
                                         data.m_slots[i].leash});
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
