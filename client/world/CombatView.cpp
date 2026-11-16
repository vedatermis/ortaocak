#include "client/world/CombatView.h"

#include "shared/game/Combat.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace vo::client::world {

// ---- Name label colour ----

LevelTint levelTint(std::uint16_t playerLevel, std::uint16_t monsterLevel) noexcept
{
    const int d = static_cast<int>(monsterLevel) - static_cast<int>(playerLevel);
    if (d <= -3)
        return LevelTint::Trivial;
    if (d <= 0)
        return LevelTint::Even;
    if (d <= 2)
        return LevelTint::Higher;
    if (d <= 4)
        return LevelTint::Hard;
    return LevelTint::Deadly;
}

std::uint32_t levelTintRgb(LevelTint t) noexcept
{
    switch (t)
    {
    case LevelTint::Trivial: return 0x9A9A96;
    case LevelTint::Even: return 0xF4F1E8;
    case LevelTint::Higher: return 0xF2D24B;
    case LevelTint::Hard: return 0xF08A30;
    case LevelTint::Deadly: return 0xEE4A3C;
    }
    return 0xF4F1E8;
}

const char* levelTintName(LevelTint t) noexcept
{
    switch (t)
    {
    case LevelTint::Trivial: return "grey";
    case LevelTint::Even: return "white";
    case LevelTint::Higher: return "yellow";
    case LevelTint::Hard: return "orange";
    case LevelTint::Deadly: return "red";
    }
    return "?";
}

// ---- Picking ----

std::optional<float> rayHitsCapsule(const Ray& ray, Vec3 feet, float radius, float height) noexcept
{
    // Capsule = segment a..b (feet + r .. feet + h - r) swept by r. Closest approach of the ray to
    // the segment, then the entry distance into the sphere around that closest segment point.
    const float r = std::max(radius, 0.01f);
    const float h = std::max(height, 2.0f * r);
    const Vec3 a = feet + Vec3{0.0f, r, 0.0f};
    const Vec3 b = feet + Vec3{0.0f, h - r, 0.0f};
    const Vec3 d = ray.direction;
    const Vec3 u = b - a;
    const Vec3 w = ray.origin - a;
    const float uu = assets::dot(u, u);
    const float ud = assets::dot(u, d);
    const float dd = assets::dot(d, d);
    const float uw = assets::dot(u, w);
    const float dw = assets::dot(d, w);
    const float denom = uu * dd - ud * ud;
    float s = 0.0f; // on the segment, 0..1
    if (denom > 1e-8f)
        s = std::clamp((uw * dd - ud * dw) / denom, 0.0f, 1.0f);
    // Iterate once: best t for that s, then best s for that t (clamped), good enough for a capsule.
    float t = std::max(0.0f, (assets::dot(d, a + u * s - ray.origin)) / std::max(dd, 1e-8f));
    if (uu > 1e-8f)
        s = std::clamp(assets::dot(ray.origin + d * t - a, u) / uu, 0.0f, 1.0f);
    const Vec3 c = a + u * s;
    // Ray vs sphere (c, r).
    const Vec3 oc = ray.origin - c;
    const float bq = assets::dot(oc, d);
    const float cq = assets::dot(oc, oc) - r * r;
    const float disc = bq * bq - dd * cq;
    if (disc < 0.0f)
        return std::nullopt;
    const float sq = std::sqrt(disc);
    float hit = (-bq - sq) / dd;
    if (hit < 0.0f)
        hit = (-bq + sq) / dd; // origin inside
    if (hit < 0.0f)
        return std::nullopt;
    return hit;
}

std::uint32_t pickEntity(const Ray& ray, std::span<const PickCandidate> candidates, float maxDistance) noexcept
{
    std::uint32_t best = 0;
    float bestT = maxDistance;
    for (const PickCandidate& c : candidates)
    {
        if (const auto t = rayHitsCapsule(ray, c.feet, c.radius, c.height); t && *t <= bestT)
        {
            bestT = *t;
            best = c.id;
        }
    }
    return best;
}

// ---- Tab targeting ----

