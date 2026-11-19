// Client-side quest logic (M17, docs/PROTOCOL_QUESTS.md §7), no D3D, no sockets: pure and unit-tested
// (tests/client/QuestViewTests.cpp). The server stays authoritative (QUEST_LIST, QUEST_UPDATE, QUEST_RES);
// these helpers keep the character's quest rows on the client clock, the one request in flight, what an NPC
// offers in its dialog, the tracker and log lines, the completion notices, and the Turkish texts.
//
//   QuestBook          the rows of QUEST_LIST / QUEST_UPDATE (cooldowns on the client's clock), notices
//   QuestRequests      the one quest request in flight (seq, action, quest), its pacing (4 a second)
//   npcQuestEntries()  what an NPC's dialog lists (offered, in progress, ready to hand in), in order
//   objectiveText() .. "Kül Çömezi 3/8", "Kül Tozu 2/5", "Avcı Selvi ile konuş"
//   questResultText()  the refusal lines of §7.3
//   parseQuestCommand() the quest `scene` automation commands
#pragma once

#include "client/world/LootView.h"
#include "shared/game/Items.h"
#include "shared/game/Quests.h"
#include "shared/net/Messages.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::game {
class NpcCatalog;
}

namespace vo::client::world {

// ---- Presentation (docs/PROTOCOL_QUESTS.md §7.1) ----

// The three marker states over an NPC, drawn procedurally (an original look: a faceted köz gem on a short
// stem): Available = amber gem with a bright core, InProgress = a hollow ash-grey gem, Ready = a green-gold
// gem with a halo that pulses. All three keep their shape at distance (silhouette, outline, colour).
inline constexpr std::uint32_t kQuestAvailableRgb = 0xF2B33D;
inline constexpr std::uint32_t kQuestProgressRgb = 0xA7AEB8;
inline constexpr std::uint32_t kQuestReadyRgb = 0x8EE06A;
inline constexpr float kQuestMarkerMinPx = 26.0f; // logical pixels on screen
inline constexpr float kQuestMarkerMaxPx = 44.0f;
inline constexpr float kQuestMarkerHidden = 70.0f; // metres: farther, no marker
inline constexpr std::size_t kTrackerMaxQuests = 5; // the HUD tracker shows at most this many quests
inline constexpr double kQuestNoticeSeconds = 4.0;

// ---- The character's quests ----

struct QuestNotice
{
    enum class Kind : std::uint8_t
    {
        Accepted,
        ObjectiveDone, // one objective reached its count
        Ready,         // every objective met: back to the turn-in NPC
        HandedIn,
        Abandoned,
    };
    Kind kind = Kind::Accepted;
    game::QuestId quest = 0;
    std::string text; // the line shown ("Görev tamamlandı: Kül Tozu — Avcı Selvi'ye dön")
};

class QuestBook
{
public:
    // QUEST_LIST at client time `now` (seconds): every row replaced.
    void apply(const vo::net::QuestList& m, double now);
    // QUEST_UPDATE: one row changed (None: gone). Kill objectives that reach their count, and a quest that
    // becomes ready, produce notices (collect objectives: see noteInventory).
    void apply(const vo::net::QuestUpdate& m, double now, const game::Inventory& inv, const game::ItemCatalog* items);
    // The bag changed (collect objectives): notices for objectives / quests that are now complete.
    void noteInventory(const game::Inventory& inv, const game::ItemCatalog* items);
    void clear() noexcept;

    [[nodiscard]] bool known() const noexcept { return m_known; }
    [[nodiscard]] const game::QuestLog& log() const noexcept { return m_log; }
    // Whole seconds of the client clock (the rules' `now`).
    [[nodiscard]] static std::int64_t clock(double now) noexcept { return static_cast<std::int64_t>(now); }
    // Active quests in acceptance order (oldest first; the tracker and the log list them so).
    [[nodiscard]] std::vector<game::QuestId> activeInOrder() const;
    // Notices produced since the last call.
    [[nodiscard]] std::vector<QuestNotice> takeNotices();
    [[nodiscard]] std::uint32_t updates() const noexcept { return m_updates; }

private:
    void noteReadiness(game::QuestId id, const game::Inventory& inv, const game::ItemCatalog* items, bool announce);

