// Client-side item upgrade logic (M16 "Tav", docs/PROTOCOL_UPGRADE.md §7), no D3D, no sockets: pure and
// unit-tested (tests/client/UpgradeViewTests.cpp). The server decides (UPGRADE_RES); these helpers keep the
// item put into the smith's "Yükselt" section and the one attempt in flight, compute what the section shows
// with the shared tables (game::planUpgrade and friends) and produce the Turkish texts of §7.
//
//   UpgradeSession      the chosen item (bag slot + item id), the attempt in flight, the last outcome
//   upgradePanelView()  what the section shows for the chosen item (stone, owned, fee, chance, failure)
//   upgradeConfirmText() the question before an attempt that can destroy the item (+6 and above)
//   upgradeResultText() the result / refusal lines
//   parseUpgradeCommand() the upgrade `scene` automation commands
#pragma once

#include "client/world/LootView.h"
#include "shared/game/Items.h"
#include "shared/game/Upgrade.h"
#include "shared/net/Messages.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vo::client::world {

// ---- The chosen item and the attempt in flight ----

class UpgradeSession
{
public:
    struct Request
    {
        std::uint32_t seq = 0;
        std::uint32_t npc = 0;
        std::uint8_t bagSlot = 0;
        std::uint64_t itemId = 0;
        std::uint8_t fromLevel = 0;
        std::uint16_t templateId = 0;
    };
    // What the last answered attempt did (the section's result line and effect).
    struct Outcome
    {
        vo::net::UpgradeResult result = vo::net::UpgradeResult::Success;
        std::uint16_t templateId = 0;
        std::uint8_t level = 0; // as UPGRADE_RES.level
        double at = -1e9;       // local time it arrived
    };

    // Puts the item of `bagSlot` into the section (false: empty slot). Replaces an earlier choice.
    bool pick(const game::Inventory& inv, std::uint8_t bagSlot);
    void clear() noexcept { m_slot.reset(); }
    // Keeps the choice in step with the bag: it follows nothing, an item that left its slot (moved, sold,
    // destroyed) is dropped from the section.
    void sync(const game::Inventory& inv) noexcept;
    [[nodiscard]] std::optional<std::uint8_t> slot() const noexcept { return m_slot; }
    [[nodiscard]] std::uint64_t itemId() const noexcept { return m_itemId; }

    [[nodiscard]] bool busy() const noexcept { return m_request.has_value(); }
    [[nodiscard]] const std::optional<Request>& request() const noexcept { return m_request; }
    // UPGRADE_REQ for the chosen item at the open smith `npc` (nullopt: nothing chosen / busy).
    std::optional<vo::net::UpgradeReq> begin(const game::Inventory& inv, std::uint32_t npc,
                                             std::uint8_t stoneSlot = vo::net::kUpgradeAnyStone);
    // Automation `upgradeforce`: a request as given (refusal tests), regardless of the choice.
    std::optional<vo::net::UpgradeReq> beginRaw(std::uint32_t npc, std::uint8_t bagSlot, std::uint64_t itemId,
                                                std::uint8_t fromLevel, std::uint8_t stoneSlot, std::uint16_t templateId);
    // UPGRADE_RES: the request it answers (nullopt: unknown seq, ignored). Records the outcome at `now`.
    std::optional<Request> finish(const vo::net::UpgradeRes& m, double now);
    void abandon() noexcept { m_request.reset(); }
    // The shop closed: the choice goes; with `forgetRequest` (a zone change) the attempt in flight too.
    void close(bool forgetRequest) noexcept;
    [[nodiscard]] const std::optional<Outcome>& outcome() const noexcept { return m_outcome; }
    void clearOutcome() noexcept { m_outcome.reset(); }

private:
    std::optional<std::uint8_t> m_slot;
    std::uint64_t m_itemId = 0;
    std::optional<Request> m_request;
    std::optional<Outcome> m_outcome;
    std::uint32_t m_nextSeq = 1;
};