std::uint32_t nextTabTarget(std::span<const TabCandidate> candidates, std::uint32_t current)
{
    std::vector<TabCandidate> v;
    v.reserve(candidates.size());
    for (const TabCandidate& c : candidates)
        if (c.id != 0 && c.distance >= 0.0f && c.distance <= kTabRange)
            v.push_back(c);
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end(), [](const TabCandidate& x, const TabCandidate& y) {
        return x.distance != y.distance ? x.distance < y.distance : x.id < y.id;
    });
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].id == current)
            return v[(i + 1) % v.size()].id;
    return v.front().id;
}

// ---- Attack animation ----

AnimState attackClipFor(const game::ItemTemplate* mainHand, bool monster, std::uint32_t swingIndex) noexcept
{
    const AnimState punch = (swingIndex % 2 == 0) ? AnimState::PunchJab : AnimState::PunchCross;
    if (monster || mainHand == nullptr)
        return punch;
    switch (mainHand->grip)
    {
    case game::Grip::Sword:
    case game::Grip::Dagger:
    case game::Grip::Mace:
    case game::Grip::Axe2h:
        return AnimState::SwordAttack;
    case game::Grip::Staff:
        return AnimState::SpellShoot;
    case game::Grip::Shield:
        return punch; // a shield alone in the main hand cannot happen; punch anyway
    }
    return punch;
}

// ---- Skill timers ----

void SkillTimers::onAction(double now) noexcept
{
    m_gcdStart = now;
    m_gcdEnd = now + static_cast<double>(game::kGlobalCooldown.count()) / 1000.0;
}

void SkillTimers::onSkillUsed(std::uint16_t skillId, double now) noexcept
{
    const game::SkillDef* s = game::findSkill(skillId);
    if (s == nullptr)
        return;
    onAction(now);
    if (m_skill == skillId && now < m_skillEnd - 0.25)
        return; // a further hit of the same use (Çifte Kesik's second cut)
    m_skill = skillId;
    m_skillStart = now;
    m_skillEnd = now + static_cast<double>(s->cooldownMs) / 1000.0;
}

void SkillTimers::onCastStart(std::uint16_t skillId, std::uint32_t castMs, double now) noexcept
{
    m_castSkill = skillId;
    m_castStart = now;
    m_castEnd = now + static_cast<double>(castMs) / 1000.0;
}

void SkillTimers::onCastEnd(std::uint16_t skillId, bool completed, double now) noexcept
{
    if (m_castSkill == skillId)
        m_castSkill = 0;
    if (completed)
        onSkillUsed(skillId, now);
}

double SkillTimers::remaining(std::uint16_t skillId, double now) const noexcept
{
    double r = std::max(0.0, m_gcdEnd - now);
    if (skillId != 0 && skillId == m_skill)
        r = std::max(r, m_skillEnd - now);
    return std::max(0.0, r);
}

float SkillTimers::sweep(std::uint16_t skillId, double now) const noexcept
{
    const double own = skillId != 0 && skillId == m_skill ? m_skillEnd - now : 0.0;
    const double gcd = m_gcdEnd - now;
    if (own <= 0.0 && gcd <= 0.0)
        return 0.0f;
    if (own >= gcd)
        return static_cast<float>(std::clamp(own / std::max(m_skillEnd - m_skillStart, 1e-3), 0.0, 1.0));
    return static_cast<float>(std::clamp(gcd / std::max(m_gcdEnd - m_gcdStart, 1e-3), 0.0, 1.0));
}

float SkillTimers::castProgress(double now) const noexcept
{
    if (m_castSkill == 0)
        return 0.0f;
    const double len = m_castEnd - m_castStart;
    if (len <= 0.0)
        return 1.0f;
    return static_cast<float>(std::clamp((now - m_castStart) / len, 0.0, 1.0));
}

// ---- Floating numbers ----

FloatKind classifyCombatNumber(std::uint8_t kind, std::uint8_t flags, bool targetIsLocal) noexcept
{
    if (kind == vo::net::kCombatKindHeal)
        return FloatKind::Heal;
    if (targetIsLocal)
        return FloatKind::TakenDamage;
    if ((flags & vo::net::kCombatFlagCritical) != 0)
        return FloatKind::Critical;
    return FloatKind::Damage;
}

