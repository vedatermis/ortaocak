// Combat rules shared by client and server (docs/PROTOCOL_COMBAT.md, protocol v3; attributes and
// stat points docs/PROTOCOL_STATS.md, v5): class tables and starting spreads, stat derivation
// (class, level, allocated points, equipped items), the damage / critical / dodge / experience
// formulas, the skill table and the level curve. Everything here is pure: randomness is injected
// (the caller passes the rolls, or a generator to rollDamage), so the server's maths can be
// unit-tested and the client can show (and preview) exactly the numbers the server uses.
//
//   const Stats s = deriveStats(charClass, level, allocated, equipmentBonus(catalog, inventory));
//   const Hit hit = rollDamage(s.attack, skill.multiplier, target.defense, s.critChance, rng);
//   const std::uint32_t xp = xpForKill(monster.xp, playerLevel, monster.level);
//
// Tuning target (§1.1): a level-1 character with its starter kit kills a level-1 Kül Çömezi in 4-6
// basic attacks and survives about 10 of its hits (see tests/game/CombatTests.cpp, which checks it
// against gamedata/items.toml and gamedata/monsters.toml).
#pragma once

#include "shared/game/Skills.h"
#include "shared/net/Opcodes.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
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

// ---- Attributes and stat points (docs/PROTOCOL_STATS.md §1) ----

enum class Attribute : std::uint8_t
{
    Strength = 0,     // Güç
    Dexterity = 1,    // Çeviklik
    Vitality = 2,     // Dayanıklılık
    Intelligence = 3, // Zekâ
    Spirit = 4,       // İrade
};
inline constexpr std::size_t kAttributeCount = 5;
// One value per attribute, in Attribute order (the wire and database order too).
using Attributes = std::array<std::uint16_t, kAttributeCount>;
inline constexpr std::uint16_t kAttributeCap = 255;        // class start + allocated
inline constexpr std::uint16_t kPointsPerLevel = 3;        // free points per level-up
inline constexpr std::uint16_t kMaxItemAttributeBonus = 50; // items.toml limit per attribute

[[nodiscard]] constexpr std::size_t attributeIndex(Attribute a) noexcept { return static_cast<std::size_t>(a); }
// "strength", "dexterity", "vitality", "intelligence", "spirit" (items.toml keys, tools).
[[nodiscard]] std::string_view attributeKey(Attribute a) noexcept;
// "Güç", "Çeviklik", "Dayanıklılık", "Zekâ", "İrade".
[[nodiscard]] std::string_view attributeDisplayName(Attribute a) noexcept;

// Points a character of `level` (clamped to 1..kLevelCap) has to spend in total: 3 x (level - 1).
[[nodiscard]] std::uint32_t pointsForLevel(std::uint16_t level) noexcept;
[[nodiscard]] std::uint32_t allocatedTotal(const Attributes& allocated) noexcept;
// max(0, pointsForLevel(level) - allocatedTotal(allocated)).
[[nodiscard]] std::uint16_t unspentPoints(std::uint16_t level, const Attributes& allocated) noexcept;

// ---- Classes (PROTOCOL_COMBAT.md §1.1, PROTOCOL_STATS.md §1.1, §2) ----

struct ClassTable
{
    std::uint8_t charClass = 0;
    std::int32_t baseHp = 0, hpPerLevel = 0;
    std::int32_t baseMp = 0, mpPerLevel = 0;
    std::int32_t baseAttack = 0, attackPerLevel = 0;
    std::int32_t baseDefense = 0, defensePerLevel = 0;
    float attackRange = 2.5f;              // metres (horizontal)
    std::uint32_t attackIntervalMs = 1600; // basic attack
    std::uint16_t baseCritPermille = 50;   // before Çeviklik
    std::uint16_t skillId = 0;             // the class skill (M9: one per class)
    Attributes start{};                    // starting spread (sum 35)
    Attribute attackAttribute = Attribute::Strength; // scales the attack
};

