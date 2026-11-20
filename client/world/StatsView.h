// Client-side character sheet logic (M11 "Künye", docs/PROTOCOL_STATS.md §7), no D3D, no sockets:
// everything here is pure and unit-tested (tests/client/StatsViewTests.cpp). The server stays
// authoritative (CHAR_STATS); these helpers keep the player's not yet confirmed ("pending") points,
// preview the derived values with the shared game::deriveStats and produce the Turkish texts.
//
//   StatSheet            the last CHAR_STATS + the pending allocation + the request in flight
//   statResultText()     STAT_ALLOCATE_RES results as short Turkish messages
//   attributeTooltip()   what an attribute does (for the character's class)
//   derivedRows()        the "Değerler" rows of the window (label, value, preview)
//   toCharStats()        game::Stats in CHAR_STATS units (permille, tenths) - the preview's numbers
//   formatDecimal()      12.5 -> "12,5" (Turkish decimal comma)
//   parseStatsCommand()  the Künye `scene` automation commands (syntax errors name the problem)
#pragma once

#include "client/world/LootView.h"
#include "shared/game/Combat.h"
#include "shared/net/Messages.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::client::world {

// ---- The sheet ----

class StatSheet
{
public:
    // Forget everything (world entry).
    void reset() noexcept;

    // CHAR_STATS: the authoritative values. Pending points are kept but trimmed to what is still
    // allowed (unspent, the 255 cap); after an accepted request (Ok) the first CHAR_STATS ends the
    // wait and drops the pending points (they are now part of `base`).
    void applyCharStats(const vo::net::CharStats& m);
    [[nodiscard]] bool known() const noexcept { return m_known; }
    [[nodiscard]] const vo::net::CharStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] std::uint16_t level() const noexcept { return m_stats.level; }
    [[nodiscard]] std::uint16_t unspent() const noexcept { return m_stats.unspent; }
    [[nodiscard]] std::uint16_t base(std::size_t i) const noexcept { return m_stats.attributes[i].base; }
    [[nodiscard]] std::uint16_t bonus(std::size_t i) const noexcept { return m_stats.attributes[i].bonus; }
    [[nodiscard]] game::Attributes baseValues() const noexcept;
    [[nodiscard]] game::Attributes bonusValues() const noexcept;
    // base - the class start (what the player spent), never below 0.
    [[nodiscard]] game::Attributes allocated(std::uint8_t charClass) const noexcept;

    // ---- Pending points ----
    [[nodiscard]] std::uint16_t pending(std::size_t i) const noexcept { return m_pending[i]; }
    [[nodiscard]] const game::Attributes& pendingValues() const noexcept { return m_pending; }
    [[nodiscard]] std::uint32_t pendingTotal() const noexcept;
    [[nodiscard]] bool hasPending() const noexcept { return pendingTotal() > 0; }
    // unspent - pending (what "+" may still add).
    [[nodiscard]] std::uint16_t remaining() const noexcept;
    // "+" is usable: known, not busy, points left, base + pending below 255.
    [[nodiscard]] bool canAdd(std::size_t i) const noexcept;
    // "−" is usable: not busy and something pending on that row.
    [[nodiscard]] bool canRemove(std::size_t i) const noexcept;
    // Adds up to `n` pending points (clamped by remaining() and the cap); returns how many.
    std::uint16_t add(std::size_t i, std::uint16_t n = 1) noexcept;
    // Removes up to `n` pending points; returns how many.
    std::uint16_t remove(std::size_t i, std::uint16_t n = 1) noexcept;
    // "Vazgeç": drops every pending point (not while busy).
    bool clearPending() noexcept;

    // ---- The request ----
    // "Onayla": the request for the pending points (nullopt when nothing is pending or one is
    // already in flight / waiting for its CHAR_STATS). `now` in seconds (wait timeout).
    std::optional<vo::net::StatAllocateReq> beginConfirm(double now);
    struct Answer
    {
        vo::net::StatAllocateResult result = vo::net::StatAllocateResult::Ok;
        bool matched = false; // the seq was the one in flight
    };
    // STAT_ALLOCATE_RES: Ok keeps the points pending (shown as the preview) until the next CHAR_STATS
    // replaces them; a refusal keeps them too, trimmed to the answer's `unspent`.
    Answer finish(const vo::net::StatAllocateRes& m, double now);
    // Busy: a request in flight, or an accepted one waiting for its CHAR_STATS (at most `timeout`
    // seconds; after that the pending points are dropped and the sheet is usable again).
    [[nodiscard]] bool busy() const noexcept { return m_inFlight || m_awaitingStats; }
    [[nodiscard]] bool inFlight() const noexcept { return m_inFlight; }
    [[nodiscard]] bool awaitingStats() const noexcept { return m_awaitingStats; }
    void expire(double now, double timeout = 3.0) noexcept;

    // ---- Preview ----
    // deriveStats with the spent points + the pending ones; `equip` gives the weapon / armour sums
    // (its attribute bonuses are replaced by CHAR_STATS' bonus values).
    [[nodiscard]] game::Stats preview(std::uint8_t charClass, game::EquipBonus equip) const noexcept;
    // The same without the pending points (should equal what CHAR_STATS says).
    [[nodiscard]] game::Stats current(std::uint8_t charClass, game::EquipBonus equip) const noexcept;

