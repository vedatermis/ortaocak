#include "shared/game/Skills.h"

#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>

namespace vo::game {

namespace {

// Collects the first validation error with "<source>:<line>: " in front (as Monsters.cpp).
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
                                     std::optional<std::string> fallback = {})
{
    const toml::node* n = t.get(key);
    if (!n)
    {
        if (fallback)
            return fallback;
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

std::optional<double> numberOf(const toml::node& n)
{
    if (const auto* f = n.as_floating_point())
        return f->get();
    if (const auto* i = n.as_integer())
        return static_cast<double>(i->get());
    return std::nullopt;
}

// One number for every rank, or an array of exactly kMaxSkillRank numbers; each finite and within
// [min, max] (and an integer if `integer`).
std::optional<std::array<double, kMaxSkillRank>> getRanked(const toml::table& t, std::string_view key,
                                                           std::string_view what, Problems& p, double min, double max,
                                                           std::optional<double> fallback, bool integer = false)
{
    const toml::node* n = t.get(key);
    std::array<double, kMaxSkillRank> out{};
    if (!n)
    {
        if (!fallback)
        {
            p.add(&t, std::format("{}: missing '{}'", what, key));
            return std::nullopt;
        }
        out.fill(*fallback);
        return out;
    }
    const auto check = [&](const toml::node& v, double& into) {
        const auto x = numberOf(v);
        if (!x || (integer && !v.is_integer()))
        {
            p.add(&v, std::format("{}: '{}' must be {} (or an array of {})", what, key,
                                  integer ? "an integer" : "a number", kMaxSkillRank));
            return false;
        }
        if (!std::isfinite(*x) || *x < min || *x > max)
        {
            p.add(&v, std::format("{}: '{}' = {} is out of range ({}..{})", what, key, *x, min, max));
            return false;
        }
        into = *x;
        return true;
    };
    if (const auto* a = n->as_array())
    {
        if (a->size() != kMaxSkillRank)
        {
            p.add(n, std::format("{}: '{}' must have exactly {} values (one per rank)", what, key, kMaxSkillRank));
            return std::nullopt;
        }
        for (std::size_t i = 0; i < kMaxSkillRank; ++i)
            if (!check(*a->get(i), out[i]))
                return std::nullopt;
        return out;
    }
    double v = 0.0;
    if (!check(*n, v))
        return std::nullopt;
    out.fill(v);
    return out;
}

bool validKey(std::string_view key)
{
    return !key.empty() && key.size() <= 32 &&
           std::all_of(key.begin(), key.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

constexpr std::array<std::string_view, 12> kPlaceholders = {"damage", "hits",  "crit",     "heal",   "radius", "targets",
                                                            "range",  "value", "duration", "value2", "duration2", "total"};

// Placeholders are {name} with a known name; braces must balance.
bool checkDescription(std::string_view d, const SkillDef& s, std::string& why)
{
    for (std::size_t i = 0; i < d.size(); ++i)
    {
        if (d[i] == '}')
        {
            why = "unbalanced '}'";
            return false;
        }
        if (d[i] != '{')
            continue;
        const std::size_t end = d.find('}', i);
        if (end == std::string_view::npos)
        {
            why = "unbalanced '{'";
            return false;
        }
        const std::string_view name = d.substr(i + 1, end - i - 1);
        if (std::find(kPlaceholders.begin(), kPlaceholders.end(), name) == kPlaceholders.end())
        {
            why = std::format("unknown placeholder '{{{}}}'", name);
            return false;
        }
        const bool needsEffect = name == "value" || name == "duration" || name == "total";
        const bool needsEffect2 = name == "value2" || name == "duration2";
        if ((needsEffect && s.effects.empty()) || (needsEffect2 && s.effects.size() < 2) ||
            (name == "damage" && s.ranks[0].multiplier <= 0.0f) || (name == "heal" && s.ranks[0].healFraction <= 0.0f) ||
            ((name == "radius" || name == "targets") && s.area == SkillArea::None) ||
            (name == "range" && s.target == SkillTarget::Self) || (name == "crit" && s.ranks[0].critBonus <= 0.0f))
        {
            why = std::format("placeholder '{{{}}}' has no value for this skill", name);
            return false;
        }
        i = end;
    }
    return true;
}

bool parseEffect(const toml::table& t, const SkillDef& skill, EffectDef& out, std::string_view parent, Problems& p)
{
    const std::string what = std::format("{} effect", parent);
    if (!onlyKeys(t, {"kind", "on", "duration_ms", "value", "tick_ms"}, what, p))
        return false;
    const auto kindName = getString(t, "kind", what, p);
    if (!kindName)
        return false;
    const auto kind = effectKindFromKey(*kindName);
    if (!kind)
    {
        p.add(t.get("kind"), std::format("{}: unknown kind '{}'", what, *kindName));
        return false;
    }
    out.kind = *kind;
    const auto on = getString(t, "on", what, p, std::string("target"));
    if (!on)
        return false;
    if (*on != "target" && *on != "self")
    {
        p.add(t.get("on"), std::format("{}: 'on' must be \"target\" or \"self\"", what));
        return false;
    }
    out.onSelf = *on == "self";
    const bool helpful = isHelpfulEffect(out.kind);
    // Helpful effects land on players, harmful ones on monsters (PROTOCOL_SKILLS.md §1.5).
    if (!helpful && (out.onSelf || !skill.hitsEnemies()))
    {
        p.add(&t, std::format("{}: '{}' is harmful and must land on the monsters the skill hits", what, *kindName));
        return false;
    }
    if (helpful && !out.onSelf && skill.hitsEnemies())
    {
        p.add(&t, std::format("{}: '{}' is helpful and cannot land on monsters (use on = \"self\")", what, *kindName));
        return false;
    }
    const auto duration = getRanked(t, "duration_ms", what, p, 100.0, 60'000.0, std::nullopt, true);
    if (!duration)
        return false;
    for (std::size_t i = 0; i < kMaxSkillRank; ++i)
        out.durationMs[i] = static_cast<std::uint32_t>((*duration)[i]);
    const bool valueless = out.kind == EffectKind::Stun || out.kind == EffectKind::Root || out.kind == EffectKind::Taunt;
    const bool periodic = out.kind == EffectKind::Dot || out.kind == EffectKind::Hot;
    if (valueless)
    {
        if (t.get("value"))
        {
            p.add(t.get("value"), std::format("{}: '{}' takes no value", what, *kindName));
            return false;
        }
    }
    else
    {
        const double max = out.kind == EffectKind::Slow || out.kind == EffectKind::Weaken ? 90.0
                           : out.kind == EffectKind::DodgeUp                              ? 50.0
                                                                                          : 500.0;
        const auto value = getRanked(t, "value", what, p, 0.1, max, std::nullopt);
        if (!value)
            return false;
        for (std::size_t i = 0; i < kMaxSkillRank; ++i)
            out.value[i] = static_cast<float>((*value)[i]);
    }
    if (periodic)
    {
        const auto tick = getInt(t, "tick_ms", what, p, 200, 5000, 1000);
        if (!tick)
            return false;
        out.tickMs = static_cast<std::uint32_t>(*tick);
        for (const auto d : out.durationMs)
            if (d < out.tickMs)
            {
                p.add(t.get("duration_ms"), std::format("{}: duration_ms must be at least tick_ms", what));
                return false;
            }
    }
    else if (t.get("tick_ms"))
    {
        p.add(t.get("tick_ms"), std::format("{}: tick_ms is only for dot / hot", what));
        return false;
    }
    return true;
}

bool parseSkill(const toml::table& t, SkillDef& out, Problems& p)
{
    std::string what = "[[skill]]";
    if (const auto id = getInt(t, "id", what, p, 1, kMaxSkillId))
        out.id = static_cast<std::uint16_t>(*id);
    else
        return false;
    what = std::format("skill {}", out.id);
    if (!onlyKeys(t,
                  {"id", "key", "name", "description", "class", "branch", "tier", "free", "target", "area", "radius",
                   "max_targets", "affects", "range", "mp", "cooldown_ms", "cast_ms", "damage", "hits", "crit_bonus",
                   "heal", "leap", "leap_stop", "projectile", "effect"},
                  what, p))
        return false;
    const auto key = getString(t, "key", what, p);
    const auto name = getString(t, "name", what, p);
    const auto description = getString(t, "description", what, p);
    if (!key || !name || !description)
        return false;
    if (!validKey(*key))
    {
        p.add(t.get("key"), std::format("{}: key '{}' must be 1-32 characters a-z, 0-9, _", what, *key));
        return false;
    }
    out.key = *key;
    out.name = *name;
    out.description = *description;
    const auto cls = getInt(t, "class", what, p, 1, 255);
    const auto branch = getInt(t, "branch", what, p, 0, 1);
    const auto tier = getInt(t, "tier", what, p, 1, kMaxSkillRank);
    const auto free = getBool(t, "free", what, p, false);
    const auto targetName = getString(t, "target", what, p);
    if (!cls || !branch || !tier || !free || !targetName)
        return false;
    if (!classTable(static_cast<std::uint8_t>(*cls)))
    {
        p.add(t.get("class"), std::format("{}: unknown class {}", what, *cls));
        return false;
    }
    out.charClass = static_cast<std::uint8_t>(*cls);
    out.branch = static_cast<std::uint8_t>(*branch);
    out.tier = static_cast<std::uint8_t>(*tier);
    out.free = *free;
    if (*targetName == "enemy")
        out.target = SkillTarget::Enemy;
    else if (*targetName == "friendly")
        out.target = SkillTarget::SelfOrFriendly;
    else if (*targetName == "self")
        out.target = SkillTarget::Self;
    else
    {
        p.add(t.get("target"), std::format("{}: target must be \"enemy\", \"friendly\" or \"self\"", what));
        return false;
    }

    if (const toml::node* a = t.get("area"))
    {
        const auto area = getString(t, "area", what, p);
        if (!area)
            return false;
        if (*area == "around_self")
            out.area = SkillArea::AroundSelf;
        else if (*area == "around_target")
            out.area = SkillArea::AroundTarget;
        else
        {
            p.add(a, std::format("{}: area must be \"around_self\" or \"around_target\"", what));
            return false;
        }
        if ((out.area == SkillArea::AroundSelf && out.target != SkillTarget::Self) ||
            (out.area == SkillArea::AroundTarget && out.target != SkillTarget::Enemy))
        {
            p.add(a, std::format("{}: around_self needs target \"self\", around_target needs target \"enemy\"", what));
            return false;
        }
        const auto targets = getInt(t, "max_targets", what, p, 1, 16);
        if (!targets)
            return false;
        out.maxTargets = static_cast<std::uint8_t>(*targets);
        const toml::node* r = t.get("radius");
        const auto rv = r ? numberOf(*r) : std::nullopt;
        if (!rv || !std::isfinite(*rv) || *rv < 0.5 || *rv > 30.0)
        {
            p.add(r ? r : &t, std::format("{}: radius must be a number 0.5..30", what));
            return false;
        }
        out.radius = static_cast<float>(*rv);
        const auto affects = getString(t, "affects", what, p, std::string("enemies"));
        if (!affects)
            return false;
        if (*affects != "enemies" && *affects != "allies")
        {
            p.add(t.get("affects"), std::format("{}: affects must be \"enemies\" or \"allies\"", what));
            return false;
        }
        out.affectsAllies = *affects == "allies";
        if (out.affectsAllies && out.area != SkillArea::AroundSelf)
        {
            p.add(t.get("affects"), std::format("{}: an ally area must be around_self", what));
            return false;
        }
    }
    else
    {
        for (const char* k : {"radius", "max_targets", "affects"})
            if (t.get(k))
            {
                p.add(t.get(k), std::format("{}: '{}' needs an area", what, k));
                return false;
            }
    }

    std::array<double, kMaxSkillRank> range{};
    if (out.target == SkillTarget::Self)
    {
        if (t.get("range"))
        {
            p.add(t.get("range"), std::format("{}: a self skill has no range", what));
            return false;
        }
    }
    else if (const auto r = getRanked(t, "range", what, p, 0.5, 40.0, std::nullopt))
        range = *r;
    else
        return false;
    const auto mp = getRanked(t, "mp", what, p, 0.0, 1000.0, std::nullopt, true);
    const auto cooldown = getRanked(t, "cooldown_ms", what, p, 0.0, kMaxSkillCooldownMs, std::nullopt, true);
    const auto cast = getRanked(t, "cast_ms", what, p, 0.0, 10'000.0, 0.0, true);
    const auto damage = getRanked(t, "damage", what, p, 0.0, 20.0, 0.0);
    const auto heal = getRanked(t, "heal", what, p, 0.0, 100.0, 0.0);
    const auto crit = getRanked(t, "crit_bonus", what, p, 0.0, 80.0, 0.0);
    const auto hits = getInt(t, "hits", what, p, 1, 5, 1);
    const auto leap = getBool(t, "leap", what, p, false);
    const auto projectile = getBool(t, "projectile", what, p, false);
    if (!mp || !cooldown || !cast || !damage || !heal || !crit || !hits || !leap || !projectile)
        return false;
    out.hits = static_cast<std::uint8_t>(*hits);
    out.leap = *leap;
    out.projectile = *projectile;
    for (std::size_t i = 0; i < kMaxSkillRank; ++i)
    {
        auto& r = out.ranks[i];
        r.range = static_cast<float>(range[i]);
        r.mpCost = static_cast<std::uint32_t>((*mp)[i]);
        r.cooldownMs = static_cast<std::uint32_t>((*cooldown)[i]);
        r.castMs = static_cast<std::uint32_t>((*cast)[i]);
        r.multiplier = static_cast<float>((*damage)[i]);
        r.healFraction = static_cast<float>((*heal)[i] / 100.0);
        r.critBonus = static_cast<float>((*crit)[i] / 100.0);
    }
    const bool anyDamage = std::any_of(out.ranks.begin(), out.ranks.end(), [](const SkillRankData& r) { return r.multiplier > 0.0f; });
    const bool anyHeal = std::any_of(out.ranks.begin(), out.ranks.end(), [](const SkillRankData& r) { return r.healFraction > 0.0f; });
    if (anyDamage && !out.hitsEnemies())
    {
        p.add(t.get("damage"), std::format("{}: damage needs an enemy target or an enemy area", what));
        return false;
    }
    if (anyHeal && !out.hitsAllies())
    {
        p.add(t.get("heal"), std::format("{}: heal needs a friendly / self target or an ally area", what));
        return false;
    }
    if ((out.hits > 1 || t.get("crit_bonus")) && !anyDamage)
    {
        p.add(&t, std::format("{}: hits / crit_bonus need damage", what));
        return false;
    }
    if (out.leap)
    {
        if (out.target != SkillTarget::Enemy || out.area != SkillArea::None)
        {
            p.add(t.get("leap"), std::format("{}: a leap needs target \"enemy\" and no area", what));
            return false;
        }
        const toml::node* ls = t.get("leap_stop");
        const auto v = ls ? numberOf(*ls) : std::optional<double>(1.5);
        if (!v || !std::isfinite(*v) || *v < 0.5 || *v > 5.0)
        {
            p.add(ls, std::format("{}: leap_stop must be a number 0.5..5", what));
            return false;
        }
        out.leapStop = static_cast<float>(*v);
    }
    else if (t.get("leap_stop"))
    {
        p.add(t.get("leap_stop"), std::format("{}: leap_stop needs leap = true", what));
        return false;
    }

    if (const toml::node* e = t.get("effect"))
    {
        const auto* list = e->as_array();
        if (!list || list->size() > kMaxSkillEffects)
        {
            p.add(e, std::format("{}: 'effect' must be at most {} [[skill.effect]] tables", what, kMaxSkillEffects));
            return false;
        }
        for (const auto& node : *list)
        {
            const auto* et = node.as_table();
            if (!et)
            {
                p.add(&node, std::format("{}: 'effect' must be an array of tables ([[skill.effect]])", what));
                return false;
            }
            EffectDef def;
            if (!parseEffect(*et, out, def, what, p))
                return false;
            if (std::any_of(out.effects.begin(), out.effects.end(), [&](const EffectDef& o) { return o.kind == def.kind; }))
            {
                p.add(et, std::format("{}: two effects of the same kind", what));
                return false;
            }
            out.effects.push_back(def);
        }
    }
    if (!anyDamage && !anyHeal && out.effects.empty())
    {
        p.add(&t, std::format("{}: the skill does nothing (no damage, heal or effect)", what));
        return false;
    }
    std::string why;
    if (!checkDescription(out.description, out, why))
    {
        p.add(t.get("description"), std::format("{}: description: {}", what, why));
        return false;
    }
    const SkillRankData& r1 = out.ranks[0];
    out.range = r1.range;
    out.mpCost = r1.mpCost;
    out.cooldownMs = r1.cooldownMs;
    out.castMs = r1.castMs;
    out.multiplier = r1.multiplier;
    out.healFraction = r1.healFraction;
    return true;
}

// ---- Process-wide catalog ----

struct Global
{
    std::mutex mutex;
    std::vector<std::shared_ptr<const SkillCatalog>> installed; // kept alive forever
    std::atomic<const SkillCatalog*> current{nullptr};
    std::string defaultError;
};

Global& global()
{
    static Global g;
    return g;
}

const SkillCatalog& defaultCatalog()
{
    static const SkillCatalog catalog = [] {
        SkillCatalog c;
        std::string error;
        if (const auto file = locateSkillsFile())
        {
            if (!loadSkillCatalog(*file, c, error))
                c = {};
        }
        else
            error = std::format("{} not found (searched upwards from the working directory and the executable directory)",
                                kSkillsFile);
        if (!error.empty())
        {
            VO_LOG_ERROR("skills: {}", error);
            const std::scoped_lock lock(global().mutex);
            global().defaultError = error;
        }
        return c;
    }();
    return catalog;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to)
{
    for (std::size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
        s.replace(pos, from.size(), to);
    return s;
}

} // namespace

bool isHelpfulEffect(EffectKind k) noexcept
{
    switch (k)
    {
    case EffectKind::AttackUp:
    case EffectKind::DefenseUp:
    case EffectKind::Haste:
    case EffectKind::DodgeUp:
    case EffectKind::MaxHpUp:
    case EffectKind::Hot:
    case EffectKind::Shield: return true;
    default: return false;
    }
}

namespace {
constexpr std::array<std::string_view, kEffectKindCount> kEffectKeys = {
    "attack_up", "defense_up", "haste", "dodge_up", "max_hp_up", "dot", "hot",
    "slow",      "stun",       "root",  "shield",   "weaken",    "taunt"};
}

std::optional<EffectKind> effectKindFromKey(std::string_view key) noexcept
{
    for (std::size_t i = 0; i < kEffectKeys.size(); ++i)
        if (kEffectKeys[i] == key)
            return static_cast<EffectKind>(i + 1);
    return std::nullopt;
}

std::string_view effectKindKey(EffectKind k) noexcept
{
    const auto i = static_cast<std::size_t>(k);
    return i >= 1 && i <= kEffectKeys.size() ? kEffectKeys[i - 1] : std::string_view("?");
}

const SkillDef* SkillCatalog::find(std::uint16_t id) const noexcept
{
    if (id == 0)
        return nullptr;
    const auto it = std::lower_bound(m_skills.begin(), m_skills.end(), id,
                                     [](const SkillDef& s, std::uint16_t v) { return s.id < v; });
    return it != m_skills.end() && it->id == id ? &*it : nullptr;
}

const SkillDef* SkillCatalog::freeSkill(std::uint8_t charClass) const noexcept
{
    for (const auto& s : m_skills)
        if (s.charClass == charClass && s.free)
            return &s;
    return nullptr;
}

std::string_view SkillCatalog::branchName(std::uint8_t charClass, std::uint8_t branch) const noexcept
{
    for (const auto& b : m_branches)
        if (b.charClass == charClass && b.index == branch)
            return b.name;
    return {};
}

bool parseSkillCatalog(std::string_view tomlText, std::string_view sourceName, SkillCatalog& out, std::string& error)
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
    SkillCatalog c;
    if (!onlyKeys(root, {"rules", "branch", "skill"}, "top level", p))
    {
        error = p.error();
        return false;
    }
    // [rules]
    if (const toml::node* n = root.get("rules"))
    {
        const auto* t = n->as_table();
        if (!t)
            p.add(n, "'rules' must be a table ([rules])");
        else if (onlyKeys(*t, {"points_per_level", "tier_points"}, "[rules]", p))
        {
            if (const auto ppl = getInt(*t, "points_per_level", "[rules]", p, 1, 10))
                c.m_rules.pointsPerLevel = static_cast<std::uint16_t>(*ppl);
            if (const toml::node* tp = t->get("tier_points"))
            {
                const auto* a = tp->as_array();
                bool ok = a && a->size() == kMaxSkillRank;
                for (std::size_t i = 0; ok && i < kMaxSkillRank; ++i)
                {
                    const auto* v = a->get(i)->as_integer();
                    ok = v && v->get() >= 0 && v->get() <= 100 && (i > 0 || v->get() == 0) &&
                         (i == 0 || v->get() >= c.m_rules.tierPoints[i - 1]);
                    if (ok)
                        c.m_rules.tierPoints[i] = static_cast<std::uint16_t>(v->get());
                }
                if (!ok)
                    p.add(tp, "[rules]: tier_points must be 3 non-decreasing integers 0..100 starting with 0");
            }
            else
                p.add(t, "[rules]: missing 'tier_points'");
        }
    }
    else
        p.add(nullptr, "missing [rules]");

    const auto arrayOf = [&](std::string_view key) -> const toml::array* {
        const toml::node* n = root.get(key);
        const auto* a = n ? n->as_array() : nullptr;
        if (!a || a->empty())
        {
            p.add(n, std::format("'{}' must be a non-empty array of tables ([[{}]])", key, key));
            return nullptr;
        }
        return a;
    };
    if (!p.any())
    {
        if (const toml::array* list = arrayOf("branch"))
        {
            for (const auto& node : *list)
            {
                const auto* t = node.as_table();
                if (!t)
                {
                    p.add(&node, "'branch' must be an array of tables ([[branch]])");
                    break;
                }
                if (!onlyKeys(*t, {"class", "index", "name"}, "[[branch]]", p))
                    break;
                const auto cls = getInt(*t, "class", "[[branch]]", p, 1, 255);
                const auto index = getInt(*t, "index", "[[branch]]", p, 0, 1);
                const auto name = getString(*t, "name", "[[branch]]", p);
                if (!cls || !index || !name)
                    break;
                if (!classTable(static_cast<std::uint8_t>(*cls)))
                {
                    p.add(t, std::format("[[branch]]: unknown class {}", *cls));
                    break;
                }
                SkillBranch b{static_cast<std::uint8_t>(*cls), static_cast<std::uint8_t>(*index), *name};
                if (!c.branchName(b.charClass, b.index).empty())
                {
                    p.add(t, std::format("[[branch]]: class {} branch {} defined twice", b.charClass, b.index));
                    break;
                }
                c.m_branches.push_back(std::move(b));
            }
        }
    }
    if (!p.any())
    {
        if (const toml::array* list = arrayOf("skill"))
        {
            for (const auto& node : *list)
            {
                const auto* t = node.as_table();
                if (!t)
                {
                    p.add(&node, "'skill' must be an array of tables ([[skill]])");
                    break;
                }
                SkillDef s;
                if (!parseSkill(*t, s, p))
                    break;
                if (c.branchName(s.charClass, s.branch).empty())
                {
                    p.add(t, std::format("skill {}: class {} has no branch {} ([[branch]])", s.id, s.charClass, s.branch));
                    break;
                }
                for (const auto& o : c.m_skills)
                {
                    if (o.id == s.id)
                        p.add(t, std::format("duplicate skill id {}", s.id));
                    else if (o.key == s.key)
                        p.add(t, std::format("skill {}: key '{}' already used by skill {}", s.id, s.key, o.id));
                    else if (o.name == s.name)
                        p.add(t, std::format("skill {}: name '{}' already used by skill {}", s.id, s.name, o.id));
                }
                if (p.any())
                    break;
                c.m_skills.push_back(std::move(s));
            }
        }
    }
    std::sort(c.m_skills.begin(), c.m_skills.end(), [](const SkillDef& a, const SkillDef& b) { return a.id < b.id; });
    // Per class: every branch defined, has a tier-1 skill; exactly one free skill (branch 0, tier 1,
    // the class's M9 skill id); at most kMaxSkillStateEntries skills.
    if (!p.any())
    {
        for (std::uint8_t cls = 1; cls <= 4 && !p.any(); ++cls)
        {
            const ClassTable* table = classTable(cls);
            for (std::uint8_t b = 0; b < 2 && !p.any(); ++b)
            {
                if (c.branchName(cls, b).empty())
                    p.add(nullptr, std::format("class {} has no [[branch]] with index {}", cls, b));
                else if (std::none_of(c.m_skills.begin(), c.m_skills.end(), [&](const SkillDef& s) {
                             return s.charClass == cls && s.branch == b && s.tier == 1;
                         }))
                    p.add(nullptr, std::format("class {} branch {} has no tier-1 skill", cls, b));
            }
            const auto frees = std::count_if(c.m_skills.begin(), c.m_skills.end(),
                                             [&](const SkillDef& s) { return s.charClass == cls && s.free; });
            const SkillDef* f = c.freeSkill(cls);
            if (!p.any() && frees != 1)
                p.add(nullptr, std::format("class {} must have exactly one free skill (has {})", cls, frees));
            else if (!p.any() && (f->branch != 0 || f->tier != 1 || !table || f->id != table->skillId))
                p.add(nullptr, std::format("the free skill of class {} must be skill {} in branch 0, tier 1", cls,
                                           table ? table->skillId : 0));
            const auto count = std::count_if(c.m_skills.begin(), c.m_skills.end(),
                                             [&](const SkillDef& s) { return s.charClass == cls; });
            if (!p.any() && static_cast<std::size_t>(count) > net::kMaxSkillStateEntries)
                p.add(nullptr, std::format("class {} has more than {} skills", cls, net::kMaxSkillStateEntries));
        }
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    out = std::move(c);
    return true;
}

bool loadSkillCatalog(const std::filesystem::path& file, SkillCatalog& out, std::string& error)
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
    return parseSkillCatalog(ss.str(), core::pathToUtf8(file), out, error);
}

std::optional<std::filesystem::path> locateSkillsFile()
{
    return core::locateDataFile(std::filesystem::path(kSkillsFile));
}

const SkillCatalog& skills() noexcept
{
    if (const SkillCatalog* c = global().current.load(std::memory_order_acquire))
        return *c;
    return defaultCatalog();
}

void installSkillCatalog(std::shared_ptr<const SkillCatalog> catalog)
{
    if (!catalog)
        return;
    Global& g = global();
    const std::scoped_lock lock(g.mutex);
    g.installed.push_back(catalog);
    g.current.store(catalog.get(), std::memory_order_release);
}

std::string defaultSkillCatalogError()
{
    if (global().current.load(std::memory_order_acquire))
        return {};
    (void)defaultCatalog();
    const std::scoped_lock lock(global().mutex);
    return global().defaultError;
}

// ---- Ranks and points ----

SkillRanks defaultSkillRanks(const SkillCatalog& catalog, std::uint8_t charClass) noexcept
{
    SkillRanks r{};
    if (const SkillDef* f = catalog.freeSkill(charClass))
        r[f->id] = 1;
    return r;
}

SkillRanks normalizeSkillRanks(const SkillCatalog& catalog, std::uint8_t charClass, const SkillRanks& ranks) noexcept
{
    SkillRanks r{};
    for (const auto& s : catalog.skills())
    {
        if (s.charClass != charClass)
            continue;
        r[s.id] = std::min<std::uint8_t>(ranks[s.id], kMaxSkillRank);
        if (s.free)
            r[s.id] = std::max<std::uint8_t>(r[s.id], 1);
    }
    return r;
}

std::uint32_t skillPointsForLevel(const SkillCatalog& catalog, std::uint16_t level) noexcept
{
    const std::uint32_t l = std::clamp<std::uint16_t>(level, 1, kLevelCap);
    return static_cast<std::uint32_t>(catalog.rules().pointsPerLevel) * (l - 1);
}

namespace {
std::uint32_t spentOf(const SkillDef& s, const SkillRanks& ranks) noexcept
{
    const std::uint32_t rank = std::min<std::uint8_t>(ranks[s.id], kMaxSkillRank);
    const std::uint32_t base = s.free ? 1u : 0u;
    return rank > base ? rank - base : 0u;
}
} // namespace

std::uint32_t skillPointsSpent(const SkillCatalog& catalog, std::uint8_t charClass, const SkillRanks& ranks) noexcept
{
    std::uint32_t sum = 0;
    for (const auto& s : catalog.skills())
        if (s.charClass == charClass)
            sum += spentOf(s, ranks);
    return sum;
}

std::uint32_t branchPointsSpent(const SkillCatalog& catalog, std::uint8_t charClass, std::uint8_t branch,
                                const SkillRanks& ranks) noexcept
{
    std::uint32_t sum = 0;
    for (const auto& s : catalog.skills())
        if (s.charClass == charClass && s.branch == branch)
            sum += spentOf(s, ranks);
    return sum;
}

std::uint16_t unspentSkillPoints(const SkillCatalog& catalog, std::uint8_t charClass, std::uint16_t level,
                                 const SkillRanks& ranks) noexcept
{
    const std::uint32_t points = skillPointsForLevel(catalog, level);
    const std::uint32_t spent = skillPointsSpent(catalog, charClass, ranks);
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(spent >= points ? 0u : points - spent, 65535u));
}

std::uint16_t tierGatePoints(const SkillCatalog& catalog, const SkillDef& skill) noexcept
{
    const std::size_t i = std::clamp<std::size_t>(skill.tier, 1, kMaxSkillRank) - 1;
    return catalog.rules().tierPoints[i];
}

net::SkillLearnResult checkSkillLearn(const SkillCatalog& catalog, std::uint8_t charClass, std::uint16_t level,
                                      const SkillRanks& ranks, std::uint16_t skillId) noexcept
{
    using R = net::SkillLearnResult;
    const SkillDef* s = catalog.find(skillId);
    if (!s || s->charClass != charClass)
        return R::UnknownSkill;
    const SkillRanks r = normalizeSkillRanks(catalog, charClass, ranks);
    if (r[s->id] >= kMaxSkillRank)
        return R::MaxRank;
    if (unspentSkillPoints(catalog, charClass, level, r) == 0)
        return R::NotEnoughPoints;
    if (branchPointsSpent(catalog, charClass, s->branch, r) < tierGatePoints(catalog, *s))
        return R::TierLocked;
    return R::Ok;
}

SkillBar defaultSkillBar(const SkillCatalog& catalog, std::uint8_t charClass) noexcept
{
    SkillBar bar{};
    if (const SkillDef* f = catalog.freeSkill(charClass))
        bar[0] = f->id;
    return bar;
}

namespace {
bool validSlot(const SkillCatalog& catalog, std::uint8_t charClass, const SkillRanks& ranks, std::uint16_t id,
               const ItemCatalog* items) noexcept
{
    if (id == 0)
        return true;
    if ((id & kBarItemFlag) != 0)
    {
        // v8 (PROTOCOL_SHOP.md §4.1): a consumable, whether the character owns one or not.
        const std::uint16_t t = barItemTemplate(id);
        if (t == 0)
            return false;
        if (!items)
            return true;
        const ItemTemplate* it = items->find(t);
        return it && it->isConsumable();
    }
    const SkillDef* s = catalog.find(id);
    return s && s->charClass == charClass && normalizeSkillRanks(catalog, charClass, ranks)[s->id] >= 1;
}
} // namespace

bool validSkillBar(const SkillCatalog& catalog, std::uint8_t charClass, const SkillRanks& ranks, const SkillBar& bar,
                   const ItemCatalog* items) noexcept
{
    return std::all_of(bar.begin(), bar.end(),
                       [&](std::uint16_t id) { return validSlot(catalog, charClass, ranks, id, items); });
}

SkillBar sanitizeSkillBar(const SkillCatalog& catalog, std::uint8_t charClass, const SkillRanks& ranks,
                          const SkillBar& bar, const ItemCatalog* items) noexcept
{
    SkillBar out = bar;
    for (auto& id : out)
        if (!validSlot(catalog, charClass, ranks, id, items))
            id = 0;
    return out;
}

std::string formatSkillNumber(double v)
{
    const double rounded = std::round(v * 10.0) / 10.0;
    if (std::abs(rounded - std::round(rounded)) < 1e-6)
        return std::format("{}", static_cast<long long>(std::llround(rounded)));
    std::string s = std::format("{:.1f}", rounded);
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

std::string describeSkill(const SkillDef& skill, std::uint8_t rank)
{
    const SkillRankData& r = skill.at(rank);
    const std::size_t ri = static_cast<std::size_t>(std::clamp<std::uint8_t>(rank, 1, kMaxSkillRank) - 1);
    std::string s = skill.description;
    s = replaceAll(std::move(s), "{damage}", formatSkillNumber(r.multiplier * 100.0));
    s = replaceAll(std::move(s), "{hits}", formatSkillNumber(skill.hits));
    s = replaceAll(std::move(s), "{crit}", formatSkillNumber(r.critBonus * 100.0));
    s = replaceAll(std::move(s), "{heal}", formatSkillNumber(r.healFraction * 100.0));
    s = replaceAll(std::move(s), "{radius}", formatSkillNumber(skill.radius));
    s = replaceAll(std::move(s), "{targets}", formatSkillNumber(skill.maxTargets));
    s = replaceAll(std::move(s), "{range}", formatSkillNumber(r.range));
    for (std::size_t e = 0; e < skill.effects.size(); ++e)
    {
        const EffectDef& def = skill.effects[e];
        const std::string suffix = e == 0 ? "" : "2";
        s = replaceAll(std::move(s), "{value" + suffix + "}", formatSkillNumber(def.value[ri]));
        s = replaceAll(std::move(s), "{duration" + suffix + "}", formatSkillNumber(def.durationMs[ri] / 1000.0));
        if (e == 0)
        {
            const double ticks = def.tickMs > 0 ? std::floor(static_cast<double>(def.durationMs[ri]) / def.tickMs) : 0.0;
            s = replaceAll(std::move(s), "{total}", formatSkillNumber(def.value[ri] * ticks));
        }
    }
    return s;
}

} // namespace vo::game