// nullptr for an unknown class.
[[nodiscard]] const ClassTable* classTable(std::uint8_t charClass) noexcept;
// The starting spread of a class (an unknown class gets the Warrior's).
[[nodiscard]] Attributes classStartAttributes(std::uint8_t charClass) noexcept;
// Class start + allocated (the "base" values of PROTOCOL_STATS.md §1), each at most 65535.
[[nodiscard]] Attributes baseAttributes(std::uint8_t charClass, const Attributes& allocated) noexcept;

// Decides STAT_ALLOCATE_REQ(points) against the committed allocation (PROTOCOL_STATS.md §3.3,
// checks 1-3; the caller handles the queue and the write).
[[nodiscard]] net::StatAllocateResult checkAllocation(std::uint8_t charClass, std::uint16_t level,
                                                      const Attributes& allocated, const Attributes& points) noexcept;

struct EquipBonus
{
    std::int32_t attack = 0;
    std::int32_t defense = 0;
    Attributes attributes{}; // M11: attribute bonuses of the equipped items
    bool operator==(const EquipBonus&) const = default;
};

// Sum of `attack` / `defense` / attribute bonuses of every item in the Equipment container whose
// template is known (hidden items count as nothing).
[[nodiscard]] EquipBonus equipmentBonus(const ItemCatalog& catalog, const Inventory& inv) noexcept;

struct Stats
{
    std::uint32_t maxHp = 1;
    std::uint32_t maxMp = 0;
    std::uint16_t attack = 0; // a Mage's magic power
    std::uint16_t defense = 0;
    float attackRange = 2.5f;
    std::uint32_t attackIntervalMs = 1600;
    float critChance = 0.05f;          // critPermille / 1000
    float dodgeChance = 0.0f;          // dodgePermille / 1000
    std::uint16_t critPermille = 50;   // capped at 500
    std::uint16_t dodgePermille = 0;   // capped at 250
    float hpRegen = 0.0f;              // per second out of combat, standing
    float mpRegen = 0.0f;
    float healMultiplier = 1.0f;       // heal skills: 1 + (INT + SPI) / 60
    Attributes base{};                 // class start + allocated
    Attributes bonus{};                // from the equipment
    std::uint16_t unspent = 0;         // stat points not spent yet
    bool operator==(const Stats&) const = default;

    [[nodiscard]] std::uint32_t effective(Attribute a) const noexcept
    {
        return static_cast<std::uint32_t>(base[attributeIndex(a)]) + bonus[attributeIndex(a)];
    }
};

// PROTOCOL_STATS.md §2 formulas; an unknown class gets warrior numbers. Level is clamped to
// 1..kLevelCap. `allocated` = the points the player spent per attribute.
[[nodiscard]] Stats deriveStats(std::uint8_t charClass, std::uint16_t level, const Attributes& allocated,
                                EquipBonus equip) noexcept;
// Nothing allocated (the class's starting spread).
[[nodiscard]] Stats deriveStats(std::uint8_t charClass, std::uint16_t level, EquipBonus equip = {}) noexcept;

inline constexpr std::uint16_t kMaxCritPermille = 500;  // 50 %
inline constexpr std::uint16_t kMaxDodgePermille = 250; // 25 %

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
inline constexpr float kHpRegenPerSecond = 0.02f; // of max_hp, times (1 + VIT / 100)
inline constexpr float kMpRegenPerSecond = 0.03f; // of max_mp, times (1 + SPI / 100)
inline constexpr float kSittingRegenFactor = 2.0f;

// Amount regenerated per second out of combat (fractional; the server accumulates it).
[[nodiscard]] constexpr float hpRegenPerSecond(const Stats& s, bool sitting) noexcept
{
    return s.hpRegen * (sitting ? kSittingRegenFactor : 1.0f);
}
[[nodiscard]] constexpr float mpRegenPerSecond(const Stats& s, bool sitting) noexcept
{
    return s.mpRegen * (sitting ? kSittingRegenFactor : 1.0f);
}

// ---- Death / respawn / timing (§1.1, §1.2) ----

