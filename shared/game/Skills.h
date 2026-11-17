// Skill data and the skill-point rules shared by client and server (docs/PROTOCOL_SKILLS.md, v6):
// gamedata/skills.toml (rules, branches, skills with per-rank numbers and timed effects), loaded and
// strictly validated like items/monsters/loot; ranks, points, tier gates, the skill bar, and the
// Turkish description text of a skill at a rank.
//
//   game::SkillCatalog catalog;
//   if (!game::loadSkillCatalog(path, catalog, error)) ...
//   const game::SkillDef* s = catalog.find(5);
//   const auto r = game::checkSkillLearn(catalog, charClass, level, ranks, 5);
//
// The process-wide catalog (game::skills(), used by findSkill / classSkill in Combat.h) is the one
// installed with installSkillCatalog() (the server installs the file it validated at startup), or
// else gamedata/skills.toml found like the other data files (core::locateDataFile) on first use.
#pragma once

#include "shared/net/Opcodes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::game {

class ItemCatalog;

inline constexpr std::string_view kSkillsFile = "gamedata/skills.toml";
inline constexpr std::uint8_t kMaxSkillRank = 3;
inline constexpr std::uint16_t kMaxSkillId = 63;      // ids 1..63 (wire and database CHECK)
inline constexpr std::size_t kSkillBarSlots = 8;
inline constexpr std::size_t kMaxEffectsPerEntity = 16;
inline constexpr std::size_t kMaxSkillEffects = 2;    // [[skill.effect]] per skill
inline constexpr std::uint32_t kMaxSkillCooldownMs = 60'000; // the world's relog memory (§1.7)
inline constexpr float kMaxSkillCritChance = 0.85f;

// What SKILL_REQ.target_id must be (PROTOCOL_SKILLS.md §1.4).
enum class SkillTarget : std::uint8_t
{
    Enemy,          // a monster
    SelfOrFriendly, // "friendly": the caster (target 0) or any living player
    Self,           // the caster; target id ignored
};

enum class SkillArea : std::uint8_t
{
    None,
    AroundSelf,
    AroundTarget,
};

// Effect kinds; the value is the wire value (PROTOCOL_SKILLS.md §1.5).
enum class EffectKind : std::uint8_t
{
    AttackUp = 1,
    DefenseUp = 2,
    Haste = 3,
    DodgeUp = 4,
    MaxHpUp = 5,
    Dot = 6,
    Hot = 7,
    Slow = 8,
    Stun = 9,
    Root = 10,
    Shield = 11,
    Weaken = 12,
    Taunt = 13,
};
inline constexpr std::size_t kEffectKindCount = 13;

// Helpful kinds land on players, the others on monsters.
[[nodiscard]] bool isHelpfulEffect(EffectKind k) noexcept;
// "attack_up", ... (skills.toml); nullopt for an unknown name.
[[nodiscard]] std::optional<EffectKind> effectKindFromKey(std::string_view key) noexcept;
[[nodiscard]] std::string_view effectKindKey(EffectKind k) noexcept;

struct EffectDef
{
    EffectKind kind = EffectKind::AttackUp;
    bool onSelf = false; // false: every entity the skill hits
    std::array<std::uint32_t, kMaxSkillRank> durationMs{};
    std::array<float, kMaxSkillRank> value{}; // unit by kind (§1.5)
    std::uint32_t tickMs = 1000;              // dot / hot
};

// The numbers of one rank.
struct SkillRankData
{
    float range = 0.0f;      // metres (0 for self skills)
    std::uint32_t mpCost = 0;
    std::uint32_t cooldownMs = 0;
    std::uint32_t castMs = 0; // 0 = instant
    float multiplier = 0.0f;  // damage per hit (0 = no direct damage)
    float healFraction = 0.0f; // heal of the target's max hp (0 = none)
    float critBonus = 0.0f;   // added to the critical chance (0.25 = +25 %)
};

