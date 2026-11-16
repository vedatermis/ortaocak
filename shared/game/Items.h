// Items shared by client and server (docs/PROTOCOL_ITEMS.md): item templates loaded from
// gamedata/items.toml, starter kits, the in-memory inventory of one character and the pure placement
// rules of §1.1/§1.2. Everything here is deterministic and free of I/O except the loader, so the
// client can predict a move and the server decides it with exactly the same code.
//
//   ItemCatalog catalog;
//   if (!loadItemCatalog(*locateItemsFile(), catalog, error)) -> refuse to start
//   const ItemMovePlan plan = planItemMove(catalog, inventory, {charClass, level}, from, to);
//   if (plan.result == ItemMoveResult::Ok) applyItemMove(inventory, plan);
//
// Unknown templates: an inventory may hold an item whose template id the catalog does not know (a
// row written by a newer server, or a template removed from the file). Such an item is "hidden": it
// is not sent to clients, its slot looks empty to them but cannot be used (moving out of it is
// NoItem, into it SlotBlocked), it never counts as a free bag slot and is never moved implicitly.
#pragma once

#include "shared/net/Opcodes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::game {

using ItemTemplateId = std::uint16_t; // 0 = nothing
using ItemInstanceId = std::uint64_t; // 0 = nothing
using ItemMoveResult = net::ItemMoveResult;

inline constexpr std::string_view kItemsFile = "gamedata/items.toml";

// ---- Places (§1) ----

enum class ItemContainer : std::uint8_t
{
    Equipment = 0,
    Bag = 1,
};
inline constexpr std::uint8_t kItemContainerMax = 1;

enum class EquipSlot : std::uint8_t
{
    MainHand = 0,
    OffHand = 1,
    Head = 2,
    Chest = 3,
    Legs = 4,
    Hands = 5,
    Feet = 6,
    Reserved = 7,
};
inline constexpr std::size_t kEquipmentSlots = net::kItemEquipmentSlots; // 8
inline constexpr std::size_t kBagSlots = net::kItemBagSlots;             // 28
inline constexpr std::size_t kBagColumns = 7;
inline constexpr std::size_t kBagRows = 4;

struct ItemPlace
{
    std::uint8_t container = 0;
    std::uint8_t slot = 0;

    [[nodiscard]] static constexpr ItemPlace equipment(EquipSlot s) noexcept
    {
        return {static_cast<std::uint8_t>(ItemContainer::Equipment), static_cast<std::uint8_t>(s)};
    }
    [[nodiscard]] static constexpr ItemPlace bag(std::uint8_t s) noexcept
    {
        return {static_cast<std::uint8_t>(ItemContainer::Bag), s};
    }
    [[nodiscard]] constexpr bool isEquipment() const noexcept
    {
        return container == static_cast<std::uint8_t>(ItemContainer::Equipment);
    }
    [[nodiscard]] constexpr bool isBag() const noexcept { return container == static_cast<std::uint8_t>(ItemContainer::Bag); }
    [[nodiscard]] constexpr bool is(EquipSlot s) const noexcept
    {
        return isEquipment() && slot == static_cast<std::uint8_t>(s);
    }
    bool operator==(const ItemPlace&) const = default;
};

// Container 0 with slot 0..7, or container 1 with slot 0..27.
[[nodiscard]] constexpr bool isValidPlace(ItemPlace p) noexcept
{
    if (p.container == static_cast<std::uint8_t>(ItemContainer::Equipment))
        return p.slot < kEquipmentSlots;
    if (p.container == static_cast<std::uint8_t>(ItemContainer::Bag))
        return p.slot < kBagSlots;
    return false;
}

// ---- Templates ----

enum class ItemKind : std::uint8_t
{
    Weapon,
    Shield,
};

enum class SlotRule : std::uint8_t
{
    MainHand,   // main_hand
    OffHand,    // off_hand
    EitherHand, // either_hand: MainHand or OffHand
    TwoHand,    // two_hand: MainHand, and only while OffHand is empty
};

enum class Grip : std::uint8_t
{
    Sword,
    Dagger,
    Mace,
    Staff,
    Axe2h,
    Shield,
};

struct ItemTemplate
{
    ItemTemplateId id = 0;
    std::string name;        // UTF-8, Turkish
    std::string description; // may be empty
    ItemKind kind = ItemKind::Weapon;
    SlotRule slot = SlotRule::MainHand;
    std::uint8_t classMask = 0; // bit (1 << CharClass value) per allowed class
    std::uint16_t level = 1;    // required character level
    std::int32_t attack = 0;
    std::int32_t defense = 0;
    std::string model; // mesh id (weapons/<model>.vmesh)
    std::string icon;  // ui/items/<icon>.vtex
    Grip grip = Grip::Sword;

