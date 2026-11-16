// Client-side combat presentation logic (M9 "İlk Kan", docs/PROTOCOL_COMBAT.md §5), no D3D, no
// sockets: everything here is pure and unit-tested (tests/client/CombatViewTests.cpp). The server
// stays authoritative; these helpers only decide what the client shows and which request it sends.
//
//   levelTint()          name label colour by the level difference (grey / white / yellow / orange / red)
//   rayHitsCapsule()     mouse picking of an entity (vertical capsule around its body)
//   pickEntity()         nearest entity hit by a ray
//   nextTabTarget()      Tab: cycle the monsters nearest first
//   attackClipFor()      which clip a basic attack plays (main-hand grip; monsters alternate punches)
//   SkillTimers          the local skill's cooldown, the shared global cooldown and the cast bar,
//                        driven by the server's COMBAT_EVENT / CAST_START / CAST_END
//   FloatingNumbers      damage / heal numbers rising over a target, with their lifetime and style
//   actionFailText()     ACTION_FAIL reasons as short Turkish messages
//   parseCombatCommand() the combat `scene` automation commands (syntax errors name the problem)
#pragma once

#include "client/world/Animator.h"
#include "client/world/WorldMath.h"
#include "shared/game/Items.h"
#include "shared/net/Opcodes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::client::world {

// ---- Name label colour by level difference ----

enum class LevelTint : std::uint8_t
{
    Trivial, // 3+ levels below the player (reduced experience): grey
    Even,    // 2 below .. equal: white
    Higher,  // 1-2 above: yellow
    Hard,    // 3-4 above: orange
    Deadly,  // 5+ above: red
};
[[nodiscard]] LevelTint levelTint(std::uint16_t playerLevel, std::uint16_t monsterLevel) noexcept;
[[nodiscard]] std::uint32_t levelTintRgb(LevelTint t) noexcept; // 0xRRGGBB
[[nodiscard]] const char* levelTintName(LevelTint t) noexcept;

// ---- Picking ----

// Distance along the ray (>= 0) at which it enters the vertical capsule standing on `feet`
// (radius `radius`, total height `height`), or nullopt.
[[nodiscard]] std::optional<float> rayHitsCapsule(const Ray& ray, Vec3 feet, float radius, float height) noexcept;

struct PickCandidate
{
    std::uint32_t id = 0;
    Vec3 feet{};
    float radius = 0.45f;
    float height = 1.8f;
};
// The candidate whose capsule the ray enters first within `maxDistance`; 0 if none.
[[nodiscard]] std::uint32_t pickEntity(const Ray& ray, std::span<const PickCandidate> candidates,
                                       float maxDistance) noexcept;

// ---- Tab targeting ----

struct TabCandidate
{
    std::uint32_t id = 0;
    float distance = 0.0f;
};
inline constexpr float kTabRange = 40.0f; // metres
// Candidates in [0, kTabRange] ordered by distance (ties by id); the one after `current` in that
// order (wrapping), the nearest if `current` is not among them; 0 if there is none.
[[nodiscard]] std::uint32_t nextTabTarget(std::span<const TabCandidate> candidates, std::uint32_t current);

// ---- Attack animation ----

// Basic attack clip: sword_attack for a sword / dagger / mace / two-handed axe in the main hand,
// spell_shoot for a staff, punch_jab / punch_cross alternating (by `swingIndex`) bare-handed and
// for monsters.
[[nodiscard]] AnimState attackClipFor(const game::ItemTemplate* mainHand, bool monster, std::uint32_t swingIndex) noexcept;

// ---- Skill cooldown / cast state machine ----

class SkillTimers
{
public:
    void reset() noexcept { *this = SkillTimers{}; }
    // An own basic swing or skill hit was reported (COMBAT_EVENT source = us): the global cooldown.
    void onAction(double now) noexcept;
    // An own skill took effect: instant skills on their (first) COMBAT_EVENT, cast skills on CAST_END
    // completed. Starts its cooldown (and the global one). Repeated events within the cooldown (the
    // second hit of Çifte Kesik) do not restart it.
    void onSkillUsed(std::uint16_t skillId, double now) noexcept;
    void onCastStart(std::uint16_t skillId, std::uint32_t castMs, double now) noexcept;
    // CAST_END: `completed` starts the cooldown; interrupted costs nothing.
    void onCastEnd(std::uint16_t skillId, bool completed, double now) noexcept;
    // Death / respawn / leaving: no cast in progress.
    void cancelCast() noexcept { m_castSkill = 0; }

