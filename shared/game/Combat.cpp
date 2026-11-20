#include "shared/game/Combat.h"

#include "shared/game/Character.h"
#include "shared/game/Items.h"
#include "shared/game/Upgrade.h"

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
    double raw = 80.0 * std::pow(static_cast<double>(level), 1.6);
    if (level >= kSteepCurveFrom)
        raw *= 1.0 + 0.6 * static_cast<double>(level - (kSteepCurveFrom - 1));
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
            // M16 (PROTOCOL_UPGRADE.md §2.5): the upgrade level raises attack / defence, nothing else
            // (M18: the damage range moves with it, its width and the bonus lines stay).
            b.attack += upgradedAttack(*t, st.upgrade);
            b.defense += upgradedDefense(*t, st.upgrade);
            for (std::size_t i = 0; i < kAttributeCount; ++i)
                b.attributes[i] = clampU16(static_cast<std::int64_t>(b.attributes[i]) + t->attributes[i]);
            b.attackSpread += t->attackSpread;
            b.maxHp += t->bonusHp;
            b.maxMp += t->bonusMp;
            b.critPermille += t->bonusCritPermille;
            b.dodgePermille += t->bonusDodgePermille;
            if (s == static_cast<std::size_t>(EquipSlot::MainHand) && t->isWeapon())
            {
                b.swingIntervalMs = t->speedMs;
                b.reach = t->reach;
            }
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

    const double hp = static_cast<double>(c.baseHp + l * c.hpPerLevel) + kHpPerVitality * vit + equip.maxHp;
    s.maxHp = static_cast<std::uint32_t>(std::clamp<double>(hp, 1.0, 1.0e9));
    const double mp = static_cast<double>(c.baseMp + l * c.mpPerLevel) + kMpPerSpirit * spi + kMpPerIntelligence * intl +
                      equip.maxMp;
    s.maxMp = static_cast<std::uint32_t>(std::clamp<double>(mp, 0.0, 1.0e9));
    const double weaponScale = 1.0 + primary / kWeaponScaleDivisor;
    const double attack = static_cast<double>(c.baseAttack + l * c.attackPerLevel) +
                          static_cast<double>(equip.attack) * weaponScale + primary * kAttackPerPrimary;
    s.attack = clampU16(std::lround(attack));
    // M18: the weapons' damage range, scaled like the weapon's attack.
    const double spread = static_cast<double>(std::max(equip.attackSpread, 0)) * weaponScale;
    s.attackMin = std::min(s.attack, clampU16(std::lround(attack - spread)));
    s.attackMax = std::max(s.attack, clampU16(std::lround(attack + spread)));
    s.bonusHp = equip.maxHp;
    s.bonusMp = equip.maxMp;
    s.bonusCritPermille = static_cast<std::uint16_t>(std::min<std::uint32_t>(equip.critPermille, 1000));
    s.bonusDodgePermille = static_cast<std::uint16_t>(std::min<std::uint32_t>(equip.dodgePermille, 1000));
    const double defense =
        static_cast<double>(c.baseDefense + l * c.defensePerLevel + equip.defense) + vit * kDefensePerVitality;
    s.defense = clampU16(std::lround(defense));
    // M18 (PROTOCOL_EQUIPMENT.md §3.1): the main-hand weapon sets the basic attack interval and, for the
    // melee classes, the reach (a Mage's basic attack is a 14 m spell whatever the staff).
    s.attackRange = c.attackRange;
    if (equip.reach > 0.0f && c.attackRange <= kMaxWeaponReach)
        s.attackRange = equip.reach;
    s.attackIntervalMs = c.attackIntervalMs;
    s.swingFactor = 1.0f;
    if (equip.swingIntervalMs > 0)
    {
        s.attackIntervalMs = equip.swingIntervalMs;
        s.swingFactor = static_cast<float>(static_cast<double>(equip.swingIntervalMs) / static_cast<double>(c.attackIntervalMs));
    }
    const auto dexPoints = static_cast<std::uint32_t>(dex);
    s.critPermille = static_cast<std::uint16_t>(std::min<std::uint32_t>(
        c.baseCritPermille + kCritPermillePerDex * dexPoints + equip.critPermille, kMaxCritPermille));
    s.dodgePermille = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(kDodgePermillePerDex * dexPoints + equip.dodgePermille, kMaxDodgePermille));
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

namespace {

float rollMiddle(std::uint32_t lo, std::uint32_t hi) noexcept
{
    return 0.5f * (static_cast<float>(lo) + static_cast<float>(hi));
}

} // namespace

float rolledAttackLow(std::uint32_t attackMin, std::uint32_t attackMax) noexcept
{
    const std::uint32_t lo = std::min(attackMin, attackMax);
    return static_cast<float>(lo) - (1.0f - kDamageVarianceMin) * rollMiddle(attackMin, attackMax);
}

float rolledAttackHigh(std::uint32_t attackMin, std::uint32_t attackMax) noexcept
{
    const std::uint32_t hi = std::max(attackMin, attackMax);
    return static_cast<float>(hi) + (kDamageVarianceMax - 1.0f) * rollMiddle(attackMin, attackMax);
}

