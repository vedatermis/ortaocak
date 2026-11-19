// IAccountService: asynchronous account/character persistence used by the session state machine.
//
// Implementations: MemoryAccountService (no database configured; tests) and PostgresAccountService
// (db.connection_string set). Both behave identically: tests/server/AccountServiceTests.cpp runs one
// behavioural suite against both.
//
// CALLBACK CONTRACT (every method):
//   - `done` is invoked exactly once, also when the service is stopping (then with ServerError).
//   - It may be invoked on ANY thread (typically a worker thread), possibly synchronously before
//     the method returns. Callers must marshal back to their own executor (Session does this by
//     posting onto its connection's executor) and must not hold locks the callback needs.
//   - Callbacks must not throw.
// The service enforces the rules of docs/PROTOCOL.md §5.2/§5.3 (name rules, uniqueness, 3 slots,
// soft delete) so every implementation behaves the same; the session only maps results to packets.
#pragma once

#include "shared/game/Character.h"
#include "shared/game/Items.h"
#include "shared/game/Quests.h"
#include "shared/game/Skills.h"
#include "shared/net/Opcodes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vo::server::auth {

using AccountId = std::uint32_t;
using CharacterId = std::uint32_t;

struct CharacterPosition
{
    std::uint16_t zoneId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
};

// Fallback start position, used only when the service is constructed without zone spawn data
// (unit tests). vo_server always passes the start zone's race spawns (NewCharacterSpawns).
inline constexpr CharacterPosition kNewCharacterSpawn{game::kStartZoneId, 256.0f, 0.0f, 256.0f, 0.0f};

// Where new characters of each race start (zone 1, level 1): the start zone's spawn_aydor /
// spawn_kargan points, taken from the terrain by the world (World::newCharacterSpawns()).
struct NewCharacterSpawns
{
    CharacterPosition aydor = kNewCharacterSpawn;
    CharacterPosition kargan = kNewCharacterSpawn;

    [[nodiscard]] const CharacterPosition& forRace(std::uint8_t race) const noexcept
    {
        return race == static_cast<std::uint8_t>(game::Race::Kargan) ? kargan : aydor;
    }
};

struct CharacterInfo
{
    CharacterId id = 0;
    std::string name; // as entered (UTF-8)
    std::uint8_t race = 0;
    std::uint8_t charClass = 0;
    std::uint8_t face = 0;
    std::uint8_t hair = 0;
    std::uint16_t level = 0;
    CharacterPosition position;
    // Items (docs/PROTOCOL_ITEMS.md): whether the starter kit was granted, and the template ids of
    // the items in Equipment/MainHand and OffHand (0 = empty; not checked against the catalog here).
    bool starterKitGranted = false;
    std::uint16_t mainHandTemplate = 0;
    std::uint16_t offHandTemplate = 0;
    // M17: template ids in Equipment/Head .. Feet (slots 2-6), 0 = empty (docs/PROTOCOL_ITEMS.md §2.1).
    std::array<std::uint16_t, 5> armorTemplates{};
    // Combat (docs/PROTOCOL_COMBAT.md §3.5): experience towards the next level and the stored hp/mp.
    // nullopt = full (new characters and characters from before M9). hp 0 = logged out dead.
    std::uint32_t xp = 0;
    std::optional<std::uint32_t> hp;
    std::optional<std::uint32_t> mp;
    // Loot (docs/PROTOCOL_LOOT.md §1.1): the Akçe balance, 0..kMaxCoins.
    std::uint32_t coins = 0;
    // Stats (docs/PROTOCOL_STATS.md §1.2): points spent per attribute (characters.stat_*; all 0 for
    // characters from before M11, which therefore have every point of their level unspent).
    game::Attributes statPoints{};
    // Skills (docs/PROTOCOL_SKILLS.md §6): the stored ranks (character_skills; 0 = no row, the free
    // skill's rank 1 is implicit) and the bar (characters.skill_bar; nullopt = the default bar).
    game::SkillRanks skillRanks{};
    std::optional<game::SkillBar> skillBar;
};