private:
    void trimPending() noexcept;

    bool m_known = false;
    vo::net::CharStats m_stats;
    game::Attributes m_pending{};
    bool m_inFlight = false;
    bool m_awaitingStats = false;
    std::uint32_t m_seq = 0;     // of the request in flight
    std::uint32_t m_nextSeq = 1;
    double m_answeredAt = 0.0;
};

// ---- Text ----

// Turkish message for a result ("" for Ok).
[[nodiscard]] std::string_view statResultText(vo::net::StatAllocateResult r) noexcept;
// What the attribute does (PROTOCOL_STATS.md §7), plus what it means for this class ("Savaşçı olarak
// saldırı gücün bu niteliğe bağlı." / "... saldırı gücünü artırmaz.").
[[nodiscard]] std::string attributeTooltip(game::Attribute a, std::uint8_t charClass);
// 12.5 -> "12,5" (one decimal, decimal comma).
[[nodiscard]] std::string formatDecimal(double value);
// "Dağıtılmamış puan: 3".
[[nodiscard]] std::string unspentText(std::uint16_t n);
// Level-up toast line: "+3 nitelik puanı  ·  Künye: C" (`points` gained).
[[nodiscard]] std::string levelUpPointsText(std::uint32_t points);
// Automation / log text of one attribute row: "Güç 14 (+2) +3" (bonus and pending only if > 0).
[[nodiscard]] std::string attributeRowText(game::Attribute a, std::uint16_t base, std::uint16_t bonus,
                                           std::uint16_t pending);

// game::Stats in CHAR_STATS units (the server's conversion: tenths rounded, heal of Soluk Güneş).
[[nodiscard]] vo::net::CharStats toCharStats(const game::Stats& s, std::uint16_t level);

enum class DerivedField : std::uint8_t
{
    MaxHp,
    MaxMp,
    Attack,   // M18: "48–58" with a weapon damage range
    BasicHit, // M18: a basic attack before defence: attack range x the weapon's speed factor
    Defense,
    Crit,
    Dodge,
    HpRegen,
    MpRegen,
    AttackSpeed,
    Reach, // M18: basic attack reach in metres
    Heal,  // priests only
};
inline constexpr std::size_t kDerivedFieldCount = 12;
// "maxhp", "maxmp", "attack", "hit", "defense", "crit", "dodge", "hpregen", "mpregen", "speed", "reach",
// "heal".
[[nodiscard]] std::string_view derivedFieldKey(DerivedField f) noexcept;
[[nodiscard]] std::optional<DerivedField> parseDerivedField(std::string_view key) noexcept;
// The value of a field in display units (hp, permille / 10 as percent, tenths / 10 per second, ...).
[[nodiscard]] double derivedValue(const vo::net::CharStats& s, DerivedField f) noexcept;

