// Client-side loot presentation logic (M10 "Ganimet", docs/PROTOCOL_LOOT.md §9), no D3D, no sockets:
// everything here is pure and unit-tested (tests/client/LootViewTests.cpp). The server stays
// authoritative; these helpers decide what the client shows and which request it sends.
//
//   lootLabel()           mine / locked to someone else / free for all, from the entity state bits
//   lootLabelStyle()      label colour, ring and mesh tint of each of those
//   formatThousands()     1234567 -> "1.234.567" (Turkish digit grouping)
//   lootResultText()      LOOT_OPEN_RES / LOOT_TAKE_RES results as short Turkish messages
//   lootClosedText()      LOOT_CLOSED reasons
//   rarityRgb()           item name colour by rarity
//   LootWindowModel       contents of the open loot window (rows, take requests in flight)
//   inventoryGains()      units gained per template between two inventories (pickup feedback)
//   pickupText()          "+2 Kül Tozu" / "+7 Akçe"
//   itemTipLines()        the tooltip of an item (inventory slots and loot rows)
//   escapeAction()        what Esc does: loot window, then inventory, then target, then the menu
//   nearestLoot()         the bag the interact key (E) opens
//   parseLootCommand()    the loot `scene` automation commands (syntax errors name the problem)
#pragma once

#include "client/world/WorldMath.h"
#include "shared/game/Items.h"
#include "shared/net/Messages.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::client::world {

// ---- World presentation of a loot container (entity_type 3) ----

enum class LootLabel : std::uint8_t
{
    Mine,   // LootMine: the owner window runs and we are the owner
    Locked, // LootLocked: the owner window runs for somebody else
    Free,   // neither bit: anybody may take
};
[[nodiscard]] LootLabel lootLabel(std::uint8_t entityState) noexcept;
[[nodiscard]] const char* lootLabelName(LootLabel l) noexcept; // "mine", "locked", "free"

struct LootLabelStyle
{
    std::uint32_t labelRgb = 0xFFFFFF;
    float labelAlpha = 1.0f;
    std::uint32_t ringRgb = 0xFFFFFF; // ground ring under the bag
    float ringAlpha = 0.0f;           // 0: no ring
    bool ringPulse = false;           // the ring breathes (own bag)
    float meshTint = 1.0f;            // multiplies the bag's base colour (locked: dimmed)
};
[[nodiscard]] LootLabelStyle lootLabelStyle(LootLabel l) noexcept;

// Distances (metres, horizontal) of the client's loot handling.
inline constexpr float kLootRange = 3.0f;        // server: 3 m + 0.5 m tolerance
inline constexpr float kLootWalkStop = 2.2f;     // walking to a bag stops this close
inline constexpr float kLootAutoClose = 4.5f;    // the window closes itself beyond this
inline constexpr float kLootInteractSearch = 12.0f; // E looks for bags this far

// ---- Text ----

// 1234567 -> "1.234.567"; 999 -> "999".
[[nodiscard]] std::string formatThousands(std::uint64_t value);
// Turkish message for a result ("" for Ok).
[[nodiscard]] std::string_view lootResultText(vo::net::LootResult r) noexcept;
// Turkish message for LOOT_CLOSED ("" when nothing needs saying: emptied, died).
[[nodiscard]] std::string_view lootClosedText(vo::net::LootClosedReason r) noexcept;
// 0xRRGGBB of an item name: common warm white, uncommon green, rare blue.
[[nodiscard]] std::uint32_t rarityRgb(game::Rarity r) noexcept;

// ---- Loot window ----

struct LootRow
{
    bool coins = false;           // the Akçe row (take what = kLootTakeCoins)
    std::uint8_t index = 0;       // entry index 0..7 (what = index)
    std::uint16_t templateId = 0; // 0 for the Akçe row
    std::uint32_t quantity = 0;   // units, or the coin amount
    [[nodiscard]] std::uint8_t what() const noexcept { return coins ? vo::net::kLootTakeCoins : index; }
};