// What the world persists besides the position (level, experience, hp, mp).
struct CharacterProgress
{
    std::uint16_t level = 1;
    std::uint32_t xp = 0;
    std::uint32_t hp = 0;
    std::uint32_t mp = 0;
    bool operator==(const CharacterProgress&) const = default;
};

struct LoginOutcome
{
    net::LoginResult result = net::LoginResult::ServerError;
    AccountId accountId = 0; // valid only if result == Ok
};

struct CharacterListOutcome
{
    bool ok = false;                      // false: storage error
    std::vector<CharacterInfo> characters; // not deleted, at most kMaxCharactersPerAccount
};

struct CreateCharacterRequest
{
    std::string name;
    std::uint8_t race = 0;
    std::uint8_t charClass = 0;
    std::uint8_t face = 0;
    std::uint8_t hair = 0;
};

struct CreateCharacterOutcome
{
    net::CharCreateResult result = net::CharCreateResult::ServerError;
    CharacterInfo character; // valid only if result == Ok
};

struct DeleteCharacterOutcome
{
    net::CharDeleteResult result = net::CharDeleteResult::ServerError;
};

struct LoadCharacterOutcome
{
    net::EnterWorldResult result = net::EnterWorldResult::ServerError;
    CharacterInfo character; // valid only if result == Ok
};

struct LoadItemsOutcome
{
    bool ok = false;          // false: storage error (or the character is not this account's)
    bool kitGranted = false;  // the starter kit was granted by this call
    std::vector<game::StoredItem> items; // every row of the character, unknown templates included
};

// One item of an atomic move: it must be at `from` (or already at `to`) and ends at `to`.
using ItemRelocation = game::ItemMovePlan::Relocation;

// An atomic change of item quantities, new stacks and the coin balance (docs/PROTOCOL_LOOT.md §6):
// a loot take, a merge move, a purchase, a sale or a consumed potion (docs/PROTOCOL_SHOP.md §7), and
// since v10 an upgrade attempt (the stone, the fee and the item's level / destruction together).
// Every quantity change requires the stack to hold exactly `before` units (after 0 deletes it);
// inserts must land on free places. The balance changes by coinsAdded - coinsRemoved and must stay
// within 0..kMaxCoins.
// v10 (docs/PROTOCOL_UPGRADE.md §6): the upgrade level of one item changes, or the item is destroyed.
// The item must hold exactly `before` (and be a single unit).
struct ItemUpgradeChange
{
    game::ItemInstanceId id = 0;
    std::uint8_t before = 0;
    std::uint8_t after = 0; // ignored when destroyed
    bool destroyed = false; // the row is deleted
    bool operator==(const ItemUpgradeChange&) const = default;
};

struct ItemWrite
{
    std::vector<game::ItemQuantityChange> quantities;
    std::vector<game::NewItem> inserts;
    std::uint32_t coinsAdded = 0;
    std::uint32_t coinsRemoved = 0; // v8: a purchase; v10: an upgrade fee
    std::vector<ItemUpgradeChange> upgrades; // v10: an upgrade attempt's item (success / destroyed)
};

struct ItemWriteOutcome
{
    bool ok = false;                          // false: nothing changed
    std::vector<game::ItemInstanceId> newIds; // ids of `inserts`, same order (only if ok)
    std::uint32_t coins = 0;                  // the stored balance after the write (only if ok)
};

// One STAT_ALLOCATE_REQ write (docs/PROTOCOL_STATS.md §6.1).
struct StatAllocation
{
    game::Attributes points{};     // added per attribute
    std::uint32_t budget = 0;      // the sum after must not exceed this (3 x (level - 1) of the live level)
    game::Attributes classStart{}; // class start + allocated after must not exceed kAttributeCap
};

struct StatAllocationOutcome
{
    bool ok = false;               // false: nothing changed
    game::Attributes allocated{};  // the stored allocation after the write (only if ok)
};

// One SKILL_LEARN_REQ write (docs/PROTOCOL_SKILLS.md §6.1): raise skillId by one rank if the shared
// rules (game::checkSkillLearn with the class and the live level) allow it on the stored ranks.
struct SkillLearn
{
    std::uint16_t skillId = 0;
    std::uint8_t charClass = 0;
    std::uint16_t level = 1;
};

