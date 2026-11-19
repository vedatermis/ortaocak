// Item upgrade rules shared by client and server (docs/PROTOCOL_UPGRADE.md §2, v10): which items can
// be upgraded, the tier of an item, the stone it needs, the success chance, the Akçe fee and what a
// failure costs, the stat bonus of each level, and the planning of one attempt against an inventory
// and a balance. Pure and deterministic: the client shows what the server will decide with the same
// tables; only the roll itself is the server's.
//
//   const auto plan = game::planUpgrade(items, inventory, coins, bagSlot, itemId, fromLevel, stoneSlot);
//   if (plan.result == net::UpgradeResult::Success) -> roll plan.chancePermille, then write
//       upgradeAttemptWrite(plan, succeeded) in one transaction
#pragma once

#include "shared/game/Items.h"
#include "shared/net/Opcodes.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace vo::game {

using UpgradeResult = net::UpgradeResult;

inline constexpr std::uint8_t kUpgradeTierCount = 5;

// Upgrade stone grades (items.toml `upgrade_stone`).
enum class StoneGrade : std::uint8_t
{
    None = 0,
    Ham = 1,  // item tiers 1-2
    Saf = 2,  // item tiers 3-4
    Usta = 3, // item tier 5 and every unique item
};

// Weapons, shields and armour (uniques included); never stackables, materials or consumables.
[[nodiscard]] bool isUpgradable(const ItemTemplate& t) noexcept;
// The item's tier 1..5 by its required level: 1-4 -> 1, 5-8 -> 2, 9-12 -> 3, 13-16 -> 4, 17+ -> 5.
[[nodiscard]] std::uint8_t upgradeTier(const ItemTemplate& t) noexcept;
// The stone an attempt on `t` spends: Usta for uniques and tier 5, Saf for tiers 3-4, Ham for 1-2.
[[nodiscard]] StoneGrade requiredStone(const ItemTemplate& t) noexcept;
// Turkish names ("Ham Tav Taşı"); "" for None.
[[nodiscard]] std::string_view stoneGradeName(StoneGrade g) noexcept;

// Success chance of an attempt that raises an item to `target` (1..10), in permille:
// +1..+3 1000, +4 800, +5 600, +6 450, +7 300, +8 150, +9 80, +10 30. 0 outside 1..10.
[[nodiscard]] std::uint16_t upgradeChancePermille(std::uint8_t target) noexcept;
// A failed attempt at `target` destroys the item (target >= 6); at +4 / +5 it stays as it was.
// (+1..+3 cannot fail.)
inline constexpr std::uint8_t kFirstDestroyingTarget = 6;
[[nodiscard]] constexpr bool failureDestroys(std::uint8_t target) noexcept
{
    return target >= kFirstDestroyingTarget;
}

// The Akçe fee of one attempt raising `t` to `target` (1..10): the tier's base fee times the
// target's multiplier (tables in Upgrade.cpp and §2.4). 0 outside 1..10.
[[nodiscard]] std::uint32_t upgradeFee(const ItemTemplate& t, std::uint8_t target) noexcept;
[[nodiscard]] std::uint32_t upgradeFeeForTier(std::uint8_t tier, std::uint8_t target) noexcept;

// The share of an item's attack / defence a level adds, in percent of the base value (cumulative):
// +1 3, +2 6, +3 9, +4 12, +5 15, +6 21, +7 28, +8 36, +9 45, +10 55.
[[nodiscard]] std::uint16_t upgradeBonusPercent(std::uint8_t level) noexcept;
// What `level` adds to a base value: round(base x percent / 100), halves up. 0 for base 0.
[[nodiscard]] std::int32_t upgradeBonus(std::int32_t base, std::uint8_t level) noexcept;
// The item's attack / defence at `level` (base + bonus). Attributes never change.
[[nodiscard]] std::int32_t upgradedAttack(const ItemTemplate& t, std::uint8_t level) noexcept;
[[nodiscard]] std::int32_t upgradedDefense(const ItemTemplate& t, std::uint8_t level) noexcept;

// The lowest-id stone template of a grade in the catalog (nullptr if the catalog has none).
[[nodiscard]] const ItemTemplate* stoneTemplate(const ItemCatalog& catalog, StoneGrade g) noexcept;
// How many units of stones of grade `g` the bag holds.
[[nodiscard]] std::uint32_t countStones(const ItemCatalog& catalog, const Inventory& inv, StoneGrade g) noexcept;

// ---- One attempt (§2.2) ----

struct UpgradePlan
{
    // Success = the attempt may be made (the roll decides Success / Failed / Destroyed); otherwise the
    // refusal (nothing changes).
    UpgradeResult result = UpgradeResult::ServerError;
    ItemTemplateId templateId = 0; // what the item slot holds (0 if nothing known)
    std::uint8_t level = 0;        // the item's current level
    // On an attempt:
    ItemPlace itemPlace;
    ItemStack item;
    ItemPlace stonePlace;
    ItemStack stone;
    std::uint8_t target = 0;          // level + 1
    std::uint32_t fee = 0;            // Akçe paid
    std::uint16_t chancePermille = 0; // success chance
    bool destroysOnFailure = false;
};

// Decides UPGRADE_REQ(bag_slot, item_id, from_level, stone_slot) against `inv` and the balance (the
// world already checked the open smith, life and range). Checks, first failure wins: NoItem (slot
// out of range, empty, hidden, another item id), Changed (level != fromLevel), NotUpgradable,
// MaxLevel (already 10), NoStone (no fitting stone in the bag for kUpgradeAnyStone; the named slot
// empty or out of range), WrongStone (the named slot holds something else), NotEnoughCoins, else
// Success (= attempt allowed).
[[nodiscard]] UpgradePlan planUpgrade(const ItemCatalog& catalog, const Inventory& inv, std::uint32_t coins,
                                      std::uint8_t bagSlot, ItemInstanceId itemId, std::uint8_t fromLevel,
                                      std::uint8_t stoneSlot);

// The outcome of an allowed attempt for a roll in 0..999 (uniform): Success if roll < chance, else
// Destroyed when the target destroys on failure, else Failed.
[[nodiscard]] UpgradeResult upgradeOutcome(const UpgradePlan& plan, std::uint16_t roll) noexcept;

// The item's slot content after an attempt with `outcome` (Success: one level up; Failed: as it
// was; Destroyed: empty) and the stone stack after it (one unit less; empty when it was the last).
[[nodiscard]] ItemStack itemAfterUpgrade(const UpgradePlan& plan, UpgradeResult outcome) noexcept;
[[nodiscard]] ItemStack stoneAfterUpgrade(const UpgradePlan& plan) noexcept;

} // namespace vo::game
