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
#include "shared/net/Opcodes.h"

#include <cstdint>
#include <functional>
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

// Where new characters start (zone 1, level 1). TODO(M6): per-race spawn points from zone data.
inline constexpr CharacterPosition kNewCharacterSpawn{game::kStartZoneId, 256.0f, 0.0f, 256.0f, 0.0f};

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

    // Completes every queued request, then stops; later requests complete at once with ServerError.
    // Idempotent. Call before tearing down whatever the callbacks post into.
    virtual void stop() = 0;
};

} // namespace vo::server::auth