    // Seconds until the skill can be used (its own cooldown or the global one, whichever is longer).
    [[nodiscard]] double remaining(std::uint16_t skillId, double now) const noexcept;
    // remaining / the duration of whichever cooldown is binding (0 = ready, 1 = just started).
    [[nodiscard]] float sweep(std::uint16_t skillId, double now) const noexcept;
    [[nodiscard]] bool ready(std::uint16_t skillId, double now) const noexcept { return remaining(skillId, now) <= 0.0; }
    [[nodiscard]] bool casting() const noexcept { return m_castSkill != 0; }
    [[nodiscard]] std::uint16_t castSkill() const noexcept { return m_castSkill; }
    // 0..1 over the cast time (1 while the server's CAST_END has not come yet).
    [[nodiscard]] float castProgress(double now) const noexcept;

private:
    std::uint16_t m_skill = 0; // the skill whose cooldown runs
    double m_skillStart = -1e9;
    double m_skillEnd = -1e9;
    double m_gcdStart = -1e9;
    double m_gcdEnd = -1e9;
    std::uint16_t m_castSkill = 0;
    double m_castStart = 0.0;
    double m_castEnd = 0.0;
};

// ---- Floating combat numbers ----

enum class FloatKind : std::uint8_t
{
    Damage,      // dealt by anyone to someone else: white
    Critical,    // a critical hit: larger, yellow
    Heal,        // green, with a leading +
    TakenDamage, // damage the local player takes: red
    Miss,        // M11: a dodged monster attack (COMBAT_EVENT kind Miss): "Iska", muted grey, no number
};
// The floating text of a number: "+12" (heal), "34!" (critical), "Iska" (miss), else "12".
[[nodiscard]] std::string floatText(FloatKind kind, std::uint32_t amount);
// COMBAT_EVENT kind / flags + whether the local player is the target -> how the number is drawn.
[[nodiscard]] FloatKind classifyCombatNumber(std::uint8_t kind, std::uint8_t flags, bool targetIsLocal) noexcept;

struct FloatStyle
{
    std::uint32_t rgb = 0xFFFFFF;
    float fontScale = 1.0f; // relative to the base number size
    float lifetime = 1.1f;  // seconds
    float rise = 1.2f;      // metres risen over the lifetime
};
[[nodiscard]] FloatStyle floatStyle(FloatKind kind) noexcept;

struct FloatingNumber
{
    Vec3 anchor{};      // world point above the target's head when spawned
    float offsetX = 0.0f; // pixels (spreads simultaneous numbers)
    std::uint32_t amount = 0;
    FloatKind kind = FloatKind::Damage;
    float age = 0.0f;
};

class FloatingNumbers
{
public:
    static constexpr std::size_t kMax = 48; // oldest dropped beyond this

    void add(FloatKind kind, std::uint32_t amount, Vec3 anchor);
    // Ages every number and drops the expired ones.
    void update(float dt);
    void clear() noexcept { m_items.clear(); }
    [[nodiscard]] std::span<const FloatingNumber> items() const noexcept { return m_items; }
    // Alpha (fade in quickly, hold, fade out over the last third) and the rise in metres.
    [[nodiscard]] static float alpha(const FloatingNumber& n) noexcept;
    [[nodiscard]] static float rise(const FloatingNumber& n) noexcept;
    [[nodiscard]] std::uint32_t added() const noexcept { return m_added; }

private:
    std::vector<FloatingNumber> m_items;
    std::uint32_t m_added = 0;
};

// ---- Messages ----

// Short Turkish text for an ACTION_FAIL (e.g. "Menzil dışında", "Yeterli mana yok").
[[nodiscard]] std::string_view actionFailText(vo::net::CombatAction action, vo::net::ActionFailReason reason) noexcept;

// ---- Automation (the combat `scene` commands of WorldScene) ----

enum class CombatVerb : std::uint8_t
{
    Target,          // target nearest [<name>] | target none | target player <name> | target self
    Tab,             // tab: as the Tab key
    Attack,          // attack: as the F key (auto-attack the target, walking into range)
    AttackStop,      // attackstop
    Cast,            // cast: as the 1 key
    Respawn,         // respawn: as the "Yeniden Doğ" button
    Stance,          // stance sit|stand
    ExpectHp,        // expecthp <minPct> <maxPct>   own hp in percent of max
    ExpectLevel,     // expectlevel <n>
    ExpectTargetHp,  // expecttargethp <minPct> <maxPct>
    ExpectCombatMsg, // expectcombatmessage <text>
    WalkToMonster,   // walktomonster <name> [stopShort]: path-found walk next to the nearest such monster
    Hunt,            // hunt <name> <level>: fight the nearest such monsters (resting between fights)
                     // until the character reaches <level>; hunt stop ends it
    Log,             // combatlog: write the combat state to the log
};

struct CombatCommand
{
    CombatVerb verb = CombatVerb::Log;
    std::string mode; // target: nearest / none / player / self; stance: sit / stand
    std::string name; // monster / player name (may contain spaces and Turkish letters)
    float a = 0.0f;   // percent / level / stop distance
    float b = 0.0f;
};

// Parses one combat scene command. Returns nullopt with `error` empty when the verb is not a combat
// verb (the scene tries its other commands), nullopt with a message for a malformed combat command.
[[nodiscard]] std::optional<CombatCommand> parseCombatCommand(std::string_view line, std::string& error);

} // namespace vo::client::world
