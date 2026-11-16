#include "shared/game/Combat.h"

#include "shared/game/Character.h"
#include "shared/game/Items.h"

#include <algorithm>
#include <limits>

namespace vo::game {

namespace {

constexpr Attributes attrs(std::uint16_t str, std::uint16_t dex, std::uint16_t vit, std::uint16_t intl,
                           std::uint16_t spi) noexcept
{
    return Attributes{str, dex, vit, intl, spi};
}

// docs/PROTOCOL_STATS.md §1.1 / §2. With the starting spread (sum 35) and the starter kit
// (gamedata/items.toml) every class has exactly its v4 numbers at level 1, against a level-1 Kül
// Çömezi (80 hp, attack 11, defense 4):
//   Warrior  90 hp, atk 18, def 7  -> 16 per hit (5 hits), takes ~7.5 (12 hits)
//   Rogue    98 hp, atk 18, def 2  -> 16 per hit (5 hits, 1.2 s, 15 % crit), takes ~10 (10 hits)
//   Mage     90 hp, atk 18, def 2  -> 16 per hit (5 hits, from 14 m), takes ~10 (9 hits)
//   Priest   95 hp, atk 16, def 3  -> 14 per hit (6 hits), takes ~9.5 (10 hits)
// and also at level 10 with nothing spent (the per-level rates are the v4 ones).
// Columns: class, hp (base, per level), mp, attack, defense, range, interval, crit permille,
// skill, start (STR DEX VIT INT SPI), attack attribute.
constexpr std::array<ClassTable, 4> kClasses = {{
    {static_cast<std::uint8_t>(CharClass::Warrior), 15, 20, 18, 4, 2, 2, 0, 1, 2.5f, 1600, 20, kSkillHeavyBlow,
     attrs(12, 6, 11, 3, 3), Attribute::Strength},
    {static_cast<std::uint8_t>(CharClass::Rogue), 40, 18, 14, 5, -2, 2, -1, 1, 2.5f, 1200, 85, kSkillTwinCut,
     attrs(6, 13, 8, 4, 4), Attribute::Dexterity},
    {static_cast<std::uint8_t>(CharClass::Mage), 46, 14, 23, 10, 0, 2, 0, 1, 14.0f, 1800, 25, kSkillEmberBolt,
     attrs(3, 5, 6, 13, 8), Attribute::Intelligence},
    {static_cast<std::uint8_t>(CharClass::Priest), 33, 17, 17, 9, 6, 2, 0, 1, 2.5f, 1700, 35, kSkillPaleSun,
     attrs(5, 3, 9, 8, 10), Attribute::Strength},
}};

constexpr std::array<SkillDef, 4> kSkills = {{
    {kSkillHeavyBlow, "Ağır Darbe", static_cast<std::uint8_t>(CharClass::Warrior), SkillTarget::Enemy, 2.5f, 12, 6000,
     0, 2.2f, 1, 0.0f, false},
    {kSkillTwinCut, "Çifte Kesik", static_cast<std::uint8_t>(CharClass::Rogue), SkillTarget::Enemy, 2.5f, 10, 5000, 0,
     1.2f, 2, 0.0f, false},
    {kSkillEmberBolt, "Köz Oku", static_cast<std::uint8_t>(CharClass::Mage), SkillTarget::Enemy, 16.0f, 14, 3000, 600,
     2.0f, 1, 0.0f, true},
    // 19 % of the target's max hp x (1 + (INT + SPI) / 60): 24.7 % with the Priest's start (v4: 25 %).
    {kSkillPaleSun, "Soluk Güneş", static_cast<std::uint8_t>(CharClass::Priest), SkillTarget::SelfOrFriendly, 14.0f,
     16, 4000, 800, 0.0f, 1, 0.19f, false},
}};

// Per-attribute coefficients (PROTOCOL_STATS.md §2).
constexpr double kHpPerVitality = 5.0;
constexpr double kMpPerSpirit = 3.0;
constexpr double kMpPerIntelligence = 1.0;
constexpr double kWeaponScaleDivisor = 20.0;      // W x (1 + P / 20)
constexpr double kAttackPerPrimary = 1.0 / 3.0;   // + P / 3
constexpr double kDefensePerVitality = 0.2;       // + VIT / 5
constexpr std::uint32_t kCritPermillePerDex = 5;  // 0.5 % per Çeviklik
constexpr std::uint32_t kDodgePermillePerDex = 4; // 0.4 % per Çeviklik
constexpr double kRegenVitalityDivisor = 100.0;   // hp regen x (1 + VIT / 100)
constexpr double kRegenSpiritDivisor = 100.0;     // mp regen x (1 + SPI / 100)
constexpr double kHealAttributeDivisor = 60.0;    // heal x (1 + (INT + SPI) / 60)

std::uint16_t clampU16(std::int64_t v) noexcept
{
    return static_cast<std::uint16_t>(std::clamp<std::int64_t>(v, 0, std::numeric_limits<std::uint16_t>::max()));
}

const ClassTable& tableOrWarrior(std::uint8_t charClass) noexcept
{
    const ClassTable* c = classTable(charClass);
    return c ? *c : kClasses[0];
}

} // namespace

std::string_view attributeKey(Attribute a) noexcept
{
    switch (a)
    {
    case Attribute::Strength: return "strength";
    case Attribute::Dexterity: return "dexterity";
    case Attribute::Vitality: return "vitality";
    case Attribute::Intelligence: return "intelligence";
    case Attribute::Spirit: return "spirit";
    }
    return "?";
}

std::string_view attributeDisplayName(Attribute a) noexcept
{
    switch (a)
    {
    case Attribute::Strength: return "Güç";
    case Attribute::Dexterity: return "Çeviklik";
    case Attribute::Vitality: return "Dayanıklılık";
    case Attribute::Intelligence: return "Zekâ";
    case Attribute::Spirit: return "İrade";
    }
    return "?";
}

std::uint32_t pointsForLevel(std::uint16_t level) noexcept
{
    const std::uint32_t l = std::clamp<std::uint16_t>(level, 1, kLevelCap);
    return kPointsPerLevel * (l - 1);
}

std::uint32_t allocatedTotal(const Attributes& allocated) noexcept
{
    std::uint32_t sum = 0;
    for (const auto v : allocated)
        sum += v;
    return sum;
}

std::uint16_t unspentPoints(std::uint16_t level, const Attributes& allocated) noexcept
{
    const std::uint32_t points = pointsForLevel(level);
    const std::uint32_t spent = allocatedTotal(allocated);
    return static_cast<std::uint16_t>(spent >= points ? 0u : points - spent);
}

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

Attributes classStartAttributes(std::uint8_t charClass) noexcept
{
    return tableOrWarrior(charClass).start;
}

Attributes baseAttributes(std::uint8_t charClass, const Attributes& allocated) noexcept
{
    const Attributes start = classStartAttributes(charClass);
    Attributes out{};
    for (std::size_t i = 0; i < kAttributeCount; ++i)
        out[i] = clampU16(static_cast<std::int64_t>(start[i]) + allocated[i]);
    return out;
}

net::StatAllocateResult checkAllocation(std::uint8_t charClass, std::uint16_t level, const Attributes& allocated,
                                        const Attributes& points) noexcept
{
    using R = net::StatAllocateResult;
    const std::uint32_t asked = allocatedTotal(points);
    if (asked == 0)
        return R::InvalidAmount;
    if (asked > unspentPoints(level, allocated))
        return R::NotEnoughPoints;
    const Attributes base = baseAttributes(charClass, allocated);
    for (std::size_t i = 0; i < kAttributeCount; ++i)
        if (static_cast<std::uint32_t>(base[i]) + points[i] > kAttributeCap)
            return R::AttributeCap;
    return R::Ok;
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
            for (std::size_t i = 0; i < kAttributeCount; ++i)
                b.attributes[i] = clampU16(static_cast<std::int64_t>(b.attributes[i]) + t->attributes[i]);
        }
    }
    return b;
}