struct SkillLearnOutcome
{
    net::SkillLearnResult result = net::SkillLearnResult::ServerError; // ServerError: nothing known
    game::SkillRanks ranks{}; // the stored ranks after the request, normalised (unless ServerError)
};

// One SKILL_BAR_SET_REQ write: the whole bar, checked against the stored ranks.
struct SkillBarWrite
{
    game::SkillBar bar{};
    std::uint8_t charClass = 0;
};

struct SkillBarOutcome
{
    net::SkillBarResult result = net::SkillBarResult::ServerError;
    game::SkillBar bar{};     // the stored bar after the request (unless ServerError)
    game::SkillRanks ranks{}; // the stored ranks, normalised (unless ServerError)
};

// ---- Quests (docs/PROTOCOL_QUESTS.md §6, v11) ----

struct LoadQuestsOutcome
{
    bool ok = false;                       // false: storage error (or the character is not this account's)
    std::vector<game::QuestRecord> quests; // every row of the character (ascending id); completedAt = Unix seconds
};

// One quest row of an atomic quest write. The row must be in `before` (None = no row yet); it ends in
// `after` (None = deleted) with `progress`. `completed`: a hand-in (completions + 1, completed_at = now).
// A Done row that is not `completed` keeps its completions and completed_at (an abandoned repeat).
// `strict` false (a progress save): a row that is not Active is left alone instead of failing the write.
struct QuestChange
{
    game::QuestId quest = 0;
    ::vo::net::QuestState before = ::vo::net::QuestState::None;
    ::vo::net::QuestState after = ::vo::net::QuestState::Active;
    std::array<std::uint16_t, ::vo::net::kQuestObjectiveSlots> progress{};
    bool completed = false;
    bool strict = true;
    bool operator==(const QuestChange&) const = default;
};

// Accept / abandon / hand-in / progress save in ONE transaction: the quest rows, the item write of a
// hand-in (collected units removed, rewards inserted, the Akçe reward) and its experience (`xp` added to
// the stored level / xp with game::addExperience, as the world adds it after the commit), or nothing.
struct QuestWrite
{
    std::vector<QuestChange> changes;
    ItemWrite items;
    std::uint32_t xp = 0;
};

struct QuestWriteOutcome
{
    bool ok = false;                          // false: nothing changed
    std::vector<game::ItemInstanceId> newIds; // ids of items.inserts (only if ok)
    std::uint32_t coins = 0;                  // the stored balance after the write (only if ok)
    std::int64_t now = 0;                     // the storage's clock (Unix seconds): completed_at of a hand-in
};

class IAccountService
{
public:
    virtual ~IAccountService() = default;

    // Unknown user and wrong password both yield InvalidCredentials (also for malformed username
    // or password). With auto-registration enabled, an unknown valid username with a valid
    // password creates the account. Rate limiting and duplicate-login handling are the caller's job.
    // `remoteAddress` (IP text; may be empty or "?") is stored as the last login address.
    virtual void login(std::string username, std::string password, std::string remoteAddress,
                       std::function<void(LoginOutcome)> done) = 0;

    virtual void listCharacters(AccountId account, std::function<void(CharacterListOutcome)> done) = 0;

    // Check order: NameInvalid, InvalidRaceOrClass, InvalidAppearance, NoFreeSlot, NameTaken.
    virtual void createCharacter(AccountId account, CreateCharacterRequest request,
                                 std::function<void(CreateCharacterOutcome)> done) = 0;

    // Soft delete: the character disappears from the list but its name stays reserved.
    virtual void deleteCharacter(AccountId account, CharacterId character,
                                 std::function<void(DeleteCharacterOutcome)> done) = 0;

    // NotFound unless the character exists, belongs to `account` and is not deleted.
    virtual void loadCharacterForWorld(AccountId account, CharacterId character,
                                       std::function<void(LoadCharacterOutcome)> done) = 0;

    // `done(true)` if stored. `done` may be empty (fire and forget).
    virtual void saveCharacterPosition(AccountId account, CharacterId character, CharacterPosition position,
                                       std::function<void(bool ok)> done) = 0;
    // Position plus level, experience, hp and mp in one write (the world's saves since M9). Same
    // contract as saveCharacterPosition. hp 0 means the character left the world dead.
    virtual void saveCharacterState(AccountId account, CharacterId character, CharacterPosition position,
                                    CharacterProgress progress, std::function<void(bool ok)> done) = 0;