class LootWindowModel
{
public:
    struct Take
    {
        std::uint32_t seq = 0;
        std::uint32_t lootId = 0;
        std::uint8_t what = 0;
    };

    // The window shows `lootId` from now on (contents unknown until applyContents).
    void open(std::uint32_t lootId);
    // Forgets the container (takes in flight are kept: their answers still come).
    void close() noexcept;
    // LOOT_CONTENTS: false (ignored) if it is for another container than the open one.
    bool applyContents(const vo::net::LootContents& m);

    [[nodiscard]] bool isOpen() const noexcept { return m_lootId != 0; }
    [[nodiscard]] std::uint32_t lootId() const noexcept { return m_lootId; }
    [[nodiscard]] bool hasContents() const noexcept { return m_hasContents; }
    [[nodiscard]] std::uint32_t coins() const noexcept { return m_coins; }
    [[nodiscard]] std::span<const vo::net::LootEntry> entries() const noexcept { return m_entries; }
    // The Akçe row first (only if coins > 0), then the entries by index.
    [[nodiscard]] std::vector<LootRow> rows() const;
    [[nodiscard]] bool empty() const noexcept { return m_coins == 0 && m_entries.empty(); }
    // The row a take of `what` refers to, if it is listed now.
    [[nodiscard]] std::optional<LootRow> row(std::uint8_t what) const;

    // A take request: its seq (client-chosen, never 0) and bookkeeping until LOOT_TAKE_RES.
    std::uint32_t beginTake(std::uint8_t what);
    // LOOT_TAKE_RES: the take it answers (nullopt for an unknown seq).
    std::optional<Take> finishTake(std::uint32_t seq);
    [[nodiscard]] std::size_t takesInFlight() const noexcept { return m_takes.size(); }

private:
    std::uint32_t m_lootId = 0;
    bool m_hasContents = false;
    std::uint32_t m_coins = 0;
    std::vector<vo::net::LootEntry> m_entries;
    std::vector<Take> m_takes;
    std::uint32_t m_nextSeq = 1;
};

// ---- Pickup feedback ----

struct ItemGain
{
    std::uint16_t templateId = 0;
    std::uint32_t units = 0;
    bool operator==(const ItemGain&) const = default;
};
// Units per template that `after` holds more of than `before` (bag and equipment), by template id.
// Moves and merges change nothing; only additions show up.
[[nodiscard]] std::vector<ItemGain> inventoryGains(const game::Inventory& before, const game::Inventory& after);
// "+2 Kül Tozu", "+1 Kor Çekirdeği" (fix1: docs/PROTOCOL_SHOP.md §8.5; loot and purchases); unknown templates "+1 Eşya #N".
[[nodiscard]] std::string pickupText(const game::ItemTemplate* item, std::uint16_t templateId, std::uint32_t units);
// "+7 Akçe" (thousands grouped).
[[nodiscard]] std::string coinPickupText(std::uint32_t gained);

// ---- Item tooltip (inventory and loot window) ----

enum class TipTone : std::uint8_t
{
    Title,  // the name, in its rarity colour (rgb)
    Normal,
    Dim,
    Good,   // attack / defense bonus
    Bad,    // a requirement the character does not meet
    Value,  // the Akçe value
};
struct TipLine
{
    std::string text;
    TipTone tone = TipTone::Normal;
    std::uint32_t rgb = 0; // Title: the rarity colour
    bool wrap = false;
};
// Lines of an item's tooltip: name (rarity colour); "kind · slot · rarity" (materials: "Malzeme ·
// rarity"); "Adet: N / max" for stackable items; attack / defense; M11 attribute bonuses ("Güç +2",
// Good); classes and level (weapons and shields only; unmet ones Bad); M11 requirements ("Gerekli
// Güç: 20", Bad when user.attributes is lower); value in Akçe (per unit, and for the stack); description.
// `quantity` 0 means "not a concrete stack" (no Adet line).
[[nodiscard]] std::vector<TipLine> itemTipLines(const game::ItemTemplate& item, const game::ItemUser& user,
                                                std::uint32_t quantity);

