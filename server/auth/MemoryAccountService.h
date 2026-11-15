// MemoryAccountService: in-memory IAccountService used when no database is configured (M1).
//
// Uses the real rules: Argon2id password hashes, shared/game name validation and Turkish-aware
// uniqueness keys, 3 character slots, soft delete that keeps the name reserved, optional
// auto-registration (auth.dev_auto_register). Requests run in FIFO order on one internal worker
// thread (like a DB worker), so callbacks arrive on that thread - see IAccountService.h.
// Data is lost when the process exits.
#pragma once

#include "server/auth/IAccountService.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace vo::server::auth {

class MemoryAccountService final : public IAccountService
{
public:
    struct Options
    {
        bool autoRegister = false;
    };

    explicit MemoryAccountService(Options options);
    ~MemoryAccountService() override;
    MemoryAccountService(const MemoryAccountService&) = delete;
    MemoryAccountService& operator=(const MemoryAccountService&) = delete;

    // Runs the queued requests, then joins the worker. Later requests complete immediately with
    // ServerError. Idempotent; called by the destructor.
    void stop();

    void login(std::string username, std::string password, std::function<void(LoginOutcome)> done) override;
    void listCharacters(AccountId account, std::function<void(CharacterListOutcome)> done) override;
    void createCharacter(AccountId account, CreateCharacterRequest request,
                         std::function<void(CreateCharacterOutcome)> done) override;
    void deleteCharacter(AccountId account, CharacterId character,
                         std::function<void(DeleteCharacterOutcome)> done) override;
    void loadCharacterForWorld(AccountId account, CharacterId character,
                               std::function<void(LoadCharacterOutcome)> done) override;
    void saveCharacterPosition(AccountId account, CharacterId character, CharacterPosition position,
                               std::function<void(bool ok)> done) override;

    // Synchronous helpers for setup/tests (thread-safe). createAccount returns nullopt if the
    // username/password is invalid or the username is taken.
    std::optional<AccountId> createAccount(std::string_view username, std::string_view password);
    void setBanned(AccountId account, bool banned);
    [[nodiscard]] std::size_t accountCount() const;

private:
    struct Account
    {
        AccountId id = 0;
        std::string username;
        std::string passwordHash;
        bool banned = false;
    };
    struct Character
    {
        CharacterInfo info;
        AccountId account = 0;
        bool deleted = false;
    };

    // Returns false if stopped (the task is not run).
    bool enqueue(std::function<void()> task);
    void workerLoop();

    LoginOutcome doLogin(const std::string& username, const std::string& password);
    std::optional<AccountId> insertAccountLocked(const std::string& key, std::string_view username,
                                                 std::string hash);

    Options m_options;

    // Data (guarded by m_dataMutex).
    mutable std::mutex m_dataMutex;
    std::unordered_map<std::string, Account> m_accountsByKey; // key: usernameKey()
    std::unordered_map<AccountId, std::string> m_accountKeyById;
    std::map<CharacterId, Character> m_characters;
    std::set<std::string> m_reservedNames; // characterNameKey(), including deleted characters
    AccountId m_nextAccountId = 1;
    CharacterId m_nextCharacterId = 1;
    std::string m_dummyHash; // verified for unknown users to equalise timing

    // Worker (guarded by m_queueMutex).
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<std::function<void()>> m_queue;
    bool m_stopping = false;
    std::thread m_worker;
};

} // namespace vo::server::auth