    // ---- Items (docs/PROTOCOL_ITEMS.md §1.3, §5) ----

    // Loads the items of a live character of `account`. If its starter kit has not been granted
    // yet and `kit` is set, grants it first: the rows (game::starterKitItems) and the flag are
    // written in ONE transaction, so the kit is granted exactly once even under concurrency.
    virtual void loadCharacterItems(AccountId account, CharacterId character, std::optional<game::StarterKit> kit,
                                    std::function<void(LoadItemsOutcome)> done) = 0;

    // Applies one move atomically: every relocation or none (one transaction; the place
    // uniqueness is checked at commit, so swaps work). Fails (false, nothing changed) if the
    // character is not a live character of `account`, or an item is neither at its `from` nor
    // already at its `to` place, or a place would hold two items. `done(true)` only after commit.
    virtual void applyItemMove(AccountId account, CharacterId character, std::vector<ItemRelocation> relocations,
                               std::function<void(bool ok)> done) = 0;

    // ---- Loot (docs/PROTOCOL_LOOT.md §6) ----

    // Applies `write` in ONE transaction: the quantity changes, the inserts and the coin change, or
    // nothing. Fails (ok false) if the character is not a live character of `account`, a stack does
    // not hold its `before` quantity, a place would hold two items, or the balance would pass
    // kMaxCoins or go below 0. `done` only after commit; on success it carries the new item ids and the stored
    // balance (which includes changes made by vo_admin set-coins in the meantime).
    virtual void applyItemWrite(AccountId account, CharacterId character, ItemWrite write,
                                std::function<void(ItemWriteOutcome)> done) = 0;

    // ---- Stats (docs/PROTOCOL_STATS.md §6) ----

    // Adds `allocation.points` to the stored allocation of a live character of `account` in one
    // statement, if the sum after is at most `budget` and every class start + allocated after is at
    // most kAttributeCap; otherwise (or on a storage error) nothing changes and ok is false. `done`
    // only after commit; on success it carries the stored allocation (which includes a vo_admin
    // reset-stats made in the meantime).
    virtual void allocateStats(AccountId account, CharacterId character, StatAllocation allocation,
                               std::function<void(StatAllocationOutcome)> done) = 0;

    // ---- Skills (docs/PROTOCOL_SKILLS.md §6) ----

    // Raises one skill of a live character of ccount by one rank in one transaction if
    // game::checkSkillLearn allows it on the STORED ranks; otherwise nothing changes and the result
    // says why (ServerError on a storage error). done only after commit; it carries the stored
    // ranks (which include a vo_admin reset-skills made in the meantime).
    virtual void learnSkill(AccountId account, CharacterId character, SkillLearn learn,
                            std::function<void(SkillLearnOutcome)> done) = 0;
    // Stores the whole bar if every slot is empty or a skill of the class learned in the STORED
    // ranks (else NotLearned, nothing changed). done only after commit.
    virtual void setSkillBar(AccountId account, CharacterId character, SkillBarWrite write,
                             std::function<void(SkillBarOutcome)> done) = 0;

    // ---- Quests (docs/PROTOCOL_QUESTS.md §6) ----

    // The quest rows of a live character of `account` (character_quests, 0010_quests).
    virtual void loadQuests(AccountId account, CharacterId character, std::function<void(LoadQuestsOutcome)> done) = 0;
    // Applies `write` in ONE transaction (see QuestWrite): fails (ok false, nothing changed) if the character
    // is not a live character of `account`, a strict row is not in its `before` state, or the item write
    // fails (as applyItemWrite). `done` only after commit.
    virtual void applyQuestWrite(AccountId account, CharacterId character, QuestWrite write,
                                 std::function<void(QuestWriteOutcome)> done) = 0;

    // Completes every queued request, then stops; later requests complete at once with ServerError.
    // Idempotent. Call before tearing down whatever the callbacks post into.
    virtual void stop() = 0;
};

} // namespace vo::server::auth
