// Quests shared by client, server and tools (M17, docs/PROTOCOL_QUESTS.md): the quest catalog of
// gamedata/quests.toml (strictly validated like the other data files), one character's quest log and
// the pure rules of §2 (who may accept, what a kill counts for, when a quest is ready, what a hand-in
// removes and grants). The server decides with exactly this code; the client uses it for the markers,
// the dialog and the tracker; the bots for their checks.
//
//   game::QuestCatalog catalog;
//   if (!game::loadQuestCatalog(path, &items, &npcs, &monsters, catalog, error)) -> refuse to start
//   const net::QuestResult r = game::checkQuestAccept(catalog, *def, log, user, now);
//
// Objectives: kill N of some monster templates, kill a boss (an elite template), collect N of an item
// (counted in the bag; handed in and removed on completion; they come from normal loot), talk to
// another NPC (the quest is handed in to that NPC). Kill credit: the player who gets the loot (the
// experience winner, else the killing blow).
//
// Time: records keep `completedAt` in whole seconds of some clock (the server: Unix time; the client:
// its own clock, from QuestEntry.cooldownS) and every rule takes `now` in the same clock.
#pragma once

#include "shared/game/Items.h"
#include "shared/net/Opcodes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::game {

class MonsterData;
class NpcCatalog;

using QuestId = std::uint16_t; // 1..999 (QuestEntry.quest_id)
using QuestResult = net::QuestResult;
using QuestState = net::QuestState;

inline constexpr std::string_view kQuestsFile = "gamedata/quests.toml";
inline constexpr QuestId kMaxQuestId = 999;
inline constexpr std::size_t kMaxQuests = 200;
inline constexpr std::size_t kMaxQuestObjectives = net::kQuestObjectiveSlots; // 3
inline constexpr std::uint16_t kMaxObjectiveCount = 50;
inline constexpr std::size_t kDefaultMaxActiveQuests = 10; // [rules] max_active (1..25)
inline constexpr std::uint32_t kMinQuestCooldownS = 60;
inline constexpr std::uint32_t kMaxQuestCooldownS = 7 * 24 * 3600;
inline constexpr std::uint32_t kMaxQuestXp = 100'000;
inline constexpr std::uint32_t kMaxQuestCoins = 100'000;

enum class ObjectiveKind : std::uint8_t
{
    Kill,    // "kill": kill `count` of `monsters`
    Boss,    // "boss": kill `count` of `monsters` (elite templates)
    Collect, // "collect": hold `count` of `item` in the bag at the hand-in (they are taken)
    Talk,    // "talk": hand the quest in to `npc` (the quest's turn-in NPC)
};

struct QuestObjective
{
    ObjectiveKind kind = ObjectiveKind::Kill;
    std::vector<std::uint16_t> monsters; // kill / boss: monster template ids
    ItemTemplateId item = 0;             // collect
    std::uint16_t npc = 0;               // talk: NPC id of npcs.toml
    std::uint16_t count = 1;             // kill / boss / collect: 1..50; talk: 1
    std::string label;                   // shown name ("Kül Çömezi"); the monster's / item's / NPC's by default
};

struct QuestRewardItem
{
    ItemTemplateId templateId = 0;
    std::uint16_t count = 1;
    bool operator==(const QuestRewardItem&) const = default;
};

struct QuestReward
{
    std::uint32_t xp = 0;
    std::uint32_t coins = 0;
    std::vector<QuestRewardItem> items; // everyone gets these
    // "the piece that fits my class": by class value 1..4 (0 = nothing for that class); all or none set.
    std::array<ItemTemplateId, 5> classItems{};
    std::vector<ItemTemplateId> choice; // the player picks one of these (2..4) at the hand-in
    [[nodiscard]] bool hasClassItems() const noexcept;
};

struct QuestDef
{
    QuestId id = 0;
    std::string name;
    std::uint16_t giver = 0;  // NPC id (npcs.toml) that offers it
    std::uint16_t turnIn = 0; // NPC id it is handed in to (default: the giver)
    std::uint16_t level = 1;  // lowest character level that may accept it
    std::uint8_t race = 0;    // 0 = both, else game::Race
    QuestId after = 0;        // "after": a quest that must be done (handed in) first; 0 = none
    bool repeatable = false;
    std::uint32_t cooldownS = 0; // repeatable: from the hand-in until it can be taken again
    std::string story;        // the giver's text when it offers the quest
    std::string progressText; // the turn-in NPC's text while it is not ready (may be empty)
    std::string doneText;     // the turn-in NPC's text at the hand-in
    std::vector<QuestObjective> objectives; // 1..3
    QuestReward reward;
};