// ---- What the section shows ----

struct UpgradePanelView
{
    bool hasItem = false;
    std::string name;             // "Eşik Kılıcı (+4)"
    std::uint32_t nameRgb = 0xEDE6D3;
    std::uint16_t templateId = 0;
    std::uint8_t level = 0;
    std::uint8_t target = 0;      // level + 1 (0 when it cannot be upgraded)
    std::string levelLine;        // "+4 → +5"
    std::string statLine;         // "Saldırı 18 → 19" (and / or "Savunma ..."); "" when nothing changes
    game::StoneGrade stone = game::StoneGrade::None;
    std::string stoneName;        // "Saf Tav Taşı"
    std::uint16_t stoneTemplate = 0;
    std::uint32_t stonesOwned = 0;
    std::uint32_t fee = 0;
    bool affordable = true;
    std::uint16_t chancePermille = 0;
    std::string chanceText;       // "%45"
    std::string failText;         // what a failure does
    bool failDestroys = false;
    bool canAttempt = false;      // every local check passes (the server still decides)
    std::string blockText;        // why not ("" when it can)
    bool needsConfirm = false;    // a failure would destroy it: ask first
};

// The section for the item in `bagSlot` (nullopt: nothing chosen - the hint).
[[nodiscard]] UpgradePanelView upgradePanelView(const game::ItemCatalog& catalog, const game::Inventory& inv,
                                                std::uint32_t coins, std::optional<std::uint8_t> bagSlot);

inline constexpr std::string_view kUpgradeHintText =
    "Yükseltmek için çantadan bir silah ya da zırh buraya sürükle ya da sağ tıkla.";
inline constexpr std::string_view kUpgradeTabBuy = "Satın al";
inline constexpr std::string_view kUpgradeTabUpgrade = "Yükselt";
inline constexpr std::string_view kUpgradeButton = "Yükselt";
inline constexpr std::string_view kUpgradeEquippedText = "Önce çıkarmalısın: yalnız çantadaki eşya yükseltilir.";

// "%45" (Turkish: the sign before the number), "%8", "%100".
[[nodiscard]] std::string chancePercentText(std::uint16_t permille);
// The question before a destroying attempt (+6 and above).
[[nodiscard]] std::string upgradeConfirmText(const UpgradePanelView& v);
// The line for UPGRADE_RES (`item` the template, may be nullptr; `level` as UPGRADE_RES.level).
[[nodiscard]] std::string upgradeResultText(vo::net::UpgradeResult r, const game::ItemTemplate* item, std::uint8_t level);
// The result line is an error / a loss (red) rather than good news.
[[nodiscard]] bool upgradeResultIsBad(vo::net::UpgradeResult r) noexcept;

// ---- Automation (the upgrade `scene` commands of WorldScene) ----

enum class UpgradeVerb : std::uint8_t
{
    Tab,          // shoptab buy|upgrade: as a click on the tab
    Pick,         // upgradepick <bagSlot>: as a right click on the bag item in the Yükselt tab
    Clear,        // upgradeclear: empties the section
    Start,        // upgradestart: as a click on "Yükselt" (asks first from +6 on)
    Force,        // upgradeforce <bagSlot> [stoneSlot|any] [fromLevel]: the request without local checks
    ExpectLevel,  // expectupgrade <bagSlot> <level>: the item in that bag slot is at this level
    ExpectPanel,  // expectupgradepanel <text>: the section shows a line containing <text>
    Log,          // upgradelog
};

struct UpgradeCommand
{
    UpgradeVerb verb = UpgradeVerb::Log;
    std::string text;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    bool hasB = false;
    bool hasC = false;
};

// nullopt with `error` empty: not an upgrade verb; nullopt with a message: a malformed command.
[[nodiscard]] std::optional<UpgradeCommand> parseUpgradeCommand(std::string_view line, std::string& error);

} // namespace vo::client::world