FloatStyle floatStyle(FloatKind kind) noexcept
{
    switch (kind)
    {
    case FloatKind::Damage: return {0xFFFFFF, 1.0f, 1.1f, 1.1f};
    case FloatKind::Critical: return {0xFFD43A, 1.45f, 1.5f, 1.4f};
    case FloatKind::Heal: return {0x6BE36B, 1.1f, 1.4f, 1.2f};
    case FloatKind::TakenDamage: return {0xFF5A4A, 1.05f, 1.2f, 1.0f};
    }
    return {};
}

void FloatingNumbers::add(FloatKind kind, std::uint32_t amount, Vec3 anchor)
{
    if (m_items.size() >= kMax)
        m_items.erase(m_items.begin());
    FloatingNumber n;
    n.anchor = anchor;
    n.amount = amount;
    n.kind = kind;
    // Fan simultaneous numbers out sideways so they do not stack into one.
    static constexpr std::array<float, 5> kOffsets = {0.0f, -26.0f, 26.0f, -13.0f, 13.0f};
    n.offsetX = kOffsets[m_added % kOffsets.size()];
    ++m_added;
    m_items.push_back(n);
}

void FloatingNumbers::update(float dt)
{
    for (FloatingNumber& n : m_items)
        n.age += dt;
    std::erase_if(m_items, [](const FloatingNumber& n) { return n.age >= floatStyle(n.kind).lifetime; });
}

float FloatingNumbers::alpha(const FloatingNumber& n) noexcept
{
    const float life = floatStyle(n.kind).lifetime;
    const float u = std::clamp(n.age / life, 0.0f, 1.0f);
    if (u < 0.08f)
        return u / 0.08f;
    if (u > 0.66f)
        return std::max(0.0f, 1.0f - (u - 0.66f) / 0.34f);
    return 1.0f;
}

float FloatingNumbers::rise(const FloatingNumber& n) noexcept
{
    const FloatStyle s = floatStyle(n.kind);
    const float u = std::clamp(n.age / s.lifetime, 0.0f, 1.0f);
    return s.rise * (1.0f - (1.0f - u) * (1.0f - u)); // ease out
}

// ---- Messages ----

std::string_view actionFailText(vo::net::CombatAction action, vo::net::ActionFailReason reason) noexcept
{
    using R = vo::net::ActionFailReason;
    if (action == vo::net::CombatAction::Respawn && reason == R::OnCooldown)
        return "Henüz yeniden doğamazsın";
    if (action == vo::net::CombatAction::Stance && reason == R::Busy)
        return "Şu an oturamazsın";
    switch (reason)
    {
    case R::NoTarget: return "Hedef yok";
    case R::OutOfRange: return "Menzil dışında";
    case R::NotAttackable: return "Buna saldırılamaz";
    case R::TargetDead: return "Hedef ölü";
    case R::YouAreDead: return "Ölüyken yapamazsın";
    case R::OnCooldown: return "Henüz hazır değil";
    case R::NotEnoughMp: return "Yeterli mana yok";
    case R::WrongTargetType: return "Yanlış hedef";
    case R::Interrupted: return "Büyü kesildi";
    case R::InSafeZone: return "Güvenli bölgede saldırılmaz";
    case R::UnknownSkill: return "Bu beceriyi bilmiyorsun";
    case R::Busy: return "Şu an meşgulsün";
    }
    return "Yapılamadı";
}

// ---- Automation ----

namespace {

std::vector<std::string_view> tokens(std::string_view s)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t')
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool number(std::string_view s, float& out)
{
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size() && std::isfinite(out);
}

std::string join(std::span<const std::string_view> t)
{
    std::string s;
    for (const std::string_view x : t)
    {
        if (!s.empty())
            s += ' ';
        s += x;
    }
    return s;
}

} // namespace