    [[nodiscard]] bool allowsClass(std::uint8_t charClass) const noexcept
    {
        return charClass < 8 && (classMask & (1u << charClass)) != 0;
    }
    [[nodiscard]] bool isTwoHanded() const noexcept { return slot == SlotRule::TwoHand; }
};

struct StarterKit
{
    std::uint8_t charClass = 0;
    std::array<ItemTemplateId, kEquipmentSlots> equip{}; // by EquipSlot; 0 = nothing
    std::vector<ItemTemplateId> bag;                     // placed in the first free bag slots
};

class ItemCatalog
{
public:
    // nullptr if the id is 0 or unknown.
    [[nodiscard]] const ItemTemplate* find(ItemTemplateId id) const noexcept;
    // nullptr if the class has no kit (an empty catalog has none).
    [[nodiscard]] const StarterKit* kitFor(std::uint8_t charClass) const noexcept;
    [[nodiscard]] const std::vector<ItemTemplate>& templates() const noexcept { return m_templates; } // by id
    [[nodiscard]] std::size_t size() const noexcept { return m_templates.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_templates.empty(); }

private:
    friend bool parseItemCatalog(std::string_view, std::string_view, ItemCatalog&, std::string&);
    std::vector<ItemTemplate> m_templates;           // sorted by id
    std::vector<std::int32_t> m_index;               // id -> index into m_templates, -1 = none
    std::array<std::optional<StarterKit>, 8> m_kits; // by class value
};

// Parses and validates the TOML text of gamedata/items.toml. Strict: unknown keys, duplicate ids,
// ids outside 1..65535, unknown kind/slot/class/grip names, empty names, bad levels, a missing or
// duplicate starter kit per class, kits naming unknown templates or slots the template may not
// take (class, level 1, two-handed + off-hand) or more items than the bag holds -> false with a
// message naming the file and the offending entry. On failure `out` is left unchanged.
[[nodiscard]] bool parseItemCatalog(std::string_view tomlText, std::string_view sourceName, ItemCatalog& out,
                                    std::string& error);
[[nodiscard]] bool loadItemCatalog(const std::filesystem::path& file, ItemCatalog& out, std::string& error);
// gamedata/items.toml searched upwards from the working directory, then from the executable
// directory (like config/ and assets/, core::locateDataFile).
[[nodiscard]] std::optional<std::filesystem::path> locateItemsFile();

// Names used in the file ("warrior", "main_hand", ...), for messages and tools.
[[nodiscard]] std::string_view slotRuleName(SlotRule r) noexcept;
[[nodiscard]] std::string_view itemKindName(ItemKind k) noexcept;
[[nodiscard]] std::string_view gripName(Grip g) noexcept;
[[nodiscard]] std::string_view equipSlotName(EquipSlot s) noexcept; // "main_hand", "off_hand", "head", ...
[[nodiscard]] std::string_view classKey(std::uint8_t charClass) noexcept; // "warrior" ... or "?"
// Turkish display names for the UI ("Silah", "Sağ el", ...).
[[nodiscard]] std::string_view itemKindDisplayName(ItemKind k) noexcept;
[[nodiscard]] std::string_view slotRuleDisplayName(SlotRule r) noexcept;

// ---- Inventory ----

struct ItemStack
{
    ItemInstanceId id = 0; // 0 = empty slot
    ItemTemplateId templateId = 0;
    std::uint16_t quantity = 0;

    [[nodiscard]] bool empty() const noexcept { return id == 0; }
    bool operator==(const ItemStack&) const = default;
};

// One stored item as the database (or INVENTORY) carries it.
struct StoredItem
{
    ItemInstanceId id = 0;
    ItemPlace place;
    ItemTemplateId templateId = 0;
    std::uint16_t quantity = 1;
    bool operator==(const StoredItem&) const = default;
};

class Inventory
{
public:
    // Builds an inventory from stored rows. False (with `error`) for an invalid place, two rows on
    // one place, a zero item id or quantity; unknown templates are accepted (hidden items).
    [[nodiscard]] static bool fromItems(const std::vector<StoredItem>& items, Inventory& out, std::string& error);

    // Precondition: isValidPlace(p).
    [[nodiscard]] const ItemStack& at(ItemPlace p) const noexcept;
    [[nodiscard]] ItemStack& at(ItemPlace p) noexcept;
    [[nodiscard]] const ItemStack& equipped(EquipSlot s) const noexcept { return at(ItemPlace::equipment(s)); }