struct SkillDef
{
    std::uint16_t id = 0;
    std::string key;         // icon key: ui/skills/<key>.png
    std::string name;        // UTF-8, Turkish
    std::string description; // with placeholders (describeSkill)
    std::uint8_t charClass = 0;
    std::uint8_t branch = 0; // 0 or 1
    std::uint8_t tier = 1;   // 1..3
    bool free = false;       // the class's free skill (rank >= 1 always)
    SkillTarget target = SkillTarget::Enemy;
    SkillArea area = SkillArea::None;
    bool affectsAllies = false; // area: allies instead of enemies
    float radius = 0.0f;
    std::uint8_t maxTargets = 1;
    bool leap = false;
    float leapStop = 1.5f;
    std::uint8_t hits = 1;
    bool projectile = false; // presentation only
    std::array<SkillRankData, kMaxSkillRank> ranks{};
    std::vector<EffectDef> effects; // 0..kMaxSkillEffects

    // Rank-1 numbers (the v3..v5 fields; existing code uses them).
    float range = 0.0f;
    std::uint32_t mpCost = 0;
    std::uint32_t cooldownMs = 0;
    std::uint32_t castMs = 0;
    float multiplier = 0.0f;
    float healFraction = 0.0f;

    // The numbers of `rank` (clamped to 1..3).
    [[nodiscard]] const SkillRankData& at(std::uint8_t rank) const noexcept
    {
        const std::size_t i = rank < 1 ? 0 : (rank > kMaxSkillRank ? kMaxSkillRank - 1 : rank - 1u);
        return ranks[i];
    }
    // Whether the skill hits monsters (enemy target, or an area of enemies).
    [[nodiscard]] bool hitsEnemies() const noexcept
    {
        return target == SkillTarget::Enemy || (area != SkillArea::None && !affectsAllies);
    }
    // Whether the skill hits players (friendly / self target without an enemy area, or an ally area).
    [[nodiscard]] bool hitsAllies() const noexcept { return !hitsEnemies(); }
};

struct SkillBranch
{
    std::uint8_t charClass = 0;
    std::uint8_t index = 0; // 0 or 1
    std::string name;       // "Hücum", ...
};

struct SkillRules
{
    std::uint16_t pointsPerLevel = 1;
    std::array<std::uint16_t, kMaxSkillRank> tierPoints{0, 2, 4}; // points spent in the branch per tier
};

class SkillCatalog
{
public:
    [[nodiscard]] const SkillDef* find(std::uint16_t id) const noexcept;
    [[nodiscard]] const std::vector<SkillDef>& skills() const noexcept { return m_skills; } // ascending id
    [[nodiscard]] const std::vector<SkillBranch>& branches() const noexcept { return m_branches; }
    [[nodiscard]] const SkillRules& rules() const noexcept { return m_rules; }
    // The class's free skill (nullptr if none / unknown class).
    [[nodiscard]] const SkillDef* freeSkill(std::uint8_t charClass) const noexcept;
    // Branch name ("" if unknown).
    [[nodiscard]] std::string_view branchName(std::uint8_t charClass, std::uint8_t branch) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return m_skills.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_skills.size(); }

private:
    friend bool parseSkillCatalog(std::string_view, std::string_view, SkillCatalog&, std::string&);
    std::vector<SkillDef> m_skills;
    std::vector<SkillBranch> m_branches;
    SkillRules m_rules;
};

// Parses and validates skills.toml text. False + "<source>:<line>: <problem>" on any problem.
[[nodiscard]] bool parseSkillCatalog(std::string_view tomlText, std::string_view sourceName, SkillCatalog& out,
                                     std::string& error);
[[nodiscard]] bool loadSkillCatalog(const std::filesystem::path& file, SkillCatalog& out, std::string& error);
[[nodiscard]] std::optional<std::filesystem::path> locateSkillsFile();

// ---- The process-wide catalog ----

// The installed catalog, else gamedata/skills.toml loaded on first use (an empty catalog if that
// fails: see defaultSkillCatalogError()). Thread-safe to read; install before other threads use it.
[[nodiscard]] const SkillCatalog& skills() noexcept;
void installSkillCatalog(std::shared_ptr<const SkillCatalog> catalog);
// Empty if the default file loaded (or a catalog was installed).
[[nodiscard]] std::string defaultSkillCatalogError();

// ---- Ranks and points (PROTOCOL_SKILLS.md §1.1, §1.2) ----

// Rank per skill id (index = id, 0..kMaxSkillId).
using SkillRanks = std::array<std::uint8_t, kMaxSkillId + 1>;
using SkillBar = std::array<std::uint16_t, kSkillBarSlots>;

