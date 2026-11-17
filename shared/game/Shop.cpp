#include "shared/game/Shop.h"

#include <algorithm>

namespace vo::game {

ShopBuyPlan planShopBuy(const ItemCatalog& catalog, const Inventory& inv, std::uint32_t coins, ItemTemplateId templateId,
                        std::uint16_t quantity, std::uint32_t unitPrice)
{
    ShopBuyPlan out;
    const ItemTemplate* t = catalog.find(templateId);
    if (!t || quantity == 0 || quantity > t->maxStack)
    {
        out.result = ShopResult::BadQuantity;
        return out;
    }
    const std::uint64_t cost = static_cast<std::uint64_t>(unitPrice) * quantity;
    if (cost > coins)
    {
        out.result = ShopResult::NotEnoughCoins;
        return out;
    }
    ItemAddPlan add = planItemAdd(catalog, inv, {{templateId, quantity}});
    if (add.totalPlaced() < quantity)
    {
        out.result = ShopResult::BagFull;
        return out;
    }
    out.result = ShopResult::Ok;
    out.add = std::move(add);
    out.cost = static_cast<std::uint32_t>(cost);
    return out;
}

ShopSellPlan planShopSell(const ItemCatalog& catalog, const Inventory& inv, std::uint32_t coins, std::uint8_t bagSlot,
                          std::uint16_t quantity)
{
    ShopSellPlan out;
    if (quantity == 0)
    {
        out.result = ShopResult::BadQuantity;
        return out;
    }
    const ItemPlace place = ItemPlace::bag(bagSlot);
    const ItemStack* stack = isValidPlace(place) ? &inv.at(place) : nullptr;
    const ItemTemplate* t = stack && !stack->empty() ? catalog.find(stack->templateId) : nullptr;
    if (!t)
    {
        out.result = ShopResult::NoItem;
        return out;
    }
    out.templateId = t->id;
    if (quantity > stack->quantity)
    {
        out.result = ShopResult::BadQuantity;
        return out;
    }
    const std::uint32_t unit = sellPrice(*t);
    if (unit == 0)
    {
        out.result = ShopResult::CannotSell;
        return out;
    }
    const std::uint64_t gained = static_cast<std::uint64_t>(unit) * quantity;
    if (static_cast<std::uint64_t>(std::min(coins, net::kMaxCoins)) + gained > net::kMaxCoins)
    {
        out.result = ShopResult::PurseFull;
        return out;
    }
    out.result = ShopResult::Ok;
    out.change = {stack->id, place, stack->quantity, static_cast<std::uint16_t>(stack->quantity - quantity)};
    out.gained = static_cast<std::uint32_t>(gained);
    return out;
}

std::optional<std::uint8_t> findUseStack(const Inventory& inv, ItemTemplateId templateId, std::uint8_t bagSlot) noexcept
{
    if (templateId == 0)
        return std::nullopt;
    if (bagSlot == net::kItemUseAnyStack)
    {
        for (std::uint8_t s = 0; s < kBagSlots; ++s)
        {
            const ItemStack& st = inv.at(ItemPlace::bag(s));
            if (!st.empty() && st.templateId == templateId && st.quantity > 0)
                return s;
        }
        return std::nullopt;
    }
    if (bagSlot >= kBagSlots)
        return std::nullopt;
    const ItemStack& st = inv.at(ItemPlace::bag(bagSlot));
    if (st.empty() || st.templateId != templateId || st.quantity == 0)
        return std::nullopt;
    return bagSlot;
}

ItemUseResult checkItemUseStack(const ItemCatalog& catalog, const Inventory& inv, ItemTemplateId templateId,
                                std::uint8_t bagSlot) noexcept
{
    const ItemTemplate* t = catalog.find(templateId);
    if (!t || !t->isConsumable())
        return ItemUseResult::NotUsable;
    if (!findUseStack(inv, templateId, bagSlot))
        return ItemUseResult::NoItem;
    return ItemUseResult::Ok;
}

ItemUseResult checkItemUse(std::uint32_t restoreHp, std::uint32_t restoreMp, const ItemUseState& s) noexcept
{
    if (s.dead)
        return ItemUseResult::YouAreDead;
    if (s.stunned)
        return ItemUseResult::Stunned;
    if (s.onCooldown)
        return ItemUseResult::OnCooldown;
    const bool hpHelps = restoreHp > 0 && s.hp < s.maxHp;
    const bool mpHelps = restoreMp > 0 && s.mp < s.maxMp;
    if (!hpHelps && !mpHelps)
        return ItemUseResult::AlreadyFull;
    return ItemUseResult::Ok;
}

} // namespace vo::game
