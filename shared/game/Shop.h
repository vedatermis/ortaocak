// Merchant and consumable rules shared by client and server (docs/PROTOCOL_SHOP.md §2, v8): prices,
// the planning of a purchase / sale against an inventory and a balance, and the checks of using a
// consumable. Pure and deterministic: the client predicts with the same code the server decides with.
//
//   const auto buy = game::planShopBuy(items, inventory, coins, templateId, quantity, unitPrice);
//   if (buy.result == net::ShopResult::Ok) -> write buy.add + coins - buy.cost in one transaction
#pragma once

#include "shared/game/Items.h"
#include "shared/net/Opcodes.h"

#include <cstdint>
#include <optional>

namespace vo::game {

using ShopResult = net::ShopResult;
using ItemUseResult = net::ItemUseResult;

// A merchant sells an item for its value and buys it back for half of it, rounded down (§2.1). An
// item worth 0 (or 1: half rounds to 0) cannot be sold.
inline constexpr std::uint32_t kSellDivisor = 2;
[[nodiscard]] constexpr std::uint32_t buyPrice(const ItemTemplate& t) noexcept
{
    return t.value;
}
[[nodiscard]] constexpr std::uint32_t sellPrice(const ItemTemplate& t) noexcept
{
    return t.value / kSellDivisor;
}

// ---- Buying (§2.2) ----

struct ShopBuyPlan
{
    ShopResult result = ShopResult::ServerError;
    ItemAddPlan add;        // on Ok: where every unit goes (stacks first, then free bag slots)
    std::uint32_t cost = 0; // on Ok: quantity x unit price
};

// Decides the session's part of SHOP_BUY_REQ (the world already checked the shop, range and stock):
// BadQuantity (0, an unknown template, or more than its max_stack), NotEnoughCoins (cost above
// `coins`), BagFull (not every unit fits; nothing is bought), else Ok.
[[nodiscard]] ShopBuyPlan planShopBuy(const ItemCatalog& catalog, const Inventory& inv, std::uint32_t coins,
                                      ItemTemplateId templateId, std::uint16_t quantity, std::uint32_t unitPrice);

// ---- Selling (§2.2) ----

struct ShopSellPlan
{
    ShopResult result = ShopResult::ServerError;
    ItemTemplateId templateId = 0; // what the slot holds (0 if empty / hidden / out of range)
    ItemQuantityChange change;     // on Ok: the stack shrinks (after 0 = the row is deleted)
    std::uint32_t gained = 0;      // on Ok: quantity x sell price
};

// Decides the session's part of SHOP_SELL_REQ: BadQuantity (0), NoItem (bag slot out of range,
// empty or hidden), BadQuantity (more than the stack holds), CannotSell (sell price 0), PurseFull
// (the balance would pass kMaxCoins), else Ok.
[[nodiscard]] ShopSellPlan planShopSell(const ItemCatalog& catalog, const Inventory& inv, std::uint32_t coins,
                                        std::uint8_t bagSlot, std::uint16_t quantity);

// ---- Consumables (§2.3) ----

// The bag slot ITEM_USE_REQ(template, bag_slot) uses: `bagSlot` itself if it holds that template,
// or for kItemUseAnyStack the lowest bag slot holding it. nullopt = NoItem.
[[nodiscard]] std::optional<std::uint8_t> findUseStack(const Inventory& inv, ItemTemplateId templateId,
                                                       std::uint8_t bagSlot) noexcept;

// The session's checks, first failure wins: NotUsable (not a known consumable), NoItem (no such
// stack), else Ok.
[[nodiscard]] ItemUseResult checkItemUseStack(const ItemCatalog& catalog, const Inventory& inv,
                                              ItemTemplateId templateId, std::uint8_t bagSlot) noexcept;

struct ItemUseState
{
    bool dead = false;
    bool stunned = false;
    bool onCooldown = false; // the shared consumable cooldown is running
    std::uint32_t hp = 0, maxHp = 0, mp = 0, maxMp = 0;
};

// The world's checks for a consumable restoring `restoreHp` / `restoreMp`: YouAreDead, Stunned,
// OnCooldown, AlreadyFull (everything it restores is already at the maximum), else Ok.
[[nodiscard]] ItemUseResult checkItemUse(std::uint32_t restoreHp, std::uint32_t restoreMp,
                                         const ItemUseState& s) noexcept;

} // namespace vo::game