// The free skill of the class at rank 1, everything else 0.
[[nodiscard]] SkillRanks defaultSkillRanks(const SkillCatalog& catalog, std::uint8_t charClass) noexcept;
// Drops unknown and other classes' skills, clamps to 3, raises the free skill to 1.
[[nodiscard]] SkillRanks normalizeSkillRanks(const SkillCatalog& catalog, std::uint8_t charClass,
                                             const SkillRanks& ranks) noexcept;
// points_per_level x (level - 1), level clamped to 1..kLevelCap.
[[nodiscard]] std::uint32_t skillPointsForLevel(const SkillCatalog& catalog, std::uint16_t level) noexcept;
// Σ (rank - base) over the class's skills (base 1 for the free skill).
[[nodiscard]] std::uint32_t skillPointsSpent(const SkillCatalog& catalog, std::uint8_t charClass,
                                             const SkillRanks& ranks) noexcept;
[[nodiscard]] std::uint32_t branchPointsSpent(const SkillCatalog& catalog, std::uint8_t charClass, std::uint8_t branch,
                                              const SkillRanks& ranks) noexcept;
[[nodiscard]] std::uint16_t unspentSkillPoints(const SkillCatalog& catalog, std::uint8_t charClass,
                                               std::uint16_t level, const SkillRanks& ranks) noexcept;
// Decides SKILL_LEARN_REQ(skill) against `ranks` (checks 1-4 of PROTOCOL_SKILLS.md §3.3).
[[nodiscard]] net::SkillLearnResult checkSkillLearn(const SkillCatalog& catalog, std::uint8_t charClass,
                                                    std::uint16_t level, const SkillRanks& ranks,
                                                    std::uint16_t skillId) noexcept;
// Points that must be spent in the skill's branch before it can be learned (0 for tier 1).
[[nodiscard]] std::uint16_t tierGatePoints(const SkillCatalog& catalog, const SkillDef& skill) noexcept;

// ---- The skill bar (PROTOCOL_SKILLS.md §3.5; v8 item slots, PROTOCOL_SHOP.md §4.1) ----
//
// A slot is 0 (empty), a skill id 1..63, or kBarItemFlag | template id (1..0x7FFF) of a consumable.

inline constexpr std::uint16_t kBarItemFlag = net::kBarItemFlag;
[[nodiscard]] constexpr bool isBarItem(std::uint16_t slot) noexcept
{
    return (slot & kBarItemFlag) != 0 && (slot & 0x7FFF) != 0;
}
[[nodiscard]] constexpr std::uint16_t barItemTemplate(std::uint16_t slot) noexcept
{
    return isBarItem(slot) ? static_cast<std::uint16_t>(slot & 0x7FFF) : std::uint16_t{0};
}
[[nodiscard]] constexpr std::uint16_t barItemSlot(std::uint16_t templateId) noexcept
{
    return static_cast<std::uint16_t>(kBarItemFlag | (templateId & 0x7FFF));
}

// The free skill in slot 0, the rest empty.
[[nodiscard]] SkillBar defaultSkillBar(const SkillCatalog& catalog, std::uint8_t charClass) noexcept;
// Whether every non-zero slot is a skill of the class with rank >= 1, or an item slot. Item slots
// must name a consumable of `items` when it is given; without it (the account services, which
// cannot know the item file) any item slot with a template 1..0x7FFF passes.
[[nodiscard]] bool validSkillBar(const SkillCatalog& catalog, std::uint8_t charClass, const SkillRanks& ranks,
                                 const SkillBar& bar, const ItemCatalog* items = nullptr) noexcept;
// Slots that are not valid (as validSkillBar) become 0.
[[nodiscard]] SkillBar sanitizeSkillBar(const SkillCatalog& catalog, std::uint8_t charClass, const SkillRanks& ranks,
                                        const SkillBar& bar, const ItemCatalog* items = nullptr) noexcept;

// The description of `skill` at `rank` (1..3) with its placeholders filled in (Turkish number
// format: decimal comma, ",0" dropped). Unknown placeholders are left as they are.
[[nodiscard]] std::string describeSkill(const SkillDef& skill, std::uint8_t rank);
// "2,5", "12" (one decimal, comma, trailing ",0" dropped).
[[nodiscard]] std::string formatSkillNumber(double v);

} // namespace vo::game