class QuestCatalog
{
public:
    [[nodiscard]] const QuestDef* find(QuestId id) const noexcept;
    [[nodiscard]] const std::vector<QuestDef>& quests() const noexcept { return m_quests; } // file order
    [[nodiscard]] std::size_t maxActive() const noexcept { return m_maxActive; }
    [[nodiscard]] bool empty() const noexcept { return m_quests.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_quests.size(); }
    // Quests offered by / handed in to an NPC (file order).
    [[nodiscard]] std::vector<const QuestDef*> givenBy(std::uint16_t npcId) const;
    [[nodiscard]] std::vector<const QuestDef*> handedInTo(std::uint16_t npcId) const;

private:
    friend bool parseQuestCatalog(std::string_view, std::string_view, const ItemCatalog*, const NpcCatalog*,
                                  const MonsterData*, QuestCatalog&, std::string&);
    std::vector<QuestDef> m_quests;
    std::size_t m_maxActive = kDefaultMaxActiveQuests;
};

// Parses and validates quests.toml text. Strict: unknown keys, ids outside 1..999 or twice, empty or
// overlong texts, an unknown race, `after` naming itself or a quest that is not earlier in the file,
// a repeatable quest without a cooldown (60 s .. 7 days) or a one-time one with one, 0 or more than 3
// objectives, counts outside 1..50, a talk objective whose NPC is not the turn-in NPC (or the giver), a
// collect objective of an item that does not stack, a boss objective of a template that is not elite,
// rewards above the caps, class items not for their class, a choice of fewer than 2 or more than 4.
// With the catalogs: every NPC, monster and item must exist. False + "<source>:<line>: <problem>".
[[nodiscard]] bool parseQuestCatalog(std::string_view tomlText, std::string_view sourceName, const ItemCatalog* items,
                                     const NpcCatalog* npcs, const MonsterData* monsters, QuestCatalog& out,
                                     std::string& error);
[[nodiscard]] bool loadQuestCatalog(const std::filesystem::path& file, const ItemCatalog* items, const NpcCatalog* npcs,
                                    const MonsterData* monsters, QuestCatalog& out, std::string& error);
[[nodiscard]] std::optional<std::filesystem::path> locateQuestsFile();

// The installed catalog, else gamedata/quests.toml loaded on first use (checked against the other data
// files when they are found; an empty catalog if that fails). Thread-safe to read; install first.
[[nodiscard]] const QuestCatalog& quests() noexcept;
void installQuestCatalog(std::shared_ptr<const QuestCatalog> catalog);

[[nodiscard]] std::string_view objectiveKindName(ObjectiveKind k) noexcept; // "kill", "boss", ...

// ---- One character's quests ----

struct QuestRecord
{
    QuestId quest = 0;
    QuestState state = QuestState::Active; // Active or Done
    std::array<std::uint16_t, kMaxQuestObjectives> progress{}; // kill / boss counters
    std::uint32_t completions = 0; // hand-ins so far
    std::int64_t completedAt = 0;  // the last hand-in (seconds, see above); 0 = never
    bool operator==(const QuestRecord&) const = default;
};

class QuestLog
{
public:
    [[nodiscard]] const QuestRecord* find(QuestId id) const noexcept;
    [[nodiscard]] QuestRecord* find(QuestId id) noexcept;
    [[nodiscard]] const std::vector<QuestRecord>& records() const noexcept { return m_records; } // ascending id
    [[nodiscard]] std::size_t activeCount() const noexcept;
    [[nodiscard]] bool isDone(QuestId id) const noexcept; // handed in at least once
    // Inserts or replaces the record of `r.quest`.
    void put(const QuestRecord& r);
    void erase(QuestId id);
    void clear() noexcept { m_records.clear(); }
    bool operator==(const QuestLog&) const = default;

private:
    std::vector<QuestRecord> m_records;
};

struct QuestUser
{
    std::uint8_t race = 0;
    std::uint8_t charClass = 0;
    std::uint16_t level = 1;
};

// Seconds until a Done repeatable quest may be taken again (0 = now, or not on cooldown).
[[nodiscard]] std::uint32_t questCooldownLeft(const QuestDef& def, const QuestRecord* rec, std::int64_t now) noexcept;

