// PostgresAccountService: IAccountService backed by PostgreSQL (schema: db/migrations, docs/DATABASE.md).
//
// Same rules and results as MemoryAccountService (the shared suite in
// tests/server/AccountServiceTests.cpp runs against both). Every request runs on the service's own
// WorkerPool - Argon2id hashing included - and uses the ConnectionPool, so nothing blocks the
// network thread; callbacks arrive on a worker thread (see IAccountService.h). All SQL is
// parameterised. DB failures are logged (no secrets) and reported as the ServerError results.
//
// Race-freedom: auto-registration uses INSERT ... ON CONFLICT on the unique username key; character
// creation locks the account row before counting live characters (plus a trigger enforcing the
// same limit for every writer); name uniqueness is the unique index on name_key (all rows,
// soft-deleted included), whose violation maps to NameTaken.
//
// The synchronous helpers below block the calling thread: for vo_admin, tests and startup only.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/db/WorkerPool.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pqxx {
class connection;
}

namespace vo::server::db {
class ConnectionPool;
}

namespace vo::server::auth {

class PostgresAccountService final : public IAccountService
{
public:
    struct Options
    {
        bool autoRegister = false;
        std::size_t workerThreads = 4;
        NewCharacterSpawns spawns{}; // start positions of new characters per race
    };

    // `pool` must outlive the service.
    PostgresAccountService(db::ConnectionPool& pool, Options options);
    ~PostgresAccountService() override;
    PostgresAccountService(const PostgresAccountService&) = delete;
    PostgresAccountService& operator=(const PostgresAccountService&) = delete;

    void stop() override;

    void login(std::string username, std::string password, std::string remoteAddress,
               std::function<void(LoginOutcome)> done) override;
    void listCharacters(AccountId account, std::function<void(CharacterListOutcome)> done) override;
    void createCharacter(AccountId account, CreateCharacterRequest request,
                         std::function<void(CreateCharacterOutcome)> done) override;
    void deleteCharacter(AccountId account, CharacterId character,
                         std::function<void(DeleteCharacterOutcome)> done) override;
    void loadCharacterForWorld(AccountId account, CharacterId character,
                               std::function<void(LoadCharacterOutcome)> done) override;
    void saveCharacterPosition(AccountId account, CharacterId character, CharacterPosition position,
                               std::function<void(bool ok)> done) override;
    void saveCharacterState(AccountId account, CharacterId character, CharacterPosition position,
                            CharacterProgress progress, std::function<void(bool ok)> done) override;
    void loadCharacterItems(AccountId account, CharacterId character, std::optional<game::StarterKit> kit,
                            std::function<void(LoadItemsOutcome)> done) override;
    void applyItemMove(AccountId account, CharacterId character, std::vector<ItemRelocation> relocations,
                       std::function<void(bool ok)> done) override;
    void applyItemWrite(AccountId account, CharacterId character, ItemWrite write,
                        std::function<void(ItemWriteOutcome)> done) override;
    void allocateStats(AccountId account, CharacterId character, StatAllocation allocation,
                       std::function<void(StatAllocationOutcome)> done) override;
    void learnSkill(AccountId account, CharacterId character, SkillLearn learn,
                    std::function<void(SkillLearnOutcome)> done) override;
    void setSkillBar(AccountId account, CharacterId character, SkillBarWrite write,
                     std::function<void(SkillBarOutcome)> done) override;

    // ---- Synchronous administration (blocking; never on the network thread) ----

    enum class AdminResult
    {
        Ok,
        Invalid,  // malformed username/password/name
        NotFound, // no such account/character
        Taken,    // username or character name already used
        NoFreeSlot,
        Error, // database failure (see `error`)
    };

    // Same shape as MemoryAccountService's helpers (used by the shared test suite).
    std::optional<AccountId> createAccount(std::string_view username, std::string_view password);
    void setBanned(AccountId account, bool banned);
    [[nodiscard]] std::size_t accountCount();

    AdminResult createAccount(std::string_view username, std::string_view password, AccountId& id,
                              std::string& error);
    AdminResult setBannedByName(std::string_view username, bool banned, std::string_view reason, std::string& error);
    AdminResult setPassword(std::string_view username, std::string_view password, std::string& error);
    AdminResult findAccount(std::string_view username, AccountId& id, std::string& error);

    struct Counts
    {
        std::int64_t accounts = 0;
        std::int64_t bannedAccounts = 0;
        std::int64_t liveCharacters = 0;
        std::int64_t deletedCharacters = 0;
    };
    AdminResult counts(Counts& out, std::string& error);