Stats deriveStats(std::uint8_t charClass, std::uint16_t level, const Attributes& allocated, EquipBonus equip) noexcept
{
    const ClassTable& c = tableOrWarrior(charClass);
    const std::int64_t l = std::clamp<std::uint16_t>(level, 1, kLevelCap);
    Stats s;
    s.base = baseAttributes(charClass, allocated);
    s.bonus = equip.attributes;
    s.unspent = unspentPoints(level, allocated);
    const auto eff = [&](Attribute a) { return static_cast<double>(s.effective(a)); };
    const double dex = eff(Attribute::Dexterity), vit = eff(Attribute::Vitality), intl = eff(Attribute::Intelligence),
                 spi = eff(Attribute::Spirit);
    const double primary = eff(c.attackAttribute);

    const double hp = static_cast<double>(c.baseHp + l * c.hpPerLevel) + kHpPerVitality * vit;
    s.maxHp = static_cast<std::uint32_t>(std::clamp<double>(hp, 1.0, 1.0e9));
    const double mp = static_cast<double>(c.baseMp + l * c.mpPerLevel) + kMpPerSpirit * spi + kMpPerIntelligence * intl;
    s.maxMp = static_cast<std::uint32_t>(std::clamp<double>(mp, 0.0, 1.0e9));
    const double attack = static_cast<double>(c.baseAttack + l * c.attackPerLevel) +
                          static_cast<double>(equip.attack) * (1.0 + primary / kWeaponScaleDivisor) +
                          primary * kAttackPerPrimary;
    s.attack = clampU16(std::lround(attack));
    const double defense =
        static_cast<double>(c.baseDefense + l * c.defensePerLevel + equip.defense) + vit * kDefensePerVitality;
    s.defense = clampU16(std::lround(defense));
    s.attackRange = c.attackRange;
    s.attackIntervalMs = c.attackIntervalMs;
    const auto dexPoints = static_cast<std::uint32_t>(dex);
    s.critPermille = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(c.baseCritPermille + kCritPermillePerDex * dexPoints, kMaxCritPermille));
    s.dodgePermille =
        static_cast<std::uint16_t>(std::min<std::uint32_t>(kDodgePermillePerDex * dexPoints, kMaxDodgePermille));
    s.critChance = static_cast<float>(s.critPermille) / 1000.0f;
    s.dodgeChance = static_cast<float>(s.dodgePermille) / 1000.0f;
    s.hpRegen = static_cast<float>(static_cast<double>(s.maxHp) * static_cast<double>(kHpRegenPerSecond) *
                                   (1.0 + vit / kRegenVitalityDivisor));
    s.mpRegen = static_cast<float>(static_cast<double>(s.maxMp) * static_cast<double>(kMpRegenPerSecond) *
                                   (1.0 + spi / kRegenSpiritDivisor));
    s.healMultiplier = static_cast<float>(1.0 + (intl + spi) / kHealAttributeDivisor);
    return s;
}

Stats deriveStats(std::uint8_t charClass, std::uint16_t level, EquipBonus equip) noexcept
{
    return deriveStats(charClass, level, Attributes{}, equip);
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

std::uint32_t healAmount(const SkillDef& skill, std::uint32_t targetMaxHp, float healMultiplier) noexcept
{
    const double v = static_cast<double>(targetMaxHp) * static_cast<double>(skill.healFraction) *
                     static_cast<double>(std::max(healMultiplier, 0.0f));
    const double clamped = std::clamp(std::round(v), 1.0, 1.0e9);
    return static_cast<std::uint32_t>(clamped);
}

std::uint16_t healPermille(const SkillDef& skill, float healMultiplier) noexcept
{
    const double v =
        1000.0 * static_cast<double>(skill.healFraction) * static_cast<double>(std::max(healMultiplier, 0.0f));
    return static_cast<std::uint16_t>(std::clamp(std::round(v), 0.0, 1000.0));
}

} // namespace vo::game