// Whether `user` may accept `def` now (§2.1, in this order): AlreadyActive, AlreadyDone (one-time),
// OnCooldown (repeatable), WrongRace, LevelTooLow, NeedsQuest, TooManyActive (`maxActive`), else Ok.
// Who offers it (WrongNpc), range and death are the caller's checks.
[[nodiscard]] QuestResult checkQuestAccept(const QuestCatalog& catalog, const QuestDef& def, const QuestLog& log,
                                           const QuestUser& user, std::int64_t now, std::size_t maxActive);

// The progress of objective `index` (0..count): kill / boss the record's counter, collect the units in
// the bag (capped at count), talk 1 while the quest is active.
[[nodiscard]] std::uint16_t objectiveProgress(const QuestDef& def, const QuestRecord& rec, const Inventory& inv,
                                              const ItemCatalog* items, std::size_t index) noexcept;
// Every objective met (an Active record only).
[[nodiscard]] bool questReady(const QuestDef& def, const QuestRecord& rec, const Inventory& inv,
                              const ItemCatalog* items) noexcept;
// Units of `templateId` in the bag (hidden items not counted).
[[nodiscard]] std::uint32_t bagUnits(const Inventory& inv, ItemTemplateId templateId, const ItemCatalog* items) noexcept;

// A kill of monster `templateId` by the owner of `log`: every active quest with an unfinished kill / boss
// objective for it counts one. Returns the quests whose counters changed (ascending id).
std::vector<QuestId> noteQuestKill(const QuestCatalog& catalog, QuestLog& log, std::uint16_t templateId);

// The reward items of a hand-in for `charClass` and `choice` (fixed items, the class item, the chosen
// one), in that order. nullopt = BadChoice (a choice is offered and `choice` is not one of them, or a
// choice was named for a quest without one).
[[nodiscard]] std::optional<std::vector<QuestRewardItem>> questRewardItems(const QuestDef& def, std::uint8_t charClass,
                                                                           std::uint8_t choice);

struct QuestHandInPlan
{
    QuestResult result = QuestResult::ServerError;
    std::vector<ItemQuantityChange> removed; // collect objectives: the units taken (lowest bag slots first)
    ItemAddPlan add;                         // the reward items, placed after the removal
    std::vector<QuestRewardItem> rewards;    // what `add` places
    std::uint32_t xp = 0;
    std::uint32_t coins = 0;
    // The bag slot changes (removal and rewards) once the inserts got their ids.
    [[nodiscard]] std::vector<ItemSlotChange> changes(const Inventory& before,
                                                      const std::vector<ItemInstanceId>& createdIds) const;
    // Applies the plan to `inv` (removal, then rewards).
    void apply(Inventory& inv, const std::vector<ItemInstanceId>& createdIds) const;
};

// Decides QUEST_HANDIN_REQ against the committed state (§2.3, in this order): NotActive, NotReady,
// BadChoice, BagFull (a reward unit does not fit after the removal), PurseFull, else Ok with the plan.
// The turn-in NPC, range and death are the caller's checks.
[[nodiscard]] QuestHandInPlan planQuestHandIn(const ItemCatalog& items, const Inventory& inv, std::uint32_t coins,
                                              const QuestDef& def, const QuestLog& log, const QuestUser& user,
                                              std::uint8_t choice);

// What a character sees at an NPC / on the map for one quest (the client's markers and dialog).
enum class QuestStatus : std::uint8_t
{
    Hidden,    // not for this character (race), or its prerequisite is not done
    Later,     // offered from a higher level (shown greyed: "Seviye 5'te")
    Available, // may be accepted now
    Active,    // accepted, an objective is not met
    Ready,     // accepted, every objective met
    Cooldown,  // a repeatable one, handed in, waiting
    Done,      // a one-time one, handed in
};
[[nodiscard]] QuestStatus questStatus(const QuestCatalog& catalog, const QuestDef& def, const QuestLog& log,
                                      const QuestUser& user, const Inventory& inv, const ItemCatalog* items,
                                      std::int64_t now, std::size_t maxActive);

// The marker over an NPC, strongest first: a quest ready to hand in to it, one it offers, one in
// progress that it gives or takes back; None if nothing.
enum class QuestMarker : std::uint8_t
{
    None,
    InProgress,
    Available,
    Ready,
};
[[nodiscard]] QuestMarker questMarkerFor(const QuestCatalog& catalog, std::uint16_t npcId, const QuestLog& log,
                                         const QuestUser& user, const Inventory& inv, const ItemCatalog* items,
                                         std::int64_t now, std::size_t maxActive);
[[nodiscard]] std::string_view questMarkerName(QuestMarker m) noexcept; // "none", "progress", "available", "ready"

} // namespace vo::game
