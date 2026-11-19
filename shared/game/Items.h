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

#include "shared/game/Combat.h"
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
    Material,   // M10 (docs/PROTOCOL_LOOT.md §7): stackable, no equipment slot
    Consumable, // M14 (docs/PROTOCOL_SHOP.md §2): used from the bag or the skill bar (potions)
    Armor,      // M15 (docs/PROTOCOL_LEVELS.md §4): head / chest / legs / hands / feet, defense and attributes
};

// Consumables share one cooldown (docs/PROTOCOL_SHOP.md §2.3); template ids of consumables must fit
// a skill bar slot (0x8000 | id, id <= kMaxConsumableTemplateId).
inline constexpr std::uint32_t kConsumableCooldownMs = 10'000;
inline constexpr ItemTemplateId kMaxConsumableTemplateId = 0x7FFF;
inline constexpr std::uint32_t kMaxRestoreAmount = 100'000;

enum class SlotRule : std::uint8_t
{
    MainHand,   // main_hand
    OffHand,    // off_hand
    EitherHand, // either_hand: MainHand or OffHand
    TwoHand,    // two_hand: MainHand, and only while OffHand is empty
    None,       // materials: no equipment slot at all
    // M15: armour, exactly its own equipment slot.
    Head,  // head
    Chest, // chest
    Legs,  // legs
    Hands, // hands
    Feet,  // feet
};

// Presentation only (name colour); M10.
enum class Rarity : std::uint8_t
{
    Common,   // common
    Uncommon, // uncommon
    Rare,     // rare
    Unique,   // unique (M15): boss drops, "Eşsiz"
};

inline constexpr std::uint16_t kMaxStackLimit = 999; // largest max_stack a template may declare

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
    SlotRule slot = SlotRule::MainHand; // None for materials and consumables
    std::uint8_t classMask = 0; // bit (1 << CharClass value) per allowed class
    std::uint16_t level = 1;    // required character level
    std::int32_t attack = 0;
    std::int32_t defense = 0;
    std::string model; // mesh id (weapons/<model>.vmesh); empty for materials
    std::string icon;  // ui/items/<icon>.vtex
    Grip grip = Grip::Sword; // meaningless for materials
    // M10 (docs/PROTOCOL_LOOT.md §7).
    std::uint16_t maxStack = 1; // units per bag slot; 1 = not stackable (weapons, shields)
    std::uint32_t value = 0;    // Akçe: the merchants' buy price; sold for half (PROTOCOL_SHOP.md §2.1); 0 = unsellable
    Rarity rarity = Rarity::Common;
    // M14 (docs/PROTOCOL_SHOP.md §2.3), consumables only: hp / mp restored at once when used.
    std::uint32_t restoreHp = 0;
    std::uint32_t restoreMp = 0;
    // M11 (docs/PROTOCOL_STATS.md §5): attribute bonuses while equipped (0..50 each) and the base
    // attribute values (class start + allocated, 0..255) needed to equip it. Weapons and shields only.
    Attributes attributes{};
    Attributes requirements{};
    // M15: presentation tint of the icon and the weapon mesh (multiplied; 255 = unchanged). Lets items
    // share art until their own exists (docs/PROTOCOL_LEVELS.md §4).
    std::array<std::uint8_t, 3> tint{255, 255, 255};
    // M16 (docs/PROTOCOL_UPGRADE.md §2), materials only: the grade of upgrade stone this is (1 Ham,
    // 2 Saf, 3 Usta); 0 = not a stone.
    std::uint8_t upgradeStone = 0;

    [[nodiscard]] bool hasRequirements() const noexcept
    {
        for (const auto v : requirements)
            if (v != 0)
                return true;
        return false;
    }
    [[nodiscard]] bool allowsClass(std::uint8_t charClass) const noexcept
    {
        return charClass < 8 && (classMask & (1u << charClass)) != 0;
    }
    [[nodiscard]] bool isTwoHanded() const noexcept { return slot == SlotRule::TwoHand; }
    [[nodiscard]] bool isStackable() const noexcept { return maxStack > 1; }
    [[nodiscard]] bool isConsumable() const noexcept { return kind == ItemKind::Consumable; }
    [[nodiscard]] bool isArmor() const noexcept { return kind == ItemKind::Armor; }
    [[nodiscard]] bool isTinted() const noexcept { return tint != std::array<std::uint8_t, 3>{255, 255, 255}; }
    // An item that can be equipped (weapon, shield, armour).
    [[nodiscard]] bool isEquippable() const noexcept { return slot != SlotRule::None; }
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
[[nodiscard]] std::string_view slotRuleDisplayName(SlotRule r) noexcept; // "" for SlotRule::None
[[nodiscard]] std::string_view rarityName(Rarity r) noexcept;            // "common", "uncommon", "rare"
[[nodiscard]] std::string_view rarityDisplayName(Rarity r) noexcept;     // "Sıradan", "Seyrek", "Nadir"

// ---- Inventory ----

// M16 (docs/PROTOCOL_UPGRADE.md): the upgrade level of one item instance, 0..10.
inline constexpr std::uint8_t kMaxUpgradeLevel = net::kMaxUpgradeLevel;

