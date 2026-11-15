#include "server/auth/MemoryAccountService.h"

#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Names.h"
#include "shared/net/Crypto.h"

#include <utility>

namespace vo::server::auth {

MemoryAccountService::MemoryAccountService(Options options) : m_options(options)
{
    m_worker = std::thread([this] { workerLoop(); });
}

MemoryAccountService::~MemoryAccountService()
{
    stop();
}

void MemoryAccountService::stop()
{
    {
        const std::scoped_lock lock(m_queueMutex);
        if (m_stopping && !m_worker.joinable())
            return;
        m_stopping = true;
    }
    m_queueCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

bool MemoryAccountService::enqueue(std::function<void()> task)
{
    {
        const std::scoped_lock lock(m_queueMutex);
        if (m_stopping)
            return false;
        m_queue.push_back(std::move(task));
    }
    m_queueCv.notify_one();
    return true;
}

void MemoryAccountService::workerLoop()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty())
                return; // stopping and drained
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

// ---- Accounts ----

std::optional<AccountId> MemoryAccountService::insertAccountLocked(const std::string& key, std::string_view username,
                                                                   std::string hash)
{
    if (m_accountsByKey.contains(key))
        return std::nullopt;
    Account acc;
    acc.id = m_nextAccountId++;
    acc.username = std::string(username);
    acc.passwordHash = std::move(hash);
    const AccountId id = acc.id;
    m_accountKeyById[id] = key;
    m_accountsByKey.emplace(key, std::move(acc));
    return id;
}

std::optional<AccountId> MemoryAccountService::createAccount(std::string_view username, std::string_view password)
{
    if (!game::isValidUsername(username) || !game::isValidPassword(password))
        return std::nullopt;
    auto hash = net::hashPassword(password);
    if (!hash)
        return std::nullopt;
    const std::scoped_lock lock(m_dataMutex);
    return insertAccountLocked(game::usernameKey(username), username, std::move(*hash));
}

void MemoryAccountService::setBanned(AccountId account, bool banned)
{
    const std::scoped_lock lock(m_dataMutex);
    const auto keyIt = m_accountKeyById.find(account);
    if (keyIt == m_accountKeyById.end())
        return;
    m_accountsByKey[keyIt->second].banned = banned;
}

std::size_t MemoryAccountService::accountCount() const
{
    const std::scoped_lock lock(m_dataMutex);
    return m_accountsByKey.size();
}

std::vector<CharacterInfo> MemoryAccountService::charactersOf(std::string_view username) const
{
    const std::scoped_lock lock(m_dataMutex);
    std::vector<CharacterInfo> out;
    const auto it = m_accountsByKey.find(game::usernameKey(username));
    if (it == m_accountsByKey.end())
        return out;
    for (const auto& [id, c] : m_characters)
    {
        if (c.account == it->second.id && !c.deleted)
            out.push_back(c.info);
    }
    return out;
}

LoginOutcome MemoryAccountService::doLogin(const std::string& username, const std::string& password)
{
    const LoginOutcome invalid{net::LoginResult::InvalidCredentials, 0};
    if (!game::isValidUsername(username) || !game::isValidPassword(password))
        return invalid;

    const std::string key = game::usernameKey(username);
    std::optional<Account> account;
    {
        const std::scoped_lock lock(m_dataMutex);
        const auto it = m_accountsByKey.find(key);
        if (it != m_accountsByKey.end())
            account = it->second;
    }

    if (!account)
    {
        if (m_options.autoRegister)
        {
            auto hash = net::hashPassword(password);
            if (!hash)
                return {net::LoginResult::ServerError, 0};
            const std::scoped_lock lock(m_dataMutex);
            if (const auto id = insertAccountLocked(key, username, std::move(*hash)))
            {
                VO_LOG_INFO("auth: auto-registered account {} ('{}')", *id, username);
                return {net::LoginResult::Ok, *id};
            }
            return invalid; // registered concurrently by someone else
        }
        // Spend the same time as a real verification so unknown users are not distinguishable.
        if (m_dummyHash.empty())
            m_dummyHash = net::hashPassword("dummy-password-for-timing").value_or(std::string());
        if (!m_dummyHash.empty())
            (void)net::verifyPassword(m_dummyHash, password);
        return invalid;
    }

    if (!net::verifyPassword(account->passwordHash, password))
        return invalid;
    if (account->banned)
        return {net::LoginResult::Banned, 0};
    return {net::LoginResult::Ok, account->id};
}

void MemoryAccountService::login(std::string username, std::string password, std::string /*remoteAddress*/,
                                 std::function<void(LoginOutcome)> done)
{
    auto task = [this, username = std::move(username), password = std::move(password), done]() mutable {
        const LoginOutcome outcome = doLogin(username, password);
        net::secureZero(password.data(), password.size());
        done(outcome);
    };
    if (!enqueue(std::move(task)))
        done({net::LoginResult::ServerError, 0});
}

// ---- Characters ----

void MemoryAccountService::listCharacters(AccountId account, std::function<void(CharacterListOutcome)> done)
{
    auto task = [this, account, done] {
        CharacterListOutcome out;
        {
            const std::scoped_lock lock(m_dataMutex);
            for (const auto& [id, c] : m_characters)
            {
                if (c.account == account && !c.deleted)
                    out.characters.push_back(c.info);
            }
        }
        out.ok = out.characters.size() <= net::kMaxCharactersPerAccount;
        if (!out.ok)
            out.characters.clear();
        done(std::move(out));
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::createCharacter(AccountId account, CreateCharacterRequest request,
                                           std::function<void(CreateCharacterOutcome)> done)
{
    auto task = [this, account, request = std::move(request), done] {
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
            const std::scoped_lock lock(m_dataMutex);
            std::size_t used = 0;
            for (const auto& [id, c] : m_characters)
            {
                if (c.account == account && !c.deleted)
                    ++used;
            }
            if (!m_accountKeyById.contains(account))
                out.result = R::ServerError;
            else if (used >= net::kMaxCharactersPerAccount)
                out.result = R::NoFreeSlot;
            else if (m_reservedNames.contains(key))
                out.result = R::NameTaken;
            else
            {
                Character c;
                c.account = account;
                c.info.id = m_nextCharacterId++;
                c.info.name = request.name;
                c.info.race = request.race;
                c.info.charClass = request.charClass;
                c.info.face = request.face;
                c.info.hair = request.hair;
                c.info.level = game::kStartLevel;
                c.info.position = m_options.spawns.forRace(request.race);
                m_reservedNames.insert(key);
                out.result = R::Ok;
                out.character = c.info;
                m_characters.emplace(c.info.id, std::move(c));
            }
        }
        done(std::move(out));
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::deleteCharacter(AccountId account, CharacterId character,
                                           std::function<void(DeleteCharacterOutcome)> done)
{
    auto task = [this, account, character, done] {
        DeleteCharacterOutcome out{net::CharDeleteResult::NotFound};
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                it->second.deleted = true; // soft delete: name stays in m_reservedNames
                out.result = net::CharDeleteResult::Ok;
            }
        }
        done(out);
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::loadCharacterForWorld(AccountId account, CharacterId character,
                                                 std::function<void(LoadCharacterOutcome)> done)
{
    auto task = [this, account, character, done] {
        LoadCharacterOutcome out{net::EnterWorldResult::NotFound, {}};
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                out.result = net::EnterWorldResult::Ok;
                out.character = it->second.info;
            }
        }
        done(std::move(out));
    };
    if (!enqueue(std::move(task)))
        done({});
}

void MemoryAccountService::saveCharacterPosition(AccountId account, CharacterId character,
                                                 CharacterPosition position, std::function<void(bool ok)> done)
{
    auto task = [this, account, character, position, done] {
        bool ok = false;
        {
            const std::scoped_lock lock(m_dataMutex);
            const auto it = m_characters.find(character);
            if (it != m_characters.end() && it->second.account == account && !it->second.deleted)
            {
                it->second.info.position = position;
                ok = true;
            }
        }
        if (done)
            done(ok);
    };
    if (!enqueue(std::move(task)) && done)
        done(false);
}

} // namespace vo::server::auth