Hit computeDamage(std::uint32_t attackMin, std::uint32_t attackMax, float multiplier, std::uint32_t defense,
                  float critChance, float varianceRoll, float critRoll) noexcept
{
    if (attackMin == attackMax)
        return computeDamage(attackMin, multiplier, defense, critChance, varianceRoll, critRoll);
    const float lo = rolledAttackLow(attackMin, attackMax);
    const float hi = rolledAttackHigh(attackMin, attackMax);
    const float rolled = lo + (hi - lo) * std::clamp(varianceRoll, 0.0f, 1.0f);
    Hit h;
    h.critical = critRoll < critChance;
    float raw = rolled * multiplier - static_cast<float>(defense) * kDefenseFactor;
    if (h.critical)
        raw *= kCritMultiplier;
    const float rounded = std::round(raw);
    h.amount = rounded < 1.0f ? 1u : static_cast<std::uint32_t>(std::min(rounded, 1.0e9f));
    return h;
}

double basicDamagePerSecond(const Stats& s, std::uint32_t defense) noexcept
{
    const double perHit = std::max(1.0, static_cast<double>(s.attack) * static_cast<double>(s.swingFactor) -
                                            static_cast<double>(defense) * static_cast<double>(kDefenseFactor));
    const double crit = static_cast<double>(s.critPermille) / 1000.0;
    const double interval = std::max<double>(static_cast<double>(s.attackIntervalMs), kMinAttackIntervalMs) / 1000.0;
    return perHit * (1.0 + crit * (static_cast<double>(kCritMultiplier) - 1.0)) / interval;
}

const SkillDef* findSkill(std::uint16_t id) noexcept
{
    return skills().find(id);
}

const SkillDef* classSkill(std::uint8_t charClass) noexcept
{
    const ClassTable* c = classTable(charClass);
    return c ? findSkill(c->skillId) : nullptr;
}

std::uint32_t healAmount(const SkillDef& skill, std::uint32_t targetMaxHp, float healMultiplier,
                         std::uint8_t rank) noexcept
{
    const double v = static_cast<double>(targetMaxHp) * static_cast<double>(skill.at(rank).healFraction) *
                     static_cast<double>(std::max(healMultiplier, 0.0f));
    const double clamped = std::clamp(std::round(v), 1.0, 1.0e9);
    return static_cast<std::uint32_t>(clamped);
}

std::uint16_t healPermille(const SkillDef& skill, float healMultiplier, std::uint8_t rank) noexcept
{
    const double v = 1000.0 * static_cast<double>(skill.at(rank).healFraction) *
                     static_cast<double>(std::max(healMultiplier, 0.0f));
    return static_cast<std::uint16_t>(std::clamp(std::round(v), 0.0, 1000.0));
}

Stats applyEffectModifiers(Stats s, const EffectModifiers& m) noexcept
{
    if (m.none())
        return s;
    const auto scale = [](double v, float pct) { return v * (1.0 + static_cast<double>(pct) / 100.0); };
    s.attack = clampU16(std::llround(scale(s.attack, m.attackPct)));
    s.attackMin = std::min(s.attack, clampU16(std::llround(scale(s.attackMin, m.attackPct))));
    s.attackMax = std::max(s.attack, clampU16(std::llround(scale(s.attackMax, m.attackPct))));
    s.defense = clampU16(std::llround(scale(s.defense, m.defensePct)));
    s.maxHp = static_cast<std::uint32_t>(std::clamp<double>(std::round(scale(s.maxHp, m.maxHpPct)), 1.0, 1.0e9));
    if (m.hastePct > 0.0f)
    {
        const double interval = static_cast<double>(s.attackIntervalMs) / (1.0 + static_cast<double>(m.hastePct) / 100.0);
        s.attackIntervalMs = std::max<std::uint32_t>(kMinAttackIntervalMs, static_cast<std::uint32_t>(std::lround(interval)));
    }
    if (m.dodgePct > 0.0f)
    {
        const auto add = static_cast<std::uint32_t>(std::lround(static_cast<double>(m.dodgePct) * 10.0));
        s.dodgePermille = static_cast<std::uint16_t>(
            std::max<std::uint32_t>(s.dodgePermille, std::min<std::uint32_t>(s.dodgePermille + add, kMaxDodgeWithEffectsPermille)));
        s.dodgeChance = static_cast<float>(s.dodgePermille) / 1000.0f;
    }
    return s;
}

std::uint32_t periodicDamage(std::uint32_t attack, float valuePct) noexcept
{
    const double v = static_cast<double>(attack) * static_cast<double>(valuePct) / 100.0;
    return static_cast<std::uint32_t>(std::clamp(std::round(v), 1.0, 1.0e9));
}

std::uint32_t periodicHeal(std::uint32_t targetMaxHp, float valuePct, float healMultiplier) noexcept
{
    const double v = static_cast<double>(targetMaxHp) * static_cast<double>(valuePct) / 100.0 *
                     static_cast<double>(std::max(healMultiplier, 0.0f));
    return static_cast<std::uint32_t>(std::clamp(std::round(v), 1.0, 1.0e9));
}

std::uint32_t shieldAmount(std::uint32_t attack, float valuePct) noexcept
{
    return periodicDamage(attack, valuePct);
}

} // namespace vo::game
