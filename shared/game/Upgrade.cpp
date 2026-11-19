#include "shared/game/Upgrade.h"

#include <algorithm>
#include <optional>

namespace vo::game {

namespace {

// §2.3: success chance by target level (index = target), permille.
constexpr std::array<std::uint16_t, kMaxUpgradeLevel + 1> kChancePermille{0,   1000, 1000, 1000, 800, 600,
                                                                          450, 300,  150,  80,   30};
// §2.4: base fee by tier (index = tier) and the target's multiplier in tenths (index = target).
constexpr std::array<std::uint32_t, kUpgradeTierCount + 1> kTierFee{0, 6, 20, 55, 110, 190};
constexpr std::array<std::uint32_t, kMaxUpgradeLevel + 1> kFeeTenths{0, 10, 15, 20, 30, 40, 60, 80, 110, 150, 200};
// §2.5: cumulative share of the base attack / defence, percent (index = level).
constexpr std::array<std::uint16_t, kMaxUpgradeLevel + 1> kBonusPercent{0, 3, 6, 9, 12, 15, 21, 28, 36, 45, 55};

} // namespace

bool isUpgradable(const ItemTemplate& t) noexcept
{
    return (t.kind == ItemKind::Weapon || t.kind == ItemKind::Shield || t.kind == ItemKind::Armor) &&
           t.maxStack == 1 && t.isEquippable();
}

std::uint8_t upgradeTier(const ItemTemplate& t) noexcept
{
    const unsigned level = std::max<unsigned>(t.level, 1);
    return static_cast<std::uint8_t>(std::min<unsigned>((level - 1) / 4 + 1, kUpgradeTierCount));
}

StoneGrade requiredStone(const ItemTemplate& t) noexcept
{
    if (t.rarity == Rarity::Unique)
        return StoneGrade::Usta;
    const std::uint8_t tier = upgradeTier(t);
    if (tier >= 5)
        return StoneGrade::Usta;
    if (tier >= 3)
        return StoneGrade::Saf;
    return StoneGrade::Ham;
}

std::string_view stoneGradeName(StoneGrade g) noexcept
{
    switch (g)
    {
    case StoneGrade::Ham: return "Ham Tav Taşı";
    case StoneGrade::Saf: return "Saf Tav Taşı";
    case StoneGrade::Usta: return "Usta Tav Taşı";
    case StoneGrade::None: break;
    }
    return "";
}

std::uint16_t upgradeChancePermille(std::uint8_t target) noexcept
{
    return target >= 1 && target <= kMaxUpgradeLevel ? kChancePermille[target] : 0;
}

std::uint32_t upgradeFeeForTier(std::uint8_t tier, std::uint8_t target) noexcept
{
    if (target < 1 || target > kMaxUpgradeLevel)
        return 0;
    tier = std::clamp<std::uint8_t>(tier, 1, kUpgradeTierCount);
    return kTierFee[tier] * kFeeTenths[target] / 10;
}

std::uint32_t upgradeFee(const ItemTemplate& t, std::uint8_t target) noexcept
{
    return upgradeFeeForTier(upgradeTier(t), target);
}

std::uint16_t upgradeBonusPercent(std::uint8_t level) noexcept
{
    return kBonusPercent[std::min<std::uint8_t>(level, kMaxUpgradeLevel)];
}

std::int32_t upgradeBonus(std::int32_t base, std::uint8_t level) noexcept
{
    if (base <= 0)
        return 0;
    const std::int64_t scaled = static_cast<std::int64_t>(base) * upgradeBonusPercent(level);
    return static_cast<std::int32_t>((scaled + 50) / 100);
}

std::int32_t upgradedAttack(const ItemTemplate& t, std::uint8_t level) noexcept
{
    return t.attack + upgradeBonus(t.attack, level);
}

std::int32_t upgradedDefense(const ItemTemplate& t, std::uint8_t level) noexcept
{
    return t.defense + upgradeBonus(t.defense, level);
}

const ItemTemplate* stoneTemplate(const ItemCatalog& catalog, StoneGrade g) noexcept
{
    if (g == StoneGrade::None)
        return nullptr;
    for (const ItemTemplate& t : catalog.templates()) // ascending id
        if (t.kind == ItemKind::Material && t.upgradeStone == static_cast<std::uint8_t>(g))
            return &t;
    return nullptr;
}

std::uint32_t countStones(const ItemCatalog& catalog, const Inventory& inv, StoneGrade g) noexcept
{
    std::uint32_t n = 0;
    for (std::uint8_t s = 0; s < kBagSlots; ++s)
    {
        const ItemStack& st = inv.at(ItemPlace::bag(s));
        if (st.empty())
            continue;
        if (const ItemTemplate* t = catalog.find(st.templateId); t && t->upgradeStone == static_cast<std::uint8_t>(g))
            n += st.quantity;
    }
    return n;
}

UpgradePlan planUpgrade(const ItemCatalog& catalog, const Inventory& inv, std::uint32_t coins, std::uint8_t bagSlot,
                        ItemInstanceId itemId, std::uint8_t fromLevel, std::uint8_t stoneSlot)
{
    using R = UpgradeResult;
    UpgradePlan plan;
    if (bagSlot >= kBagSlots)
    {
        plan.result = R::NoItem;
        return plan;
    }
    plan.itemPlace = ItemPlace::bag(bagSlot);
    const ItemStack& item = inv.at(plan.itemPlace);
    const ItemTemplate* t = item.empty() ? nullptr : catalog.find(item.templateId);
    if (!t || item.id != itemId)
    {
        plan.result = R::NoItem;
        plan.templateId = t ? item.templateId : 0;
        plan.level = t ? item.upgrade : 0;
        return plan;
    }
    plan.templateId = item.templateId;
    plan.level = item.upgrade;
    if (item.upgrade != fromLevel)
    {
        plan.result = R::Changed;
        return plan;
    }
    if (!isUpgradable(*t))
    {
        plan.result = R::NotUpgradable;
        return plan;
    }
    if (item.upgrade >= kMaxUpgradeLevel)
    {
        plan.result = R::MaxLevel;
        return plan;
    }
    const auto grade = static_cast<std::uint8_t>(requiredStone(*t));
    std::optional<std::uint8_t> stone;
    if (stoneSlot == net::kUpgradeAnyStone)
    {
        for (std::uint8_t s = 0; s < kBagSlots && !stone; ++s)
        {
            const ItemStack& st = inv.at(ItemPlace::bag(s));
            const ItemTemplate* stoneT = st.empty() ? nullptr : catalog.find(st.templateId);
            if (stoneT && stoneT->upgradeStone == grade)
                stone = s;
        }
        if (!stone)
        {
            plan.result = R::NoStone;
            return plan;
        }
    }
    else
    {
        const ItemStack* st = stoneSlot < kBagSlots ? &inv.at(ItemPlace::bag(stoneSlot)) : nullptr;
        const ItemTemplate* stoneT = st && !st->empty() ? catalog.find(st->templateId) : nullptr;
        if (!stoneT)
        {
            plan.result = R::NoStone;
            return plan;
        }
        if (stoneT->upgradeStone != grade || stoneSlot == bagSlot)
        {
            plan.result = R::WrongStone;
            return plan;
        }
        stone = stoneSlot;
    }
    plan.target = static_cast<std::uint8_t>(item.upgrade + 1);
    plan.fee = upgradeFee(*t, plan.target);
    if (plan.fee > coins)
    {
        plan.result = R::NotEnoughCoins;
        return plan;
    }
    plan.item = item;
    plan.stonePlace = ItemPlace::bag(*stone);
    plan.stone = inv.at(plan.stonePlace);
    plan.chancePermille = upgradeChancePermille(plan.target);
    plan.destroysOnFailure = failureDestroys(plan.target);
    plan.result = R::Success;
    return plan;
}

UpgradeResult upgradeOutcome(const UpgradePlan& plan, std::uint16_t roll) noexcept
{
    if (roll < plan.chancePermille)
        return UpgradeResult::Success;
    return plan.destroysOnFailure ? UpgradeResult::Destroyed : UpgradeResult::Failed;
}

ItemStack itemAfterUpgrade(const UpgradePlan& plan, UpgradeResult outcome) noexcept
{
    ItemStack s = plan.item;
    if (outcome == UpgradeResult::Success)
        s.upgrade = plan.target;
    else if (outcome == UpgradeResult::Destroyed)
        s = {};
    return s;
}

ItemStack stoneAfterUpgrade(const UpgradePlan& plan) noexcept
{
    ItemStack s = plan.stone;
    if (s.quantity <= 1)
        return {};
    --s.quantity;
    return s;
}

} // namespace vo::game