struct ItemStack
{
    ItemInstanceId id = 0; // 0 = empty slot
    ItemTemplateId templateId = 0;
    std::uint16_t quantity = 0;
    std::uint8_t upgrade = 0; // M16: 0..10 (weapons, shields, armour; always 0 for stackables)

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
    std::uint8_t upgrade = 0; // M16: 0..10
    bool operator==(const StoredItem&) const = default;
};

class Inventory
{
public:
    // Builds an inventory from stored rows. False (with `error`) for an invalid place, two rows on
    // one place, a zero item id or quantity, an upgrade level above 10 or an upgraded stack of more
    // than one unit; unknown templates are accepted (hidden items).
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
    // M11: base attributes (class start + allocated, without item bonuses) for item requirements.
    Attributes attributes{};
};

// Whether `t` may be put into equipment slot `slot` by `user`, ignoring the rest of the inventory:
// WrongSlot (slot rule; every slot but MainHand/OffHand takes nothing in M8), WrongClass,
// LevelTooLow, AttributeTooLow (M11: a requirement above the user's base value), else Ok. SlotBlocked / BagFull depend on the inventory: see planItemMove.
[[nodiscard]] ItemMoveResult checkEquip(const ItemTemplate& t, EquipSlot slot, const ItemUser& user) noexcept;

struct ItemSlotChange
{
    ItemPlace place;
    ItemStack content; // new content (empty = the slot is now empty)
    bool operator==(const ItemSlotChange&) const = default;
};

// A quantity change of one stored stack (docs/PROTOCOL_LOOT.md §1.2, §6): it must hold `before`
// units and ends with `after` (0 = the row is deleted).
struct ItemQuantityChange
{
    ItemInstanceId id = 0;
    ItemPlace place;
    std::uint16_t before = 0;
    std::uint16_t after = 0;
    bool operator==(const ItemQuantityChange&) const = default;
};

struct ItemMovePlan
{
    ItemMoveResult result = ItemMoveResult::ServerError;
    // On Ok: the new content of every changed slot (1..4), in this order: `to`, `from`, then for a
    // displaced off-hand item OffHand and the bag slot it went to. Empty otherwise.
    std::vector<ItemSlotChange> changes;
    // On an Ok merge of two stacks (PROTOCOL_LOOT.md §1.2): the target's growth first, then the
    // source's shrink (after 0 = deleted). Empty for plain moves and swaps.
    std::vector<ItemQuantityChange> merge;

    [[nodiscard]] bool isMerge() const noexcept { return !merge.empty(); }

    // Where each moved item was and goes (for an atomic write-through), derived from `changes`.
    // Empty for a merge (nothing changes place).
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
// item), a merge (bag -> bag, same stackable template, target not full: Ok with `merge` set), the
// moved item's legality at `to` (checkEquip), the displaced item's legality at `from`
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

// ---- Adding units (loot pickup, vo_admin give-item; PROTOCOL_LOOT.md §1.2) ----

struct ItemAddRequest
{
    ItemTemplateId templateId = 0;
    std::uint32_t count = 0;
};

struct ItemAddPlan
{
    std::vector<ItemQuantityChange> grown; // existing stacks that grow (each once, after = final size)
    std::vector<NewItem> created;          // new stacks in free bag slots (lowest first)
    std::vector<std::uint32_t> placed;     // per request: units placed (the rest did not fit)

    [[nodiscard]] bool empty() const noexcept { return grown.empty() && created.empty(); }
    [[nodiscard]] std::uint32_t totalPlaced() const noexcept;
    // The bag slot changes this plan makes, as (place, new content): grown stacks first, then the
    // created ones with the ids the database assigned (`createdIds`, same order as `created`).
    [[nodiscard]] std::vector<ItemSlotChange> changes(const Inventory& before,
                                                      const std::vector<ItemInstanceId>& createdIds) const;
};

// Places the requests in order against `inv` without changing it: each request first fills bag
// stacks of the same template below max_stack (lowest slot first, stacks created by earlier
// requests included), then the lowest free bag slots (at most max_stack per new stack). Unknown
// templates, count 0 and hidden items are never touched (placed 0). Equipment is never used.
[[nodiscard]] ItemAddPlan planItemAdd(const ItemCatalog& catalog, const Inventory& inv,
                                      const std::vector<ItemAddRequest>& requests);
// Applies a plan whose created stacks got `createdIds` (same order as plan.created).
void applyItemAdd(Inventory& inv, const ItemAddPlan& plan, const std::vector<ItemInstanceId>& createdIds);

// The rows granting `kit` creates next to the `existing` items (normally none): each equipped item
// goes to its slot if that is free, otherwise like a bag item; bag items take the lowest free bag
// slots; what does not fit is left out (`dropped` counts it).
[[nodiscard]] std::vector<NewItem> starterKitItems(const StarterKit& kit, const std::vector<StoredItem>& existing,
                                                   std::size_t* dropped = nullptr);

} // namespace vo::game
