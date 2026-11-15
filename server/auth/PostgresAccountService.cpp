#include "server/auth/PostgresAccountService.h"

#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Names.h"
#include "shared/net/Crypto.h"

#include <asio/ip/address.hpp>
#include <pqxx/pqxx>

#include <limits>
#include <stdexcept>
#include <utility>

namespace vo::server::auth {

namespace {

using AdminResult = PostgresAccountService::AdminResult;

constexpr const char* kCharacterColumns = "id, name, race, class, face, hair, level, zone_id, x, y, z, yaw";
constexpr const char* kIsoUtc = "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'";

// IDs travel as u32 on the wire and are CHECKed to that range in the schema.
template <class Field>
std::uint32_t toId(const Field& f)
{
    const auto v = f.template as<std::int64_t>();
    if (v < 0 || v > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
        throw std::range_error("id out of u32 range");
    return static_cast<std::uint32_t>(v);
}

// Row with kCharacterColumns starting at `first`.
template <class Row>
CharacterInfo toCharacter(const Row& r, int first = 0)
{
    CharacterInfo c;
    c.id = toId(r[first + 0]);
    c.name = r[first + 1].template as<std::string>();
    c.race = static_cast<std::uint8_t>(r[first + 2].template as<int>());
    c.charClass = static_cast<std::uint8_t>(r[first + 3].template as<int>());
    c.face = static_cast<std::uint8_t>(r[first + 4].template as<int>());
    c.hair = static_cast<std::uint8_t>(r[first + 5].template as<int>());
    c.level = static_cast<std::uint16_t>(r[first + 6].template as<int>());
    c.position.zoneId = static_cast<std::uint16_t>(r[first + 7].template as<int>());
    c.position.x = r[first + 8].template as<float>();
    c.position.y = r[first + 9].template as<float>();
    c.position.z = r[first + 10].template as<float>();
    c.position.yaw = r[first + 11].template as<float>();
    return c;
}

// inet column value: the address if it parses, otherwise NULL.
std::optional<std::string> inetOrNull(const std::string& address)
{
    std::error_code ec;
    const auto parsed = asio::ip::make_address(address, ec);
    if (ec)
        return std::nullopt;
    return parsed.to_string();
}

// The characters_slot_limit trigger raises check_violation with this message.
bool isSlotLimitError(const pqxx::check_violation& e)
{
    return std::string_view(e.what()).find("already has 3 live characters") != std::string_view::npos;
}

struct AccountRow
{
    AccountId id = 0;
    std::string passwordHash;
    bool banned = false;
};

std::optional<AccountRow> selectAccount(pqxx::connection& conn, const std::string& key)
{
    pqxx::read_transaction tx(conn);
    const auto r = tx.exec("SELECT id, password_hash, banned FROM accounts WHERE username_key = $1", pqxx::params{key})
                       .opt_row();
    tx.commit();
    if (!r)
        return std::nullopt;
    return AccountRow{toId((*r)[0]), (*r)[1].as<std::string>(), (*r)[2].as<bool>()};
}

} // namespace

PostgresAccountService::PostgresAccountService(db::ConnectionPool& pool, Options options)
    : m_pool(pool), m_options(options), m_workers(options.workerThreads, "db-accounts")
{
}

PostgresAccountService::~PostgresAccountService()
{
    stop();
}

void PostgresAccountService::stop()
{
    m_workers.stop(); // strands already running finish their queues before the threads exit
}

void PostgresAccountService::submitForAccount(AccountId account, std::function<void()> run,
                                              std::function<void()> fail)
{
    std::deque<StrandItem> failed;
    {
        const std::scoped_lock lock(m_strandMutex);
        auto& queue = m_strands[account];
        const bool idle = queue.empty();
        queue.push_back({std::move(run), std::move(fail)});
        if (!idle)
            return; // the running strand picks it up
        if (m_workers.submit([this, account] { runStrand(account); }))
            return;
        failed.swap(queue); // stopped: nothing will run this strand
        m_strands.erase(account);
    }
    for (auto& item : failed)
        item.fail();
}

void PostgresAccountService::runStrand(AccountId account)
{
    for (;;)
    {
        std::function<void()> run;
        {
            const std::scoped_lock lock(m_strandMutex);
            run = std::move(m_strands.at(account).front().run);
        }
        run();
        {
            const std::scoped_lock lock(m_strandMutex);
            auto it = m_strands.find(account);
            it->second.pop_front();
            if (it->second.empty())
            {
                m_strands.erase(it);
                return;
            }
        }
    }
}

bool PostgresAccountService::runDb(const char* what, const std::function<void(pqxx::connection&)>& job)
{
    std::string error;
    return runDb(what, job, error);
}

bool PostgresAccountService::runDb(const char* what, const std::function<void(pqxx::connection&)>& job,
                                   std::string& error)
{
    if (m_pool.run(job, error))
        return true;
    VO_LOG_ERROR("db: {} failed: {}", what, error);
    return false;
}

// ---- Login ----

void PostgresAccountService::burnDummyVerify(const std::string& password)
{
    std::call_once(m_dummyOnce,
                   [this] { m_dummyHash = net::hashPassword("dummy-password-for-timing").value_or(std::string()); });
    if (!m_dummyHash.empty())
        (void)net::verifyPassword(m_dummyHash, password);
}

LoginOutcome PostgresAccountService::doLogin(const std::string& username, const std::string& password,
                                             const std::string& address)
{
    const LoginOutcome invalid{net::LoginResult::InvalidCredentials, 0};
    const LoginOutcome serverError{net::LoginResult::ServerError, 0};
    if (!game::isValidUsername(username) || !game::isValidPassword(password))
        return invalid;

    const std::string key = game::usernameKey(username);
    std::optional<AccountRow> account;
    if (!runDb("login lookup", [&](pqxx::connection& c) { account = selectAccount(c, key); }))
        return serverError;

    if (!account)
    {
        if (!m_options.autoRegister)
        {
            // Spend the same time as a real verification so unknown users are not distinguishable.
            burnDummyVerify(password);
            return invalid;
        }
        auto hash = net::hashPassword(password); // on this worker, holding no connection
        if (!hash)
            return serverError;
        std::optional<AccountId> created;
        const auto ip = inetOrNull(address);
        const bool ok = runDb("auto-register", [&](pqxx::connection& c) {
            created.reset();
            pqxx::work tx(c);
            const auto r = tx.exec("INSERT INTO accounts (username, username_key, password_hash, last_login_at, "
                                   "last_login_ip) VALUES ($1, $2, $3, now(), $4) "
                                   "ON CONFLICT (username_key) DO NOTHING RETURNING id",
                                   pqxx::params{username, key, *hash, ip})
                               .opt_row();
            tx.commit();
            if (r)
                created = toId((*r)[0]);
        });
        net::secureZero(hash->data(), hash->size());
        if (!ok)
            return serverError;
        if (created)
        {
            VO_LOG_INFO("auth: auto-registered account {} ('{}')", *created, username);
            return {net::LoginResult::Ok, *created};
        }
        // Registered concurrently by another login: verify against that account like a normal login.
        if (!runDb("login lookup", [&](pqxx::connection& c) { account = selectAccount(c, key); }))
            return serverError;
        if (!account)
            return serverError;
    }

    if (!net::verifyPassword(account->passwordHash, password))
        return invalid;
    if (account->banned)
        return {net::LoginResult::Banned, 0};

    const auto ip = inetOrNull(address);
    if (!runDb("last login update", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            tx.exec("UPDATE accounts SET last_login_at = now(), last_login_ip = $2 WHERE id = $1",
                    pqxx::params{static_cast<std::int64_t>(account->id), ip});
            tx.commit();
        }))
    {
        // The credentials were verified; a failed bookkeeping update does not fail the login.
        VO_LOG_WARN("auth: account {} logged in, but last_login_at was not updated", account->id);
    }
    return {net::LoginResult::Ok, account->id};
}

void PostgresAccountService::login(std::string username, std::string password, std::string remoteAddress,
                                   std::function<void(LoginOutcome)> done)
{
    auto job = [this, username = std::move(username), password, address = std::move(remoteAddress), done]() mutable {
        const LoginOutcome outcome = doLogin(username, password, address);
        net::secureZero(password.data(), password.size());
        done(outcome);
    };
    if (!m_workers.submit(std::move(job)))
    {
        net::secureZero(password.data(), password.size());
        done({net::LoginResult::ServerError, 0});
    }
    else
    {
        net::secureZero(password.data(), password.size()); // the job holds its own copy
    }
}

// ---- Characters ----

void PostgresAccountService::listCharacters(AccountId account, std::function<void(CharacterListOutcome)> done)
{
    auto job = [this, account, done] {
        CharacterListOutcome out;
        out.ok = runDb("character list", [&](pqxx::connection& c) {
            out.characters.clear();
            pqxx::read_transaction tx(c);
            for (const auto& r : tx.exec(std::string("SELECT ") + kCharacterColumns +
                                             " FROM characters WHERE account_id = $1 AND deleted_at IS NULL "
                                             "ORDER BY id",
                                         pqxx::params{static_cast<std::int64_t>(account)}))
                out.characters.push_back(toCharacter(r));
            tx.commit();
        });
        if (out.ok && out.characters.size() > net::kMaxCharactersPerAccount)
            out.ok = false;
        if (!out.ok)
            out.characters.clear();
        done(std::move(out));
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

void PostgresAccountService::createCharacter(AccountId account, CreateCharacterRequest request,
                                             std::function<void(CreateCharacterOutcome)> done)
{
    auto job = [this, account, request = std::move(request), done] {
        using R = net::CharCreateResult;
        CreateCharacterOutcome out;
        if (!game::isValidCharacterName(request.name))
            out.result = R::NameInvalid;
        else if (!game::isAllowedRaceClass(request.race, request.charClass))
            out.result = R::InvalidRaceOrClass;
        else if (!game::isValidAppearance(request.face, request.hair))
            out.result = R::InvalidAppearance;
        else
        {
            const std::string key = game::characterNameKey(request.name);
            const bool ok = runDb("character create", [&](pqxx::connection& c) {
                out = {};
                pqxx::work tx(c);
                const auto acc = static_cast<std::int64_t>(account);
                // Locking the account row serialises creates of one account: the count below then
                // sees every committed character, so the slot limit holds under concurrency.
                if (tx.exec("SELECT 1 FROM accounts WHERE id = $1 FOR UPDATE", pqxx::params{acc}).empty())
                {
                    out.result = R::ServerError; // unknown account (as MemoryAccountService)
                    return;
                }
                const auto used = tx.query_value<std::int64_t>(
                    "SELECT count(*) FROM characters WHERE account_id = $1 AND deleted_at IS NULL", pqxx::params{acc});
                if (used >= static_cast<std::int64_t>(net::kMaxCharactersPerAccount))
                {
                    out.result = R::NoFreeSlot;
                    return;
                }
                const auto& spawn = m_options.spawns.forRace(request.race);
                try
                {
                    const auto r =
                        tx.exec(std::string("INSERT INTO characters (account_id, name, name_key, race, class, face, "
                                            "hair, level, zone_id, x, y, z, yaw) VALUES ($1, $2, $3, $4, $5, $6, "
                                            "$7, $8, $9, $10, $11, $12, $13) RETURNING ") +
                                    kCharacterColumns,
                                pqxx::params{acc, request.name, key, static_cast<int>(request.race),
                                             static_cast<int>(request.charClass), static_cast<int>(request.face),
                                             static_cast<int>(request.hair), static_cast<int>(game::kStartLevel),
                                             static_cast<int>(spawn.zoneId), spawn.x, spawn.y, spawn.z, spawn.yaw})
                            .one_row();
                    out.character = toCharacter(r);
                    tx.commit();
                    out.result = R::Ok;
                }
                catch (const pqxx::unique_violation&)
                {
                    out.result = R::NameTaken; // name_key exists (live or soft-deleted character)
                }
                catch (const pqxx::check_violation& e)
                {
                    if (!isSlotLimitError(e))
                        throw;
                    out.result = R::NoFreeSlot; // trigger: only reachable if the rules above diverge
                }
            });
            if (!ok)
                out = {};
        }
        done(std::move(out));
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

void PostgresAccountService::deleteCharacter(AccountId account, CharacterId character,
                                             std::function<void(DeleteCharacterOutcome)> done)
{
    auto job = [this, account, character, done] {
        DeleteCharacterOutcome out{net::CharDeleteResult::ServerError};
        const bool ok = runDb("character delete", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto r = tx.exec("UPDATE characters SET deleted_at = now() "
                                   "WHERE id = $1 AND account_id = $2 AND deleted_at IS NULL",
                                   pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account)});
            tx.commit();
            out.result = r.affected_rows() == 1 ? net::CharDeleteResult::Ok : net::CharDeleteResult::NotFound;
        });
        if (!ok)
            out.result = net::CharDeleteResult::ServerError;
        done(out);
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

void PostgresAccountService::loadCharacterForWorld(AccountId account, CharacterId character,
                                                   std::function<void(LoadCharacterOutcome)> done)
{
    auto job = [this, account, character, done] {
        LoadCharacterOutcome out{net::EnterWorldResult::NotFound, {}};
        const bool ok = runDb("character load", [&](pqxx::connection& c) {
            out = {net::EnterWorldResult::NotFound, {}};
            pqxx::work tx(c);
            const auto r = tx.exec(std::string("UPDATE characters SET last_played_at = now() "
                                               "WHERE id = $1 AND account_id = $2 AND deleted_at IS NULL "
                                               "RETURNING ") +
                                       kCharacterColumns,
                                   pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account)})
                               .opt_row();
            tx.commit();
            if (r)
            {
                out.result = net::EnterWorldResult::Ok;
                out.character = toCharacter(*r);
            }
        });
        if (!ok)
            out = {};
        done(std::move(out));
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

void PostgresAccountService::saveCharacterPosition(AccountId account, CharacterId character,
                                                   CharacterPosition position, std::function<void(bool ok)> done)
{
    auto job = [this, account, character, position, done] {
        bool stored = false;
        const bool ok = runDb("character position save", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto r = tx.exec("UPDATE characters SET zone_id = $3, x = $4, y = $5, z = $6, yaw = $7 "
                                   "WHERE id = $1 AND account_id = $2 AND deleted_at IS NULL",
                                   pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account),
                                                static_cast<int>(position.zoneId), position.x, position.y, position.z,
                                                position.yaw});
            tx.commit();
            stored = r.affected_rows() == 1;
        });
        if (done)
            done(ok && stored);
    };
    submitForAccount(account, std::move(job), [done] {
        if (done)
            done(false);
    });
}

// ---- Synchronous administration ----

AdminResult PostgresAccountService::createAccount(std::string_view username, std::string_view password, AccountId& id,
                                                  std::string& error)
{
    if (!game::isValidUsername(username) || !game::isValidPassword(password))
        return AdminResult::Invalid;
    auto hash = net::hashPassword(password);
    if (!hash)
    {
        error = "password hashing failed (out of memory?)";
        return AdminResult::Error;
    }
    std::optional<AccountId> created;
    const std::string user(username);
    const std::string key = game::usernameKey(username);
    const bool ok = runDb("account create", [&](pqxx::connection& c) {
        created.reset();
        pqxx::work tx(c);
        const auto r = tx.exec("INSERT INTO accounts (username, username_key, password_hash) VALUES ($1, $2, $3) "
                               "ON CONFLICT (username_key) DO NOTHING RETURNING id",
                               pqxx::params{user, key, *hash})
                           .opt_row();
        tx.commit();
        if (r)
            created = toId((*r)[0]);
    }, error);
    net::secureZero(hash->data(), hash->size());
    if (!ok)
        return AdminResult::Error;
    if (!created)
        return AdminResult::Taken;
    id = *created;
    return AdminResult::Ok;
}

std::optional<AccountId> PostgresAccountService::createAccount(std::string_view username, std::string_view password)
{
    AccountId id = 0;
    std::string error;
    if (createAccount(username, password, id, error) != AdminResult::Ok)
        return std::nullopt;
    return id;
}

void PostgresAccountService::setBanned(AccountId account, bool banned)
{
    (void)runDb("ban update", [&](pqxx::connection& c) {
        pqxx::work tx(c);
        tx.exec("UPDATE accounts SET banned = $2, ban_reason = CASE WHEN $2 THEN ban_reason END WHERE id = $1",
                pqxx::params{static_cast<std::int64_t>(account), banned});
        tx.commit();
    });
}

std::size_t PostgresAccountService::accountCount()
{
    Counts c;
    std::string error;
    if (counts(c, error) != AdminResult::Ok)
        return 0;
    return static_cast<std::size_t>(c.accounts);
}

AdminResult PostgresAccountService::setBannedByName(std::string_view username, bool banned, std::string_view reason,
                                                    std::string& error)
{
    if (!game::isValidUsername(username))
        return AdminResult::Invalid;
    std::optional<std::string> why;
    if (banned && !reason.empty())
        why = std::string(reason.substr(0, 200));
    bool found = false;
    const std::string key = game::usernameKey(username);
    if (!runDb("ban update", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto r = tx.exec("UPDATE accounts SET banned = $2, ban_reason = $3 WHERE username_key = $1",
                                   pqxx::params{key, banned, why});
            tx.commit();
            found = r.affected_rows() == 1;
        }, error))
        return AdminResult::Error;
    return found ? AdminResult::Ok : AdminResult::NotFound;
}