std::optional<CombatCommand> parseCombatCommand(std::string_view line, std::string& error)
{
    error.clear();
    const std::vector<std::string_view> t = tokens(line);
    if (t.empty())
        return std::nullopt;
    const std::string_view verb = t[0];
    const std::span<const std::string_view> args(t.data() + 1, t.size() - 1);
    CombatCommand c;
    const auto bad = [&](std::string_view usage) -> std::optional<CombatCommand> {
        error = std::format("usage: {}", usage);
        return std::nullopt;
    };
    const auto range = [&](CombatVerb v, std::string_view usage) -> std::optional<CombatCommand> {
        if (args.size() != 2 || !number(args[0], c.a) || !number(args[1], c.b) || c.a > c.b)
            return bad(usage);
        c.verb = v;
        return c;
    };
    if (verb == "target")
    {
        c.verb = CombatVerb::Target;
        if (args.empty())
            return bad("target nearest [<monster name>] | target none | target self | target player <name>");
        c.mode = std::string(args[0]);
        if (c.mode == "none" || c.mode == "self")
        {
            if (args.size() != 1)
                return bad("target none | target self");
            return c;
        }
        if (c.mode == "nearest")
        {
            c.name = join(args.subspan(1));
            return c;
        }
        if (c.mode == "player")
        {
            if (args.size() < 2)
                return bad("target player <name>");
            c.name = join(args.subspan(1));
            return c;
        }
        return bad("target nearest [<monster name>] | target none | target self | target player <name>");
    }
    const auto noArgs = [&](CombatVerb v, std::string_view usage) -> std::optional<CombatCommand> {
        if (!args.empty())
            return bad(usage);
        c.verb = v;
        return c;
    };
    if (verb == "tab")
        return noArgs(CombatVerb::Tab, "tab");
    if (verb == "attack")
        return noArgs(CombatVerb::Attack, "attack");
    if (verb == "attackstop")
        return noArgs(CombatVerb::AttackStop, "attackstop");
    if (verb == "cast")
        return noArgs(CombatVerb::Cast, "cast");
    if (verb == "respawn")
        return noArgs(CombatVerb::Respawn, "respawn");
    if (verb == "combatlog")
        return noArgs(CombatVerb::Log, "combatlog");
    if (verb == "stance")
    {
        if (args.size() != 1 || (args[0] != "sit" && args[0] != "stand"))
            return bad("stance sit|stand");
        c.verb = CombatVerb::Stance;
        c.mode = std::string(args[0]);
        return c;
    }
    if (verb == "expecthp")
        return range(CombatVerb::ExpectHp, "expecthp <minPercent> <maxPercent>");
    if (verb == "expecttargethp")
        return range(CombatVerb::ExpectTargetHp, "expecttargethp <minPercent> <maxPercent>");
    if (verb == "expectlevel")
    {
        if (args.size() != 1 || !number(args[0], c.a) || c.a < 1.0f)
            return bad("expectlevel <level>");
        c.verb = CombatVerb::ExpectLevel;
        return c;
    }
    if (verb == "expectcombatmessage")
    {
        if (args.empty())
            return bad("expectcombatmessage <text>");
        c.verb = CombatVerb::ExpectCombatMsg;
        c.name = join(args);
        return c;
    }
    if (verb == "hunt")
    {
        c.verb = CombatVerb::Hunt;
        if (args.size() == 1 && args[0] == "stop")
        {
            c.mode = "stop";
            return c;
        }
        if (args.size() < 2 || !number(args.back(), c.a) || c.a < 2.0f || c.a > 10.0f)
            return bad("hunt <monster name> <level 2..10> | hunt stop");
        c.name = join(args.subspan(0, args.size() - 1));
        return c;
    }
    if (verb == "walktomonster")
    {
        if (args.empty())
            return bad("walktomonster <monster name> [stopShort]");
        std::size_t nameEnd = args.size();
        c.a = 2.0f;
        if (args.size() >= 2 && number(args.back(), c.a))
            nameEnd = args.size() - 1;
        if (c.a < 0.0f)
            return bad("walktomonster <monster name> [stopShort >= 0]");
        c.verb = CombatVerb::WalkToMonster;
        c.name = join(args.subspan(0, nameEnd));
        return c;
    }
    return std::nullopt; // not a combat verb
}

} // namespace vo::client::world