// ---- Input ----

enum class EscapeAction : std::uint8_t
{
    None,
    CloseLoot,
    CloseShop,  // M14: the shop window (ShopView.h escapeActionShop)
    CloseKunye, // M11: the character sheet (StatsView.h escapeActionWithKunye)
    CloseSkills, // M12: the "Yetenekler" window (SkillsView.h escapeActionWithSkills)
    CloseControls, // HUD: the "Tuşlar" window (HudView.h escapeActionHud)
    CloseInventory,
    ClearTarget,
    OpenMenu,
};
// Esc closes the loot window first, then the inventory, then drops the target, then opens the menu
// (None while the menu is open: the menu layer handles its own Esc).
[[nodiscard]] EscapeAction escapeAction(bool lootOpen, bool inventoryOpen, bool hasTarget, bool menuOpen) noexcept;

struct LootCandidate
{
    std::uint32_t id = 0;
    Vec3 position{};
    std::uint8_t state = 0; // entity state bits
};
// The nearest container within `maxDistance` (horizontal) that we may take from (locked ones are
// skipped unless `includeLocked`); 0 if none. Ties: the lower id.
[[nodiscard]] std::uint32_t nearestLoot(std::span<const LootCandidate> candidates, Vec3 from, float maxDistance,
                                        bool includeLocked) noexcept;

// ---- Automation (the loot `scene` commands of WorldScene) ----

enum class LootVerb : std::uint8_t
{
    Nearest,       // lootnearest [any]: as the E key (nearest bag, walking into range); any = locked too
    WalkTo,        // walktoloot [mine|locked|free|any]: path-found walk next to the nearest such bag
    TakeAll,       // takeall: the "Hepsini Al" button
    Take,          // take <index 0..7>|coins: one row of the open window
    Close,         // lootclose: as Esc on the loot window
    ExpectCoins,   // expectcoins <op> <n>: the Akçe balance (op: = == >= <= > <)
    ExpectItem,    // expectitem <templateId> [<op>] <n>: units of a template in the bag (default op =)
    ExpectStack,   // expectstack <bagSlot> <templateId> <quantity>: one bag slot's stack
    ExpectRows,    // expectlootrows <n>: rows of the open loot window
    ExpectMessage, // expectlootmessage <text>: the last loot message contains <text>
    Log,           // lootlog: write the loot state to the log
    Farm,          // lootfarm <monster name> <templateId> <pickups>: hunt such monsters and loot every
                   // own / free bag until <pickups> pickups of that template happened;
                   // lootfarm <monster name> rows <n>: ... until a bag with >= n rows is open (left
                   // open, nothing taken from it); lootfarm stop
    MoveStack,     // movestack <templateId> <bagSlot>: move the first stack of a template to a bag slot
};

enum class CompareOp : std::uint8_t
{
    Eq,
    Ge,
    Le,
    Gt,
    Lt,
};
[[nodiscard]] bool compare(CompareOp op, std::uint64_t value, std::uint64_t n) noexcept;
[[nodiscard]] const char* compareOpText(CompareOp op) noexcept;

struct LootCommand
{
    LootVerb verb = LootVerb::Log;
    std::string mode;   // nearest: "" / "any"; walktoloot: mine / locked / free / any
    std::string text;   // expectlootmessage; lootfarm: the monster name
    CompareOp op = CompareOp::Eq;
    std::uint32_t a = 0; // take: index (or kLootTakeCoins); expect*: template / slot / n
    std::uint32_t b = 0; // expectitem / expectstack: n / template
    std::uint32_t c = 0; // expectstack: quantity
};

// Parses one loot scene command. nullopt with `error` empty: not a loot verb (the scene tries its
// other commands); nullopt with a message: a malformed loot command.
[[nodiscard]] std::optional<LootCommand> parseLootCommand(std::string_view line, std::string& error);

} // namespace vo::client::world
