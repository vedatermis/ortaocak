#include "shared/game/Combat.h"

#include "shared/game/Character.h"
#include "shared/game/Items.h"

#include <algorithm>
#include <limits>

namespace vo::game {

namespace {

// Level-1 values (base + 1 x per level) with the starter kit (gamedata/items.toml), against a
// level-1 Kül Çömezi (80 hp, attack 11, defense 4):
//   Warrior  90 hp, atk 12+6, def 3+4  -> 16 per hit (5 hits), takes ~7.5 (12 hits)
//   Rogue    98 hp, atk 10+8, def 2    -> 16 per hit (5 hits, 1.2 s, 15 % crit), takes ~10 (10 hits)
//   Mage     90 hp, atk 11+7, def 2    -> 16 per hit (5 hits, from 14 m), takes ~10 (9 hits)
//   Priest   95 hp, atk 11+5, def 3    -> 14 per hit (6 hits), takes ~9.5 (10 hits)
constexpr std::array<ClassTable, 4> kClasses = {{
    {static_cast<std::uint8_t>(CharClass::Warrior), 70, 20, 30, 4, 10, 2, 2, 1, 2.5f, 1600, 0.05f, kSkillHeavyBlow},
    {static_cast<std::uint8_t>(CharClass::Rogue), 80, 18, 30, 5, 8, 2, 1, 1, 2.5f, 1200, 0.15f, kSkillTwinCut},
    {static_cast<std::uint8_t>(CharClass::Mage), 76, 14, 60, 10, 9, 2, 1, 1, 14.0f, 1800, 0.05f, kSkillEmberBolt},
    {static_cast<std::uint8_t>(CharClass::Priest), 78, 17, 55, 9, 9, 2, 2, 1, 2.5f, 1700, 0.05f, kSkillPaleSun},
}};

constexpr std::array<SkillDef, 4> kSkills = {{
    {kSkillHeavyBlow, "Ağır Darbe", static_cast<std::uint8_t>(CharClass::Warrior), SkillTarget::Enemy, 2.5f, 12, 6000,
     0, 2.2f, 1, 0.0f, false},
    {kSkillTwinCut, "Çifte Kesik", static_cast<std::uint8_t>(CharClass::Rogue), SkillTarget::Enemy, 2.5f, 10, 5000, 0,
     1.2f, 2, 0.0f, false},
    {kSkillEmberBolt, "Köz Oku", static_cast<std::uint8_t>(CharClass::Mage), SkillTarget::Enemy, 16.0f, 14, 3000, 600,
     2.0f, 1, 0.0f, true},
    {kSkillPaleSun, "Soluk Güneş", static_cast<std::uint8_t>(CharClass::Priest), SkillTarget::SelfOrFriendly, 14.0f,
     16, 4000, 800, 0.0f, 1, 0.25f, false},
}};

std::uint16_t clampU16(std::int64_t v) noexcept
{
    return static_cast<std::uint16_t>(std::clamp<std::int64_t>(v, 0, std::numeric_limits<std::uint16_t>::max()));
}

} // namespace

std::uint32_t xpToNext(std::uint16_t level) noexcept
{
    if (level == 0 || level >= kLevelCap)
        return 0;
    const double raw = 80.0 * std::pow(static_cast<double>(level), 1.6);
    return static_cast<std::uint32_t>(std::floor(raw / 10.0 + 0.5)) * 10u;
}

std::uint32_t xpForKill(std::uint32_t monsterXp, std::uint16_t playerLevel, std::uint16_t monsterLevel) noexcept
{
    if (playerLevel >= kLevelCap)
        return 0;
    const int above = static_cast<int>(playerLevel) - static_cast<int>(monsterLevel);
    if (above <= 2)
        return monsterXp;
    if (above >= 7)
        return 0;
    const int percent = 100 - 20 * (above - 2); // 3 -> 80 %, ..., 6 -> 20 %
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(monsterXp) * static_cast<std::uint64_t>(percent)) / 100u);
}

LevelProgress addExperience(LevelProgress p, std::uint32_t gained) noexcept
{
    p.level = std::clamp<std::uint16_t>(p.level, 1, kLevelCap);
    if (p.level >= kLevelCap)
    {
        p.xp = 0;
        return p;
    }
    std::uint64_t xp = static_cast<std::uint64_t>(p.xp) + gained;
    while (p.level < kLevelCap)
    {
        const std::uint32_t need = xpToNext(p.level);
        if (xp < need)
            break;
        xp -= need;
        ++p.level;
    }
    p.xp = p.level >= kLevelCap ? 0u : static_cast<std::uint32_t>(xp);
    return p;
}

const ClassTable* classTable(std::uint8_t charClass) noexcept
{
    for (const auto& c : kClasses)
        if (c.charClass == charClass)
            return &c;
    return nullptr;
}

EquipBonus equipmentBonus(const ItemCatalog& catalog, const Inventory& inv) noexcept
{
    EquipBonus b;
    for (std::size_t s = 0; s < kEquipmentSlots; ++s)
    {
        const ItemStack& st = inv.equipped(static_cast<EquipSlot>(s));
        if (st.empty())
            continue;
        if (const ItemTemplate* t = catalog.find(st.templateId))
        {
            b.attack += t->attack;
            b.defense += t->defense;
        }
    }
    return b;
}

Stats deriveStats(std::uint8_t charClass, std::uint16_t level, EquipBonus equip) noexcept
{
    const ClassTable* c = classTable(charClass);
    if (!c)
        c = &kClasses[0];
    const std::uint32_t l = std::clamp<std::uint16_t>(level, 1, kLevelCap);
    Stats s;
    s.maxHp = c->baseHp + l * c->hpPerLevel;
    s.maxMp = c->baseMp + l * c->mpPerLevel;
    s.attack = clampU16(static_cast<std::int64_t>(c->baseAttack + l * c->attackPerLevel) + equip.attack);
    s.defense = clampU16(static_cast<std::int64_t>(c->baseDefense + l * c->defensePerLevel) + equip.defense);
    s.attackRange = c->attackRange;
    s.attackIntervalMs = c->attackIntervalMs;
    s.critChance = c->critChance;
    return s;
}

Hit computeDamage(std::uint32_t attack, float multiplier, std::uint32_t defense, float critChance, float varianceRoll,
                  float critRoll) noexcept
{
    const float v = kDamageVarianceMin + (kDamageVarianceMax - kDamageVarianceMin) * std::clamp(varianceRoll, 0.0f, 1.0f);
    Hit h;
    h.critical = critRoll < critChance;
    float raw = static_cast<float>(attack) * multiplier * v - static_cast<float>(defense) * kDefenseFactor;
    if (h.critical)
        raw *= kCritMultiplier;
    const float rounded = std::round(raw);
    h.amount = rounded < 1.0f ? 1u : static_cast<std::uint32_t>(std::min(rounded, 1.0e9f));
    return h;
}

const SkillDef* findSkill(std::uint16_t id) noexcept
{
    for (const auto& s : kSkills)
        if (s.id == id)
            return &s;
    return nullptr;
}

const SkillDef* classSkill(std::uint8_t charClass) noexcept
{
    const ClassTable* c = classTable(charClass);
    return c ? findSkill(c->skillId) : nullptr;
}

std::uint32_t healAmount(const SkillDef& skill, std::uint32_t targetMaxHp) noexcept
{
    const auto v = static_cast<std::uint32_t>(std::lround(static_cast<double>(targetMaxHp) * skill.healFraction));
    return std::max<std::uint32_t>(v, 1);
}

} // namespace vo::game