    // Lowest bag slot that is empty, if any.
    [[nodiscard]] std::optional<std::uint8_t> firstFreeBagSlot() const noexcept;
    // Every occupied slot, equipment first, each container by slot.
    [[nodiscard]] std::vector<StoredItem> items() const;
    [[nodiscard]] std::size_t count() const noexcept;
    bool operator==(const Inventory&) const = default;

private:
    std::array<ItemStack, kEquipmentSlots> m_equipment{};
    std::array<ItemStack, kBagSlots> m_bag{};
};

// Template ids the other players see (MainHand, OffHand); hidden items count as nothing.
struct VisibleEquipment
{
    ItemTemplateId mainHand = 0;
    ItemTemplateId offHand = 0;
    bool operator==(const VisibleEquipment&) const = default;
};
[[nodiscard]] VisibleEquipment visibleEquipment(const ItemCatalog& catalog, const Inventory& inv) noexcept;
// What a character whose kit has not been granted yet will wear (§1.4); {0, 0} without a kit.
[[nodiscard]] VisibleEquipment starterKitVisible(const ItemCatalog& catalog, std::uint8_t charClass) noexcept;

// ---- Rules (§1.1, §1.2) ----

struct ItemUser
{
    std::uint8_t charClass = 0;
    std::uint16_t level = 1;
};

// Whether `t` may be put into equipment slot `slot` by `user`, ignoring the rest of the inventory:
// WrongSlot (slot rule; every slot but MainHand/OffHand takes nothing in M8), WrongClass,
// LevelTooLow, else Ok. SlotBlocked / BagFull depend on the inventory: see planItemMove.
[[nodiscard]] ItemMoveResult checkEquip(const ItemTemplate& t, EquipSlot slot, const ItemUser& user) noexcept;

struct ItemSlotChange
{
    ItemPlace place;
    ItemStack content; // new content (empty = the slot is now empty)
    bool operator==(const ItemSlotChange&) const = default;
};

struct ItemMovePlan
{
    ItemMoveResult result = ItemMoveResult::ServerError;
    // On Ok: the new content of every changed slot (1..4), in this order: `to`, `from`, then for a
    // displaced off-hand item OffHand and the bag slot it went to. Empty otherwise.
    std::vector<ItemSlotChange> changes;

    // Where each moved item was and goes (for an atomic write-through), derived from `changes`.
    struct Relocation
    {
        ItemInstanceId id = 0;
        ItemPlace from;
        ItemPlace to;
        bool operator==(const Relocation&) const = default;
    };
    [[nodiscard]] std::vector<Relocation> relocations(const Inventory& before) const;
};

// Decides ITEM_MOVE_REQ(from, to) against `inv` without changing it. Check order: BadSlot (a place
// out of range, then from == to), NoItem (from empty or hidden), SlotBlocked (to holds a hidden
// item), the moved item's legality at `to` (checkEquip), the displaced item's legality at `from`
// (checkEquip; a bag slot always fits), SlotBlocked (an item would land in OffHand while MainHand
// holds a two-handed one), and when MainHand ends up two-handed with the off-hand item untouched,
// that item moves to the lowest free bag slot (BagFull if none, SlotBlocked if it is hidden).
[[nodiscard]] ItemMovePlan planItemMove(const ItemCatalog& catalog, const Inventory& inv, const ItemUser& user,
                                        ItemPlace from, ItemPlace to);
// Applies an Ok plan (no-op otherwise).
void applyItemMove(Inventory& inv, const ItemMovePlan& plan);

// Where a double click / right click sends an item (§6): for a bag item its natural equipment slot
// (main_hand/two_hand -> MainHand, off_hand -> OffHand, either_hand -> MainHand unless MainHand is
// occupied and OffHand is free), for an equipped item the first free bag slot. nullopt if `from` is
// empty, hidden or there is nowhere to go. The server still decides; this only picks `to`.
[[nodiscard]] std::optional<ItemPlace> quickMoveTarget(const ItemCatalog& catalog, const Inventory& inv,
                                                       ItemPlace from);

// ---- Starter kit placement (§1.3) ----

struct NewItem
{
    ItemPlace place;
    ItemTemplateId templateId = 0;
    std::uint16_t quantity = 1;
    bool operator==(const NewItem&) const = default;
};

// The rows granting `kit` creates next to the `existing` items (normally none): each equipped item
// goes to its slot if that is free, otherwise like a bag item; bag items take the lowest free bag
// slots; what does not fit is left out (`dropped` counts it).
[[nodiscard]] std::vector<NewItem> starterKitItems(const StarterKit& kit, const std::vector<StoredItem>& existing,
                                                   std::size_t* dropped = nullptr);

} // namespace vo::game