    game::QuestLog m_log;
    bool m_known = false;
    std::vector<game::QuestId> m_order;                // active quests, oldest first
    std::vector<game::QuestId> m_ready;                // active quests known ready (notice once)
    std::vector<std::pair<game::QuestId, std::uint8_t>> m_doneObjectives; // (quest, objective) noticed
    std::vector<QuestNotice> m_notices;
    std::uint32_t m_updates = 0;
};

// One quest request in flight at a time (the dialog's buttons wait meanwhile); sends paced to 4 a second.
class QuestRequests
{
public:
    struct Pending
    {
        std::uint32_t seq = 0;
        vo::net::QuestAction action = vo::net::QuestAction::Accept;
        game::QuestId quest = 0;
        std::uint32_t npc = 0;
    };
    [[nodiscard]] bool busy() const noexcept { return m_pending.has_value(); }
    [[nodiscard]] const std::optional<Pending>& pending() const noexcept { return m_pending; }
    // False while busy or above 4 sends in the last second (`now`: seconds).
    [[nodiscard]] bool canSend(double now) const noexcept;
    std::uint32_t begin(vo::net::QuestAction action, game::QuestId quest, std::uint32_t npc, double now);
    // QUEST_RES: the request it answers (nullopt: not ours / stale).
    std::optional<Pending> finish(const vo::net::QuestRes& m);
    void forget() noexcept { m_pending.reset(); }

private:
    std::optional<Pending> m_pending;
    std::uint32_t m_nextSeq = 1;
    std::array<double, 4> m_sends{-1e9, -1e9, -1e9, -1e9};
    std::size_t m_sendNext = 0;
};

// ---- An NPC's quests (the dialog) ----

struct NpcQuestEntry
{
    const game::QuestDef* quest = nullptr;
    game::QuestStatus status = game::QuestStatus::Hidden;
    bool giver = false;  // this NPC offers it
    bool turnIn = false; // it is handed in to this NPC
};
// What the dialog of NPC `npcId` lists, in this order: ready to hand in here, offered now, in progress
// (given or taken back here), offered from a higher level (greyed). Done, cooldown and hidden ones are
// left out.
[[nodiscard]] std::vector<NpcQuestEntry> npcQuestEntries(const game::QuestCatalog& catalog, std::uint16_t npcId,
                                                         const game::QuestLog& log, const game::QuestUser& user,
                                                         const game::Inventory& inv, const game::ItemCatalog* items,
                                                         std::int64_t now, std::size_t maxActive);
// True when the dialog of `npcId` would list something (a quest giver's click opens it; a merchant then
// shows its quests next to the shop).
[[nodiscard]] bool npcHasQuestBusiness(const game::QuestCatalog& catalog, std::uint16_t npcId, const game::QuestLog& log,
                                       const game::QuestUser& user, const game::Inventory& inv,
                                       const game::ItemCatalog* items, std::int64_t now, std::size_t maxActive);

// ---- Texts (§7.2, §7.3) ----

// "Kül Çömezi 3/8", "Kül Tozu 2/5", "Bekçiyi yen: Kor Bekçisi 0/1", "Avcı Selvi ile konuş".
[[nodiscard]] std::string objectiveText(const game::QuestDef& def, std::size_t index, std::uint16_t progress);
// Every objective line of an active quest with the current progress, plus whether each is complete.
struct ObjectiveLine
{
    std::string text;
    bool done = false;
};
[[nodiscard]] std::vector<ObjectiveLine> objectiveLines(const game::QuestDef& def, const game::QuestRecord& rec,
                                                        const game::Inventory& inv, const game::ItemCatalog* items);
// The reward lines of the dialog: "70 deneyim", "20 Akçe", then the items ("2 × Ham Tav Taşı", the class
// item "Eski Tozluk (sınıfına göre)"); the choice is listed separately (rewardChoices).
struct RewardLine
{
    std::string text;
    std::uint16_t templateId = 0; // an item (its icon and tooltip), else 0
    std::uint16_t count = 0;
};
[[nodiscard]] std::vector<RewardLine> rewardLines(const game::QuestDef& def, std::uint8_t charClass,
                                                  const game::ItemCatalog* items);
[[nodiscard]] std::vector<RewardLine> rewardChoices(const game::QuestDef& def, const game::ItemCatalog* items);
// The status line of a quest in the dialog / log ("Teslim edilebilir", "Devam ediyor", "Seviye 5'te").
[[nodiscard]] std::string questStatusText(const game::QuestDef& def, game::QuestStatus status);
// The line of a refused QUEST_RES (empty for Ok).
[[nodiscard]] std::string questResultText(vo::net::QuestResult r, const game::QuestDef* def,
                                          const game::NpcCatalog* npcs);
// "<NPC adı>"'s name, or "?".
[[nodiscard]] std::string npcName(const game::NpcCatalog* npcs, std::uint16_t npcId);
// The abandon question: "<Görev> bırakılsın mı? İlerlemen silinir."
[[nodiscard]] std::string abandonQuestion(const game::QuestDef& def);
// The hand-in line: "Görev teslim edildi: <Ad> (+<xp> deneyim, +<n> Akçe)".
[[nodiscard]] std::string handInText(const game::QuestDef& def, std::uint32_t xp, std::uint32_t coins);
inline constexpr std::string_view kQuestLogTitle = "Görevler";
inline constexpr std::string_view kQuestLogEmpty = "Etkin görevin yok. Başının üstünde köz taşı olanlarla konuş.";
inline constexpr std::string_view kQuestShopButton = "Alışveriş";
inline constexpr std::string_view kQuestTavLine = "Tav işi Eşik'te yapılır: Demirci Ilgaz'a git.";

// ---- Automation (the quest `scene` commands of WorldScene) ----

enum class QuestVerb : std::uint8_t
{
    Select,        // questselect <quest id>: as a click on the quest in the open dialog
    Accept,        // questaccept: as "Kabul et" (the selected quest)
    HandIn,        // questhandin [choice index]: as "Teslim et"
    Decline,       // questdecline: as "Vazgeç" (the dialog closes)
    AcceptForce,   // questacceptforce <npc name> <quest id>: the request without the dialog (refusal tests)
    HandInForce,   // questhandinforce <npc name> <quest id> [choice]
    Abandon,       // questabandon <quest id>: as the log's "Bırak" (asks first)
    AbandonForce,  // questabandonforce <quest id>: the request without asking
    Log,           // questlog open|close
    LogSelect,     // questlogselect <quest id>
    ExpectState,   // expectquest <id> none|active|ready|done|cooldown [p1 [p2 [p3]]]
    ExpectMarker,  // expectmarker <npc name> none|available|progress|ready
    ExpectTracker, // expecttracker <text>: a tracker line contains <text> ("none": the tracker is empty)
    ExpectMessage, // expectquestmessage <text>: the last quest line contains <text>
    ExpectDialog,  // expectquestdialog <npc name>|none
    ExpectNotice,  // expectquestnotice <text>: a notice shown now contains <text>
    Dump,          // questdump: write the book, the dialog and the markers to the log
};

struct QuestCommand
{
    QuestVerb verb = QuestVerb::Dump;
    std::string text;    // npc name / message / state word
    std::uint32_t a = 0; // quest id
    std::uint32_t b = 0; // choice index
    bool hasB = false;
    std::vector<std::uint16_t> progress; // expectquest counters
};

// nullopt with `error` empty: not a quest verb; nullopt with a message: a malformed quest command.
[[nodiscard]] std::optional<QuestCommand> parseQuestCommand(std::string_view line, std::string& error);

} // namespace vo::client::world