AdminResult PostgresAccountService::setPassword(std::string_view username, std::string_view password,
                                                std::string& error)
{
    if (!game::isValidUsername(username) || !game::isValidPassword(password))
        return AdminResult::Invalid;
    auto hash = net::hashPassword(password);
    if (!hash)
    {
        error = "password hashing failed (out of memory?)";
        return AdminResult::Error;
    }
    bool found = false;
    const std::string key = game::usernameKey(username);
    const bool ok = runDb("password update", [&](pqxx::connection& c) {
        pqxx::work tx(c);
        const auto r =
            tx.exec("UPDATE accounts SET password_hash = $2 WHERE username_key = $1", pqxx::params{key, *hash});
        tx.commit();
        found = r.affected_rows() == 1;
    }, error);
    net::secureZero(hash->data(), hash->size());
    if (!ok)
        return AdminResult::Error;
    return found ? AdminResult::Ok : AdminResult::NotFound;
}

AdminResult PostgresAccountService::findAccount(std::string_view username, AccountId& id, std::string& error)
{
    if (!game::isValidUsername(username))
        return AdminResult::Invalid;
    std::optional<AccountRow> row;
    const std::string key = game::usernameKey(username);
    if (!runDb("account lookup", [&](pqxx::connection& c) { row = selectAccount(c, key); }, error))
        return AdminResult::Error;
    if (!row)
        return AdminResult::NotFound;
    id = row->id;
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::counts(Counts& out, std::string& error)
{
    if (!runDb("counts", [&](pqxx::connection& c) {
            pqxx::read_transaction tx(c);
            const auto r = tx.exec("SELECT (SELECT count(*) FROM accounts), "
                                   "(SELECT count(*) FROM accounts WHERE banned), "
                                   "(SELECT count(*) FROM characters WHERE deleted_at IS NULL), "
                                   "(SELECT count(*) FROM characters WHERE deleted_at IS NOT NULL)")
                               .one_row();
            tx.commit();
            out.accounts = r[0].as<std::int64_t>();
            out.bannedAccounts = r[1].as<std::int64_t>();
            out.liveCharacters = r[2].as<std::int64_t>();
            out.deletedCharacters = r[3].as<std::int64_t>();
        }, error))
        return AdminResult::Error;
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::listAllCharacters(std::string_view username, std::vector<AdminCharacter>& out,
                                                      std::string& error)
{
    AccountId account = 0;
    if (const auto r = findAccount(username, account, error); r != AdminResult::Ok)
        return r;
    if (!runDb("character list", [&](pqxx::connection& c) {
            out.clear();
            pqxx::read_transaction tx(c);
            const std::string sql = std::string("SELECT ") + kCharacterColumns + ", deleted_at IS NOT NULL, " +
                                    "to_char(created_at AT TIME ZONE 'UTC', " + kIsoUtc + "), " +
                                    "coalesce(to_char(deleted_at AT TIME ZONE 'UTC', " + kIsoUtc + "), ''), " +
                                    "coalesce(to_char(last_played_at AT TIME ZONE 'UTC', " + kIsoUtc + "), '') " +
                                    "FROM characters WHERE account_id = $1 ORDER BY id";
            for (const auto& r : tx.exec(sql, pqxx::params{static_cast<std::int64_t>(account)}))
            {
                AdminCharacter a;
                a.info = toCharacter(r);
                a.deleted = r[12].as<bool>();
                a.createdAt = r[13].as<std::string>();
                a.deletedAt = r[14].as<std::string>();
                a.lastPlayedAt = r[15].as<std::string>();
                out.push_back(std::move(a));
            }
            tx.commit();
        }, error))
        return AdminResult::Error;
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::restoreCharacter(std::string_view username, CharacterId character,
                                                     std::string& error)
{
    AccountId account = 0;
    if (const auto r = findAccount(username, account, error); r != AdminResult::Ok)
        return r;
    AdminResult result = AdminResult::NotFound;
    if (!runDb("character restore", [&](pqxx::connection& c) {
            result = AdminResult::NotFound;
            pqxx::work tx(c);
            try
            {
                const auto r = tx.exec("UPDATE characters SET deleted_at = NULL "
                                       "WHERE id = $1 AND account_id = $2 AND deleted_at IS NOT NULL",
                                       pqxx::params{static_cast<std::int64_t>(character),
                                                    static_cast<std::int64_t>(account)});
                tx.commit();
                if (r.affected_rows() == 1)
                    result = AdminResult::Ok;
            }
            catch (const pqxx::check_violation& e)
            {
                if (!isSlotLimitError(e))
                    throw;
                result = AdminResult::NoFreeSlot;
            }
        }, error))
        return AdminResult::Error;
    return result;
}

AdminResult PostgresAccountService::renameCharacter(std::string_view username, CharacterId character,
                                                    std::string_view newName, std::string& error)
{
    if (!game::isValidCharacterName(newName))
        return AdminResult::Invalid;
    AccountId account = 0;
    if (const auto r = findAccount(username, account, error); r != AdminResult::Ok)
        return r;
    const std::string name(newName);
    const std::string key = game::characterNameKey(newName);
    AdminResult result = AdminResult::NotFound;
    if (!runDb("character rename", [&](pqxx::connection& c) {
            result = AdminResult::NotFound;
            pqxx::work tx(c);
            try
            {
                const auto r = tx.exec("UPDATE characters SET name = $3, name_key = $4 WHERE id = $1 AND account_id = $2",
                                       pqxx::params{static_cast<std::int64_t>(character),
                                                    static_cast<std::int64_t>(account), name, key});
                tx.commit();
                if (r.affected_rows() == 1)
                    result = AdminResult::Ok;
            }
            catch (const pqxx::unique_violation&)
            {
                result = AdminResult::Taken;
            }
        }, error))
        return AdminResult::Error;
    return result;
}

AdminResult PostgresAccountService::purgeCharacter(std::string_view username, CharacterId character,
                                                   std::string& error)
{
    AccountId account = 0;
    if (const auto r = findAccount(username, account, error); r != AdminResult::Ok)
        return r;
    bool removed = false;
    if (!runDb("character purge", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto r = tx.exec("DELETE FROM characters WHERE id = $1 AND account_id = $2 AND deleted_at IS NOT NULL",
                                   pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account)});
            tx.commit();
            removed = r.affected_rows() == 1;
        }, error))
        return AdminResult::Error;
    return removed ? AdminResult::Ok : AdminResult::NotFound;
}

AdminResult PostgresAccountService::deleteAccount(std::string_view username, std::int64_t& characters,
                                                  std::string& error)
{
    AccountId account = 0;
    if (const auto r = findAccount(username, account, error); r != AdminResult::Ok)
        return r;
    bool removed = false;
    if (!runDb("account delete", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto id = static_cast<std::int64_t>(account);
            characters = static_cast<std::int64_t>(
                tx.exec("DELETE FROM characters WHERE account_id = $1", pqxx::params{id}).affected_rows());
            removed = tx.exec("DELETE FROM accounts WHERE id = $1", pqxx::params{id}).affected_rows() == 1;
            tx.commit();
        }, error))
        return AdminResult::Error;
    return removed ? AdminResult::Ok : AdminResult::NotFound;
}

} // namespace vo::server::auth