struct DerivedRow
{
    DerivedField field = DerivedField::MaxHp;
    std::string label;   // "Can", "Saldırı Gücü" / "Büyü Gücü", "Kritik Vuruş", ...
    std::string value;   // "120", "%5,0", "2,4/sn", "1,6 sn"
    std::string preview; // "" when unchanged, else the previewed value
    std::string bonus;   // M18: the equipment's bonus line in it ("+40", "+%1,0"), "" = none
};
// The derived rows of a class (a Priest gets "İyileştirme" too). `preview` (with pending points) may
// be nullptr: no preview column.
[[nodiscard]] std::vector<DerivedRow> derivedRows(std::uint8_t charClass, const vo::net::CharStats& now,
                                                  const vo::net::CharStats* preview);
// Automation text: one row per line, "Can 120 → 135" (M18: "Can 160 (+40)").
[[nodiscard]] std::string derivedRowsText(const std::vector<DerivedRow>& rows);

// M18: the totals line under the inventory's bag: "Saldırı Gücü 48–58 · Savunma 45 · Hız 1,6 sn" (a
// Mage: "Büyü Gücü").
[[nodiscard]] std::string equipmentSummaryText(std::uint8_t charClass, const vo::net::CharStats& s);
// "48–58", or "52" without a range.
[[nodiscard]] std::string rangeText(std::uint32_t low, std::uint32_t high);

// Attribute by automation name: the items.toml keys ("strength", ...) or ASCII Turkish ("guc",
// "ceviklik", "dayaniklilik", "zeka", "irade").
[[nodiscard]] std::optional<game::Attribute> parseAttributeName(std::string_view s) noexcept;

// ---- Input ----

// Esc closes the loot window first, then the Künye, then the inventory, then drops the target, then
// opens the menu (None while the menu is open). Extends world::escapeAction (LootView.h).
[[nodiscard]] EscapeAction escapeActionWithKunye(bool lootOpen, bool kunyeOpen, bool inventoryOpen, bool hasTarget,
                                                 bool menuOpen) noexcept;

// ---- Automation (the Künye `scene` commands of WorldScene) ----

enum class StatsVerb : std::uint8_t
{
    Kunye,          // kunye open|close
    Add,            // statadd <attr> [n]: as n clicks on "+" (Shift+click: n = 5)
    Remove,         // statremove <attr> [n]
    ExpectAttr,     // expectattr <attr> <base> [bonus]: CHAR_STATS base (and bonus)
    ExpectUnspent,  // expectunspent <n>
    ExpectPending,  // expectpending <attr> <n>
    ExpectDerived,  // expectderived <field> <value>: CHAR_STATS value in display units (maxhp 120, crit 5.5)
    ExpectPreview,  // expectpreview <field> <value>: the previewed value with the pending points
    ExpectMessage,  // expectstatmessage <text>: the window's last message contains <text>
    Log,            // statlog
};

struct StatsCommand
{
    StatsVerb verb = StatsVerb::Log;
    std::string mode; // kunye: open / close
    std::string text; // expectstatmessage
    game::Attribute attribute = game::Attribute::Strength;
    DerivedField field = DerivedField::MaxHp;
    std::uint32_t a = 0;  // n / base
    std::int32_t b = -1;  // bonus (-1: not checked)
    double value = 0.0;   // expectderived / expectpreview
};

// nullopt with `error` empty: not a stats verb; nullopt with a message: a malformed stats command.
[[nodiscard]] std::optional<StatsCommand> parseStatsCommand(std::string_view line, std::string& error);

} // namespace vo::client::world