    struct AdminCharacter
    {
        CharacterInfo info;
        bool deleted = false;
        std::string createdAt;    // ISO-8601 UTC
        std::string deletedAt;    // empty if live
        std::string lastPlayedAt; // empty if never
    };
    // All characters of an account, soft-deleted ones included, by id.
    AdminResult listAllCharacters(std::string_view username, std::vector<AdminCharacter>& out, std::string& error);
    // Un-deletes a soft-deleted character (fails with NoFreeSlot if the account has 3 live ones).
    AdminResult restoreCharacter(std::string_view username, CharacterId character, std::string& error);
    // Renames a character (live or deleted); the old name is released.
    AdminResult renameCharacter(std::string_view username, CharacterId character, std::string_view newName,
                                std::string& error);
    // Permanently removes a soft-deleted character and releases its name.
    AdminResult purgeCharacter(std::string_view username, CharacterId character, std::string& error);
    // Permanently removes an account and all its characters (names are released). `characters`
    // receives the number of character rows removed.
    AdminResult deleteAccount(std::string_view username, std::int64_t& characters, std::string& error);

    // ---- Items (vo_admin give-item / list-items) ----
    // A live character by name (Turkish-aware, like the uniqueness key).
    AdminResult findCharacterByName(std::string_view name, CharacterInfo& info, std::string& error);
    // A new item (quantity 1) in the lowest free bag slot of a live character: NotFound, Invalid
    // (template 0 or bad name), NoFreeSlot (bag full). The template is not checked here (vo_admin
    // checks it against gamedata/items.toml). An item given to a character that is in the world
    // shows up on its next entry; until then a move onto that bag slot fails with ServerError.
    AdminResult giveItem(std::string_view characterName, game::ItemTemplateId templateId, game::StoredItem& given,
                         std::string& error);
    // `count` units of a template of `catalog` (M10, PROTOCOL_LOOT.md §1.2): first into the bag's
    // stacks of that template below max_stack, then into the lowest free bag slots, all or nothing
    // (NoFreeSlot if they do not all fit; Invalid for an unknown template or count 0). `changed`
    // receives every grown or new row as stored. Same caveat for characters in the world.
    AdminResult giveItems(std::string_view characterName, const game::ItemCatalog& catalog,
                          game::ItemTemplateId templateId, std::uint32_t count, std::vector<game::StoredItem>& changed,
                          std::string& error);
    // vo_admin set-coins: the Akçe balance of a live character (0..kMaxCoins, else Invalid). A
    // character in the world sees it after its next entry; a loot gain before that reports (and
    // keeps) this value plus the gain.
    AdminResult setCoins(std::string_view characterName, std::uint32_t coins, CharacterInfo& info, std::string& error);
    AdminResult listItems(std::string_view characterName, CharacterInfo& info, std::vector<game::StoredItem>& items,
                          std::string& error);

    // ---- Combat (vo_admin set-level) ----
    // Sets a live character's level (clamped to 1..game::kLevelCap), xp 0 and hp/mp to "full on the
    // next entry". `info` receives the character with the new values. A character that is in the
    // world keeps its live level until it leaves (its next save overwrites this).
    // Skill ranks that no longer fit the new level are reset (as resetSkills; `skillsReset` set).
    AdminResult setLevel(std::string_view characterName, std::uint16_t level, CharacterInfo& info, std::string& error,
                         bool* skillsReset = nullptr);

    // ---- Stats (vo_admin reset-stats; PROTOCOL_STATS.md §6.2) ----
    // Sets every stat allocation of a live character to 0 (all points unspent again). `info`
    // receives the character (findCharacterByName also carries the stored allocation, for
    // show-character). A character in the world keeps its live allocation until its next entry or
    // its next allocation.
    AdminResult resetStats(std::string_view characterName, CharacterInfo& info, std::string& error);

    // ---- Skills (vo_admin reset-skills / show-character; PROTOCOL_SKILLS.md §6.2) ----
    // Deletes every skill row of a live character and sets its bar to the default (NULL). A
    // character in the world keeps its live ranks until its next entry or its next learn.
    AdminResult resetSkills(std::string_view characterName, CharacterInfo& info, std::string& error);
    // The stored ranks (raw rows) and bar of a live character, into `info`.
    AdminResult loadSkills(std::string_view characterName, CharacterInfo& info, std::string& error);

private:
    struct StrandItem
    {
        std::function<void()> run;
        std::function<void()> fail; // called instead of run if the service is stopped
    };

    // Requests of one account run one after another in submission order (as with the single
    // worker of MemoryAccountService), so e.g. a fire-and-forget position save on disconnect is
    // never overtaken by the next load of that character. Different accounts run in parallel.
    void submitForAccount(AccountId account, std::function<void()> run, std::function<void()> fail);
    void runStrand(AccountId account);

    // Runs `job` on the pool; logs and returns false on failure.
    bool runDb(const char* what, const std::function<void(pqxx::connection&)>& job);
    bool runDb(const char* what, const std::function<void(pqxx::connection&)>& job, std::string& error);
    LoginOutcome doLogin(const std::string& username, const std::string& password, const std::string& address);
    void burnDummyVerify(const std::string& password);

    db::ConnectionPool& m_pool;
    Options m_options;
    std::once_flag m_dummyOnce;
    std::string m_dummyHash; // verified for unknown users to equalise timing (set once)
    std::mutex m_strandMutex;
    std::unordered_map<AccountId, std::deque<StrandItem>> m_strands; // front = running or next
    db::WorkerPool m_workers; // last: its threads use the members above
};

} // namespace vo::server::auth