inline constexpr std::chrono::milliseconds kRespawnMinDelay{5000};  // RESPAWN_REQ earlier: TooEarly
inline constexpr std::chrono::milliseconds kMonsterCorpseTime{3000}; // dead monster despawns after
inline constexpr std::chrono::milliseconds kGlobalCooldown{500};     // shared by attacks and skills
inline constexpr float kRangeTolerance = 0.5f;                      // metres on top of every range
inline constexpr float kRespawnHpFraction = 0.5f;                   // hp and mp after a respawn

// ---- Skills (§1.2; v6: gamedata/skills.toml, shared/game/Skills.h, docs/PROTOCOL_SKILLS.md) ----

// The class (free) skills of M9; every other skill only exists in the data file.
inline constexpr std::uint16_t kSkillHeavyBlow = 1;  // Ağır Darbe (Warrior)
inline constexpr std::uint16_t kSkillTwinCut = 2;    // Çifte Kesik (Rogue)
inline constexpr std::uint16_t kSkillEmberBolt = 3;  // Köz Oku (Mage)
inline constexpr std::uint16_t kSkillPaleSun = 4;    // Soluk Güneş (Priest)

// In the process-wide catalog (game::skills()); nullptr for an unknown id.
[[nodiscard]] const SkillDef* findSkill(std::uint16_t id) noexcept;
// The free skill of a class (nullptr if none).
[[nodiscard]] const SkillDef* classSkill(std::uint8_t charClass) noexcept;

// Heal amount of a heal skill at `rank` cast with `healMultiplier` (the caster's
// Stats::healMultiplier) on a target with `targetMaxHp`: max(1, round(targetMaxHp x healFraction x
// healMultiplier)).
[[nodiscard]] std::uint32_t healAmount(const SkillDef& skill, std::uint32_t targetMaxHp,
                                       float healMultiplier = 1.0f, std::uint8_t rank = 1) noexcept;
// Heal strength of a heal skill in 1/1000 of the target's max hp (CHAR_STATS.heal_permille).
[[nodiscard]] std::uint16_t healPermille(const SkillDef& skill, float healMultiplier, std::uint8_t rank = 1) noexcept;

// ---- Timed effects on stats (PROTOCOL_SKILLS.md §1.5) ----

inline constexpr std::uint16_t kMaxDodgeWithEffectsPermille = 500; // dodge_up can lift dodge to 50 %
inline constexpr std::uint32_t kMinAttackIntervalMs = 200;

// Sums of the active stat-changing effects of a player, in percent (dodge: percentage points).
struct EffectModifiers
{
    float attackPct = 0.0f;
    float defensePct = 0.0f;
    float hastePct = 0.0f;
    float dodgePct = 0.0f;
    float maxHpPct = 0.0f;
    bool operator==(const EffectModifiers&) const = default;
    [[nodiscard]] bool none() const noexcept { return *this == EffectModifiers{}; }
};

// The derived stats with the modifiers applied: attack / defense / max hp x (1 + pct / 100)
// (rounded), attack interval / (1 + haste / 100) (at least 200 ms), dodge + pct (at most 50 %).
[[nodiscard]] Stats applyEffectModifiers(Stats s, const EffectModifiers& m) noexcept;

// Damage-over-time tick of an effect with `valuePct` cast with `attack`: max(1, round(attack x pct / 100)).
[[nodiscard]] std::uint32_t periodicDamage(std::uint32_t attack, float valuePct) noexcept;
// Heal-over-time tick: max(1, round(targetMaxHp x pct / 100 x healMultiplier)).
[[nodiscard]] std::uint32_t periodicHeal(std::uint32_t targetMaxHp, float valuePct, float healMultiplier) noexcept;
// Absorb shield: max(1, round(attack x pct / 100)).
[[nodiscard]] std::uint32_t shieldAmount(std::uint32_t attack, float valuePct) noexcept;

// ---- Dodge (PROTOCOL_STATS.md §2.1) ----

// Whether a monster's attack is dodged: dodgeRoll in [0, 1) below the player's dodge chance.
[[nodiscard]] constexpr bool dodged(float dodgeChance, float dodgeRoll) noexcept { return dodgeRoll < dodgeChance; }

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
