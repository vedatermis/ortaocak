// Combat rules shared by client and server (docs/PROTOCOL_COMBAT.md, protocol v3): class tables,
// stat derivation (level + equipped items), the damage / critical / experience formulas, the skill
// table and the level curve. Everything here is pure: randomness is injected (the caller passes the
// rolls, or a generator to rollDamage), so the server's maths can be unit-tested and the client can
// show exactly the numbers the server uses.
//
//   const Stats s = deriveStats(charClass, level, equipmentBonus(catalog, inventory));
//   const Hit hit = rollDamage(s.attack, skill.multiplier, target.defense, s.critChance, rng);
//   const std::uint32_t xp = xpForKill(monster.xp, playerLevel, monster.level);
//
// Tuning target (§1.1): a level-1 character with its starter kit kills a level-1 Kül Çömezi in 4-6
// basic attacks and survives about 10 of its hits (see tests/game/CombatTests.cpp, which checks it
// against gamedata/items.toml and gamedata/monsters.toml).
#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string_view>

namespace vo::game {

class ItemCatalog;
class Inventory;

// ---- Level curve (§1.1) ----

inline constexpr std::uint16_t kLevelCap = 10;

// Experience needed from `level` to the next one: 80 * level^1.6 rounded to 10; 0 at the cap (and
// for level 0).
[[nodiscard]] std::uint32_t xpToNext(std::uint16_t level) noexcept;
// Experience a kill of a level `monsterLevel` monster with `monsterXp` gives a level `playerLevel`
// player: full up to 2 levels above the monster, then 20 % less per further level, 0 from 7 levels
// above. 0 at the level cap.
[[nodiscard]] std::uint32_t xpForKill(std::uint32_t monsterXp, std::uint16_t playerLevel,
                                      std::uint16_t monsterLevel) noexcept;

struct LevelProgress
{
    std::uint16_t level = 1;
    std::uint32_t xp = 0; // towards the next level (0 at the cap)
};
// Adds `gained` experience; carries over into as many level-ups as it covers, stops at the cap
// (where xp stays 0). Returns the new state.
[[nodiscard]] LevelProgress addExperience(LevelProgress p, std::uint32_t gained) noexcept;

// ---- Classes (§1.1) ----

struct ClassTable
{
    std::uint8_t charClass = 0;
    std::uint32_t baseHp = 0, hpPerLevel = 0;
    std::uint32_t baseMp = 0, mpPerLevel = 0;
    std::uint32_t baseAttack = 0, attackPerLevel = 0;
    std::uint32_t baseDefense = 0, defensePerLevel = 0;
    float attackRange = 2.5f;              // metres (horizontal)
    std::uint32_t attackIntervalMs = 1600; // basic attack
    float critChance = 0.05f;
    std::uint16_t skillId = 0; // the class skill (M9: one per class)
};

// nullptr for an unknown class.
[[nodiscard]] const ClassTable* classTable(std::uint8_t charClass) noexcept;

struct EquipBonus
{
    std::int32_t attack = 0;
    std::int32_t defense = 0;
    bool operator==(const EquipBonus&) const = default;
};

// Sum of `attack` / `defense` of every item in the Equipment container whose template is known
// (hidden items count as nothing).
[[nodiscard]] EquipBonus equipmentBonus(const ItemCatalog& catalog, const Inventory& inv) noexcept;

struct Stats
{
    std::uint32_t maxHp = 1;
    std::uint32_t maxMp = 0;
    std::uint16_t attack = 0;
    std::uint16_t defense = 0;
    float attackRange = 2.5f;
    std::uint32_t attackIntervalMs = 1600;
    float critChance = 0.05f;
    bool operator==(const Stats&) const = default;
};

// §1.1 formulas; an unknown class gets warrior numbers. Level is clamped to 1..kLevelCap.
[[nodiscard]] Stats deriveStats(std::uint8_t charClass, std::uint16_t level, EquipBonus equip = {}) noexcept;

// ---- Damage (§1.1) ----

inline constexpr float kDamageVarianceMin = 0.9f;
inline constexpr float kDamageVarianceMax = 1.1f;
inline constexpr float kDefenseFactor = 0.5f;
inline constexpr float kCritMultiplier = 1.5f;

struct Hit
{
    std::uint32_t amount = 1;
    bool critical = false;
};

// max(1, round((attack * multiplier * variance - defense * 0.5) * (critical ? 1.5 : 1))) with
// variance = 0.9 + 0.2 * varianceRoll and critical = critRoll < critChance (rolls in [0, 1)).
[[nodiscard]] Hit computeDamage(std::uint32_t attack, float multiplier, std::uint32_t defense, float critChance,
                                float varianceRoll, float critRoll) noexcept;

template <class Rng>
[[nodiscard]] Hit rollDamage(std::uint32_t attack, float multiplier, std::uint32_t defense, float critChance, Rng& rng)
{
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float variance = u(rng);
    const float crit = u(rng);
    return computeDamage(attack, multiplier, defense, critChance, variance, crit);
}

// ---- Regeneration (§1.1) ----

inline constexpr std::chrono::milliseconds kOutOfCombatAfter{6000};
inline constexpr float kHpRegenPerSecond = 0.02f; // of max_hp
inline constexpr float kMpRegenPerSecond = 0.03f; // of max_mp
inline constexpr float kSittingRegenFactor = 2.0f;

// Amount regenerated per second out of combat (fractional; the server accumulates it).
[[nodiscard]] constexpr float hpRegenPerSecond(std::uint32_t maxHp, bool sitting) noexcept
{
    return static_cast<float>(maxHp) * kHpRegenPerSecond * (sitting ? kSittingRegenFactor : 1.0f);
}
[[nodiscard]] constexpr float mpRegenPerSecond(std::uint32_t maxMp, bool sitting) noexcept
{
    return static_cast<float>(maxMp) * kMpRegenPerSecond * (sitting ? kSittingRegenFactor : 1.0f);
}

// ---- Death / respawn / timing (§1.1, §1.2) ----

inline constexpr std::chrono::milliseconds kRespawnMinDelay{5000};  // RESPAWN_REQ earlier: TooEarly
inline constexpr std::chrono::milliseconds kMonsterCorpseTime{3000}; // dead monster despawns after
inline constexpr std::chrono::milliseconds kGlobalCooldown{500};     // shared by attacks and skills
inline constexpr float kRangeTolerance = 0.5f;                      // metres on top of every range
inline constexpr float kRespawnHpFraction = 0.5f;                   // hp and mp after a respawn

// ---- Skills (§1.2) ----

enum class SkillTarget : std::uint8_t
{
    Enemy,          // a monster
    SelfOrFriendly, // the caster (target 0) or any player
};

struct SkillDef
{
    std::uint16_t id = 0;
    std::string_view name; // UTF-8, Turkish
    std::uint8_t charClass = 0;
    SkillTarget target = SkillTarget::Enemy;
    float range = 2.5f; // metres
    std::uint32_t mpCost = 0;
    std::uint32_t cooldownMs = 0;
    std::uint32_t castMs = 0; // 0 = instant (no CAST_START / CAST_END)
    float multiplier = 1.0f;  // damage skills: attack multiplier per hit
    std::uint8_t hits = 1;    // damage skills: number of hits (each can crit)
    float healFraction = 0.0f; // heal skills: fraction of the target's max_hp
    bool projectile = false;   // presentation only (the server applies the effect at cast end)
};

inline constexpr std::uint16_t kSkillHeavyBlow = 1;  // Ağır Darbe (Warrior)
inline constexpr std::uint16_t kSkillTwinCut = 2;    // Çifte Kesik (Rogue)
inline constexpr std::uint16_t kSkillEmberBolt = 3;  // Köz Oku (Mage)
inline constexpr std::uint16_t kSkillPaleSun = 4;    // Soluk Güneş (Priest)
inline constexpr std::uint16_t kMaxSkillId = 4;

// nullptr for an unknown id.
[[nodiscard]] const SkillDef* findSkill(std::uint16_t id) noexcept;
// The skill of a class (nullptr if none).
[[nodiscard]] const SkillDef* classSkill(std::uint8_t charClass) noexcept;

// Heal amount of a heal skill on a target with `targetMaxHp` (at least 1).
[[nodiscard]] std::uint32_t healAmount(const SkillDef& skill, std::uint32_t targetMaxHp) noexcept;

// ---- Helpers ----

[[nodiscard]] inline float horizontalDistance(float ax, float az, float bx, float bz) noexcept
{
    return std::hypot(ax - bx, az - bz);
}
// Whether a target at `distance` is within `range` (+ the 0.5 m tolerance).
[[nodiscard]] constexpr bool inRange(float distance, float range) noexcept
{
    return distance <= range + kRangeTolerance;
}

} // namespace vo::game
