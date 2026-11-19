#include "server/auth/PostgresAccountService.h"

#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Names.h"
#include "shared/net/Crypto.h"

#include <asio/ip/address.hpp>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vo::server::auth {

namespace {

using AdminResult = PostgresAccountService::AdminResult;

// After the position: the kit flag and the template ids in Equipment/MainHand (0, 0) and OffHand
// (0, 1) (sub-selects also work in RETURNING), then xp, hp, mp (0003_combat; NULL hp/mp = full).
constexpr const char* kCharacterColumns =
    "id, name, race, class, face, hair, level, zone_id, x, y, z, yaw, starter_kit_granted, "
    "(SELECT template_id FROM character_items i WHERE i.char_id = characters.id AND i.container = 0 AND i.slot = 0), "
    "(SELECT template_id FROM character_items i WHERE i.char_id = characters.id AND i.container = 0 AND i.slot = 1), "
    "xp, hp, mp, coins, stat_strength, stat_dexterity, stat_vitality, stat_intelligence, stat_spirit, "
    "array_to_string(skill_bar, ',')";
// 0004_loot added coins, 0005_stats the five stat_* columns, 0006_skills the bar (as text, NULL = default).
constexpr int kCharacterColumnCount = 25;
constexpr const char* kStatColumns = "stat_strength, stat_dexterity, stat_vitality, stat_intelligence, stat_spirit";
constexpr const char* kItemColumns = "item_id, container, slot, template_id, quantity, upgrade"; // upgrade: 0009 (v10)
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

// "1,0,0,0,0,0,0,0" (array_to_string of characters.skill_bar; CHECKed to 8 values 0..63).
// Stored bar elements (docs/PROTOCOL_SHOP.md §7): a skill id 0..63, or -template for an item slot
// (the wire's 0x8000 | template).
game::SkillBar parseSkillBar(const std::string& text)
{
    game::SkillBar bar{};
    std::size_t slot = 0, pos = 0;
    while (slot < bar.size() && pos <= text.size())
    {
        const std::size_t comma = std::min(text.find(',', pos), text.size());
        int v = 0;
        bool negative = false;
        for (std::size_t i = pos; i < comma; ++i)
        {
            if (text[i] == '-')
                negative = true;
            else if (text[i] >= '0' && text[i] <= '9')
                v = std::min(v * 10 + (text[i] - '0'), 65535);
        }
        if (negative)
            bar[slot++] = v >= 1 && v <= 0x7FFF ? game::barItemSlot(static_cast<std::uint16_t>(v)) : std::uint16_t{0};
        else
            bar[slot++] = static_cast<std::uint16_t>(v);
        pos = comma + 1;
    }
    return bar;
}

// "{1,0,-16,...}" for a smallint[] parameter.
std::string skillBarLiteral(const game::SkillBar& bar)
{
    std::string s = "{";
    for (std::size_t i = 0; i < bar.size(); ++i)
    {
        const int v = game::isBarItem(bar[i]) ? -static_cast<int>(game::barItemTemplate(bar[i])) : static_cast<int>(bar[i]);
        s += (i ? "," : "") + std::to_string(v);
    }
    return s + "}";
}

// The stored skill rows of a character (raw ranks; PROTOCOL_SKILLS.md §6).
game::SkillRanks selectSkillRanks(pqxx::transaction_base& tx, CharacterId character)
{
    game::SkillRanks ranks{};
    for (auto [id, rank] : tx.query<int, int>("SELECT skill_id, rank FROM character_skills WHERE char_id = $1",
                                              pqxx::params{static_cast<std::int64_t>(character)}))
        if (id >= 1 && id <= static_cast<int>(game::kMaxSkillId))
            ranks[static_cast<std::size_t>(id)] = static_cast<std::uint8_t>(std::clamp(rank, 0, static_cast<int>(game::kMaxSkillRank)));
    return ranks;
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
    c.starterKitGranted = r[first + 12].template as<bool>();
    const auto templateOrZero = [](const auto& f) -> std::uint16_t {
        if (f.is_null())
            return 0;
        const auto v = f.template as<int>();
        return v >= 0 && v <= 65535 ? static_cast<std::uint16_t>(v) : std::uint16_t{0};
    };
    c.mainHandTemplate = templateOrZero(r[first + 13]);
    c.offHandTemplate = templateOrZero(r[first + 14]);
    const auto u32 = [](const auto& f) -> std::uint32_t {
        const auto v = f.template as<std::int64_t>();
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(v, 0, std::numeric_limits<std::uint32_t>::max()));
    };
    c.xp = u32(r[first + 15]);
    if (!r[first + 16].is_null())
        c.hp = u32(r[first + 16]);
    if (!r[first + 17].is_null())
        c.mp = u32(r[first + 17]);
    c.coins = std::min(u32(r[first + 18]), net::kMaxCoins); // CHECKed 0..kMaxCoins
    for (std::size_t i = 0; i < game::kAttributeCount; ++i) // CHECKed 0..255
        c.statPoints[i] = static_cast<std::uint16_t>(
            std::clamp(r[first + 19 + static_cast<int>(i)].template as<int>(), 0, static_cast<int>(game::kAttributeCap)));
    if (!r[first + 24].is_null())
        c.skillBar = parseSkillBar(r[first + 24].template as<std::string>());
    return c;
}

template <class Row>
game::Attributes toStatPoints(const Row& r)
{
    game::Attributes a{};
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        a[i] = static_cast<std::uint16_t>(
            std::clamp(r[static_cast<int>(i)].template as<int>(), 0, static_cast<int>(game::kAttributeCap)));
    return a;
}

template <class Row>
game::StoredItem toItem(const Row& r)
{
    game::StoredItem s;
    const auto id = r[0].template as<std::int64_t>();
    if (id <= 0)
        throw std::range_error("item id out of range");
    s.id = static_cast<game::ItemInstanceId>(id);
    s.place.container = static_cast<std::uint8_t>(r[1].template as<int>());
    s.place.slot = static_cast<std::uint8_t>(r[2].template as<int>());
    s.templateId = static_cast<game::ItemTemplateId>(r[3].template as<int>()); // CHECKed 1..65535
    s.quantity = static_cast<std::uint16_t>(r[4].template as<int>());
    s.upgrade = static_cast<std::uint8_t>(std::clamp(r[5].template as<int>(), 0, static_cast<int>(game::kMaxUpgradeLevel))); // CHECKed 0..10
    return s;
}

std::vector<game::StoredItem> selectItems(pqxx::transaction_base& tx, CharacterId character)
{
    std::vector<game::StoredItem> items;
    for (const auto& r : tx.exec(std::string("SELECT ") + kItemColumns +
                                     " FROM character_items WHERE char_id = $1 ORDER BY container, slot",
                                 pqxx::params{static_cast<std::int64_t>(character)}))
        items.push_back(toItem(r));
    return items;
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
            if (r)
            {
                out.result = net::EnterWorldResult::Ok;
                out.character = toCharacter(*r);
                out.character.skillRanks = selectSkillRanks(tx, character); // PROTOCOL_SKILLS.md §6
            }
            tx.commit();
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

void PostgresAccountService::saveCharacterState(AccountId account, CharacterId character, CharacterPosition position,
                                                CharacterProgress progress, std::function<void(bool ok)> done)
{
    auto job = [this, account, character, position, progress, done] {
        bool stored = false;
        const bool ok = runDb("character state save", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto r = tx.exec(
                "UPDATE characters SET zone_id = $3, x = $4, y = $5, z = $6, yaw = $7, level = $8, xp = $9, hp = $10, "
                "mp = $11 WHERE id = $1 AND account_id = $2 AND deleted_at IS NULL",
                pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account),
                             static_cast<int>(position.zoneId), position.x, position.y, position.z, position.yaw,
                             static_cast<int>(std::max<std::uint16_t>(progress.level, 1)),
                             static_cast<std::int64_t>(progress.xp), static_cast<std::int64_t>(progress.hp),
                             static_cast<std::int64_t>(progress.mp)});
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

// ---- Items ----

void PostgresAccountService::loadCharacterItems(AccountId account, CharacterId character,
                                                std::optional<game::StarterKit> kit,
                                                std::function<void(LoadItemsOutcome)> done)
{
    auto job = [this, account, character, kit = std::move(kit), done] {
        LoadItemsOutcome out;
        bool found = false;
        std::size_t dropped = 0;
        const bool ok = runDb("character items load", [&](pqxx::connection& c) {
            out = {};
            found = false;
            dropped = 0;
            pqxx::work tx(c);
            // Locks the character row: a concurrent load (second server, duplicate login) waits
            // and then sees the flag set, so the kit is granted exactly once.
            const auto r = tx.exec("SELECT starter_kit_granted FROM characters "
                                   "WHERE id = $1 AND account_id = $2 AND deleted_at IS NULL FOR UPDATE",
                                   pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account)})
                               .opt_row();
            if (!r)
                return; // not this account's live character
            found = true;
            out.items = selectItems(tx, character);
            if (!(*r)[0].as<bool>() && kit)
            {
                for (const auto& n : game::starterKitItems(*kit, out.items, &dropped))
                {
                    const auto id = tx.query_value<std::int64_t>(
                        "INSERT INTO character_items (char_id, container, slot, template_id, quantity) "
                        "VALUES ($1, $2, $3, $4, $5) RETURNING item_id",
                        pqxx::params{static_cast<std::int64_t>(character), static_cast<int>(n.place.container),
                                     static_cast<int>(n.place.slot), static_cast<int>(n.templateId),
                                     static_cast<int>(n.quantity)});
                    out.items.push_back({static_cast<game::ItemInstanceId>(id), n.place, n.templateId, n.quantity});
                }
                tx.exec("UPDATE characters SET starter_kit_granted = true WHERE id = $1",
                        pqxx::params{static_cast<std::int64_t>(character)});
                out.kitGranted = true;
            }
            tx.commit();
        });
        out.ok = ok && found;
        if (!out.ok)
            out = {};
        if (dropped > 0 && out.kitGranted)
            VO_LOG_WARN("items: {} starter kit item(s) of character {} did not fit", dropped, character);
        done(std::move(out));
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

void PostgresAccountService::applyItemMove(AccountId account, CharacterId character,
                                           std::vector<ItemRelocation> relocations, std::function<void(bool ok)> done)
{
    auto job = [this, account, character, relocations = std::move(relocations), done] {
        bool applied = false;
        const bool ok = runDb("item move", [&](pqxx::connection& c) {
            applied = false;
            pqxx::work tx(c);
            const auto ch = static_cast<std::int64_t>(character);
            if (tx.exec("SELECT 1 FROM characters WHERE id = $1 AND account_id = $2 AND deleted_at IS NULL FOR UPDATE",
                        pqxx::params{ch, static_cast<std::int64_t>(account)})
                    .empty())
                return; // rolled back
            // The place uniqueness is checked at commit, so the rows of a swap may pass each other.
            tx.exec("SET CONSTRAINTS character_items_place_uq DEFERRED");
            for (const auto& r : relocations)
            {
                // At its expected old place, or already at the new one (a job re-run after a lost
                // connection whose commit went through).
                const auto res = tx.exec(
                    "UPDATE character_items SET container = $3, slot = $4 WHERE item_id = $1 AND char_id = $2 AND "
                    "((container = $5 AND slot = $6) OR (container = $3 AND slot = $4))",
                    pqxx::params{static_cast<std::int64_t>(r.id), ch, static_cast<int>(r.to.container),
                                 static_cast<int>(r.to.slot), static_cast<int>(r.from.container),
                                 static_cast<int>(r.from.slot)});
                if (res.affected_rows() != 1)
                {
                    VO_LOG_WARN("items: move of item {} of character {} rejected: not at ({}, {})", r.id, character,
                                r.from.container, r.from.slot);
                    return; // rolled back
                }
            }
            try
            {
                tx.commit();
                applied = true;
            }
            catch (const pqxx::unique_violation&)
            {
                VO_LOG_WARN("items: move for character {} rejected: two items on one place", character);
            }
        });
        if (done)
            done(ok && applied);
    };
    submitForAccount(account, std::move(job), [done] {
        if (done)
            done(false);
    });
}

namespace {

// The statements of one ItemWrite inside `tx` (the caller locked the character row). False if a
// stack is not as expected (the caller rolls back).
bool writeItems(pqxx::transaction_base& tx, CharacterId character, const ItemWrite& write,
                std::vector<game::ItemInstanceId>& ids)
{
    const auto ch = static_cast<std::int64_t>(character);
    for (const auto& q : write.quantities)
    {
        const pqxx::params key{static_cast<std::int64_t>(q.id), ch, static_cast<int>(q.before)};
        const auto res = q.after == 0
                             ? tx.exec("DELETE FROM character_items WHERE item_id = $1 AND char_id = $2 AND quantity = $3",
                                       key)
                             : tx.exec("UPDATE character_items SET quantity = $4 WHERE item_id = $1 AND char_id = $2 AND "
                                       "quantity = $3",
                                       pqxx::params{static_cast<std::int64_t>(q.id), ch, static_cast<int>(q.before),
                                                    static_cast<int>(q.after)});
        if (res.affected_rows() != 1)
        {
            VO_LOG_WARN("items: stack {} of character {} does not hold {} unit(s); write rejected", q.id, character,
                        q.before);
            return false;
        }
    }
    // v10 (docs/PROTOCOL_UPGRADE.md §6): an upgrade attempt's item, which must still be at its level.
    for (const auto& u : write.upgrades)
    {
        const pqxx::params key{static_cast<std::int64_t>(u.id), ch, static_cast<int>(u.before)};
        const auto res = u.destroyed
                             ? tx.exec("DELETE FROM character_items WHERE item_id = $1 AND char_id = $2 AND upgrade = $3 "
                                       "AND quantity = 1",
                                       key)
                             : tx.exec("UPDATE character_items SET upgrade = $4 WHERE item_id = $1 AND char_id = $2 AND "
                                       "upgrade = $3 AND quantity = 1",
                                       pqxx::params{static_cast<std::int64_t>(u.id), ch, static_cast<int>(u.before),
                                                    static_cast<int>(u.after)});
        if (res.affected_rows() != 1)
        {
            VO_LOG_WARN("items: item {} of character {} is not at upgrade level {}; write rejected", u.id, character,
                        u.before);
            return false;
        }
    }
    for (const auto& n : write.inserts)
    {
        const auto id = tx.query_value<std::int64_t>(
            "INSERT INTO character_items (char_id, container, slot, template_id, quantity) "
            "VALUES ($1, $2, $3, $4, $5) RETURNING item_id",
            pqxx::params{ch, static_cast<int>(n.place.container), static_cast<int>(n.place.slot),
                         static_cast<int>(n.templateId), static_cast<int>(n.quantity)});
        ids.push_back(static_cast<game::ItemInstanceId>(id));
    }
    return true;
}

} // namespace

void PostgresAccountService::applyItemWrite(AccountId account, CharacterId character, ItemWrite write,
                                            std::function<void(ItemWriteOutcome)> done)
{
    auto job = [this, account, character, write = std::move(write), done] {
        ItemWriteOutcome out;
        const bool ok = runDb("item write", [&](pqxx::connection& c) {
            out = {};
            pqxx::work tx(c);
            const auto ch = static_cast<std::int64_t>(character);
            // Locks the character row (serialises with other writes of this character) and changes the
            // coins; the CHECK refuses a balance above kMaxCoins, the WHERE one below 0 (a purchase
            // after vo_admin set-coins lowered the balance: no row, nothing changes).
            std::optional<pqxx::row> coins;
            try
            {
                coins = tx.exec("UPDATE characters SET coins = coins + $3 - $4 WHERE id = $1 AND account_id = $2 AND "
                                "deleted_at IS NULL AND coins + $3 - $4 >= 0 RETURNING coins",
                                pqxx::params{ch, static_cast<std::int64_t>(account),
                                             static_cast<std::int64_t>(write.coinsAdded),
                                             static_cast<std::int64_t>(write.coinsRemoved)})
                            .opt_row();
            }
            catch (const pqxx::check_violation&)
            {
                VO_LOG_WARN("items: coins of character {} would pass the cap; write rejected", character);
                return; // rolled back
            }
            if (!coins)
                return; // not this account's live character, or not enough coins
            std::vector<game::ItemInstanceId> ids;
            try
            {
                if (!writeItems(tx, character, write, ids))
                    return;
                tx.commit();
            }
            catch (const pqxx::unique_violation&)
            {
                VO_LOG_WARN("items: write for character {} rejected: two items on one place", character);
                return;
            }
            out.ok = true;
            out.newIds = std::move(ids);
            out.coins = static_cast<std::uint32_t>(std::clamp<std::int64_t>((*coins)[0].as<std::int64_t>(), 0,
                                                                           net::kMaxCoins));
        });
        if (!ok)
            out = {};
        done(std::move(out));
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

// ---- Quests (docs/PROTOCOL_QUESTS.md §6) ----

namespace {

constexpr const char* kQuestColumns =
    "quest_id, state, progress1, progress2, progress3, completions, "
    "COALESCE(extract(epoch FROM completed_at)::bigint, 0)";

std::vector<game::QuestRecord> selectQuests(pqxx::transaction_base& tx, CharacterId character)
{
    std::vector<game::QuestRecord> out;
    for (const auto& r : tx.exec(std::string("SELECT ") + kQuestColumns +
                                     " FROM character_quests WHERE char_id = $1 ORDER BY quest_id",
                                 pqxx::params{static_cast<std::int64_t>(character)}))
    {
        game::QuestRecord q;
        q.quest = static_cast<game::QuestId>(std::clamp(r[0].as<int>(), 0, 65535)); // CHECKed 1..999
        q.state = r[1].as<int>() == 2 ? net::QuestState::Done : net::QuestState::Active;
        for (int i = 0; i < 3; ++i)
            q.progress[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(std::clamp(r[2 + i].as<int>(), 0, 1000));
        q.completions = static_cast<std::uint32_t>(std::max<std::int64_t>(r[5].as<std::int64_t>(), 0));
        q.completedAt = r[6].as<std::int64_t>();
        out.push_back(q);
    }
    return out;
}

} // namespace

void PostgresAccountService::loadQuests(AccountId account, CharacterId character,
                                        std::function<void(LoadQuestsOutcome)> done)
{
    auto job = [this, account, character, done] {
        LoadQuestsOutcome out;
        const bool ok = runDb("quest load", [&](pqxx::connection& c) {
            out = {};
            pqxx::read_transaction tx(c);
            if (tx.exec("SELECT 1 FROM characters WHERE id = $1 AND account_id = $2 AND deleted_at IS NULL",
                        pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account)})
                    .empty())
                return;
            out.quests = selectQuests(tx, character);
            tx.commit();
            out.ok = true;
        });
        if (!ok)
            out = {};
        done(std::move(out));
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

void PostgresAccountService::applyQuestWrite(AccountId account, CharacterId character, QuestWrite write,
                                             std::function<void(QuestWriteOutcome)> done)
{
    auto job = [this, account, character, write = std::move(write), done] {
        QuestWriteOutcome out;
        const bool ok = runDb("quest write", [&](pqxx::connection& c) {
            out = {};
            pqxx::work tx(c);
            const auto ch = static_cast<std::int64_t>(character);
            // Locks the character row (serialises with its other writes and vo_admin), changes the coins as
            // applyItemWrite does and reads the stored level / xp for the experience reward.
            std::optional<pqxx::row> row;
            try
            {
                row = tx.exec("UPDATE characters SET coins = coins + $3 - $4 WHERE id = $1 AND account_id = $2 AND "
                              "deleted_at IS NULL AND coins + $3 - $4 >= 0 RETURNING coins, level, xp, "
                              "extract(epoch FROM now())::bigint",
                              pqxx::params{ch, static_cast<std::int64_t>(account),
                                           static_cast<std::int64_t>(write.items.coinsAdded),
                                           static_cast<std::int64_t>(write.items.coinsRemoved)})
                          .opt_row();
            }
            catch (const pqxx::check_violation&)
            {
                VO_LOG_WARN("quests: coins of character {} would pass the cap; write rejected", character);
                return;
            }
            if (!row)
                return; // not this account's live character, or not enough coins
            for (const QuestChange& q : write.changes)
            {
                const auto quest = static_cast<int>(q.quest);
                const int before = static_cast<int>(q.before);
                const int after = static_cast<int>(q.after);
                pqxx::result res;
                if (q.before == net::QuestState::None)
                {
                    // A new row: the primary key refuses a second one (a stale accept).
                    res = tx.exec("INSERT INTO character_quests (char_id, quest_id, state, progress1, progress2, progress3) "
                                  "VALUES ($1, $2, $3, $4, $5, $6) ON CONFLICT (char_id, quest_id) DO NOTHING",
                                  pqxx::params{ch, quest, after, static_cast<int>(q.progress[0]),
                                               static_cast<int>(q.progress[1]), static_cast<int>(q.progress[2])});
                }
                else if (q.after == net::QuestState::None)
                {
                    res = tx.exec("DELETE FROM character_quests WHERE char_id = $1 AND quest_id = $2 AND state = $3",
                                  pqxx::params{ch, quest, before});
                }
                else
                {
                    // A repeat (Done -> Active) restarts accepted_at; a hand-in counts and stamps completed_at.
                    const bool repeat = q.before == ::vo::net::QuestState::Done && q.after == ::vo::net::QuestState::Active;
                    res = tx.exec("UPDATE character_quests SET state = $4, progress1 = $5, progress2 = $6, progress3 = $7, "
                                  "completions = completions + (CASE WHEN $8 THEN 1 ELSE 0 END), "
                                  "completed_at = CASE WHEN $8 THEN now() ELSE completed_at END, "
                                  "accepted_at = CASE WHEN $9 THEN now() ELSE accepted_at END "
                                  "WHERE char_id = $1 AND quest_id = $2 AND state = $3",
                                  pqxx::params{ch, quest, before, after, static_cast<int>(q.progress[0]),
                                               static_cast<int>(q.progress[1]), static_cast<int>(q.progress[2]),
                                               q.completed, repeat});
                }
                if (res.affected_rows() != 1 && q.strict)
                {
                    VO_LOG_WARN("quests: quest {} of character {} is not in state {}; write rejected", q.quest, character,
                                before);
                    return; // rolled back
                }
            }
            std::vector<game::ItemInstanceId> ids;
            try
            {
                if (!writeItems(tx, character, write.items, ids))
                    return;
                if (write.xp > 0)
                {
                    const auto level = static_cast<std::uint16_t>(std::clamp((*row)[1].as<int>(), 1, 65535));
                    const auto xp = static_cast<std::uint32_t>(std::clamp<std::int64_t>((*row)[2].as<std::int64_t>(), 0,
                                                                                        std::numeric_limits<std::uint32_t>::max()));
                    const game::LevelProgress after = game::addExperience({level, xp}, write.xp);
                    tx.exec("UPDATE characters SET level = $2, xp = $3 WHERE id = $1",
                            pqxx::params{ch, static_cast<int>(after.level), static_cast<std::int64_t>(after.xp)});
                }
                tx.commit();
            }
            catch (const pqxx::unique_violation&)
            {
                VO_LOG_WARN("quests: write for character {} rejected: two items on one place", character);
                return;
            }
            out.ok = true;
            out.newIds = std::move(ids);
            out.coins = static_cast<std::uint32_t>(std::clamp<std::int64_t>((*row)[0].as<std::int64_t>(), 0, net::kMaxCoins));
            out.now = (*row)[3].as<std::int64_t>();
        });
        if (!ok)
            out = {};
        done(std::move(out));
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

AdminResult PostgresAccountService::listQuests(std::string_view characterName, CharacterInfo& info,
                                               std::vector<game::QuestRecord>& quests, std::string& error)
{
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    quests.clear();
    const bool ok = runDb(
        "list quests",
        [&](pqxx::connection& c) {
            pqxx::read_transaction tx(c);
            quests = selectQuests(tx, info.id);
            tx.commit();
        },
        error);
    return ok ? AdminResult::Ok : AdminResult::Error;
}

AdminResult PostgresAccountService::setQuest(std::string_view characterName, const game::QuestRecord& record,
                                             net::QuestState state, std::uint32_t doneSecondsAgo, CharacterInfo& info,
                                             std::string& error)
{
    if (record.quest == 0 || record.quest > game::kMaxQuestId ||
        std::any_of(record.progress.begin(), record.progress.end(), [](std::uint16_t p) { return p > 1000; }))
        return AdminResult::Invalid;
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    const auto ch = static_cast<std::int64_t>(info.id);
    const int quest = static_cast<int>(record.quest);
    const bool ok = runDb(
        "set quest",
        [&](pqxx::connection& c) {
            pqxx::work tx(c);
            tx.exec("SELECT 1 FROM characters WHERE id = $1 FOR UPDATE", pqxx::params{ch});
            if (state == net::QuestState::None)
                tx.exec("DELETE FROM character_quests WHERE char_id = $1 AND quest_id = $2", pqxx::params{ch, quest});
            else if (state == net::QuestState::Active)
                tx.exec("INSERT INTO character_quests (char_id, quest_id, state, progress1, progress2, progress3, completions) "
                        "VALUES ($1, $2, 1, $3, $4, $5, $6) ON CONFLICT (char_id, quest_id) DO UPDATE SET state = 1, "
                        "progress1 = EXCLUDED.progress1, progress2 = EXCLUDED.progress2, progress3 = EXCLUDED.progress3, "
                        "accepted_at = now()",
                        pqxx::params{ch, quest, static_cast<int>(record.progress[0]), static_cast<int>(record.progress[1]),
                                     static_cast<int>(record.progress[2]), static_cast<std::int64_t>(record.completions)});
            else
                tx.exec("INSERT INTO character_quests (char_id, quest_id, state, completions, completed_at) "
                        "VALUES ($1, $2, 2, $3, now() - make_interval(secs => $4)) ON CONFLICT (char_id, quest_id) DO UPDATE "
                        "SET state = 2, progress1 = 0, progress2 = 0, progress3 = 0, "
                        "completions = GREATEST(character_quests.completions, EXCLUDED.completions), "
                        "completed_at = EXCLUDED.completed_at",
                        pqxx::params{ch, quest, static_cast<std::int64_t>(std::max<std::uint32_t>(record.completions, 1)),
                                     static_cast<double>(doneSecondsAgo)});
            tx.commit();
        },
        error);
    return ok ? AdminResult::Ok : AdminResult::Error;
}

// ---- Stats ----

void PostgresAccountService::allocateStats(AccountId account, CharacterId character, StatAllocation allocation,
                                           std::function<void(StatAllocationOutcome)> done)
{
    auto job = [this, account, character, allocation, done] {
        StatAllocationOutcome out;
        const bool ok = runDb("stat allocation", [&](pqxx::connection& c) {
            out = {};
            pqxx::work tx(c);
            const auto& p = allocation.points;
            const auto& s = allocation.classStart;
            // One statement: the budget (points of the live level) and the cap are checked on the
            // values after the update; the characters_stat_budget CHECK guards the stored level too.
            std::optional<pqxx::row> r;
            try
            {
                r = tx.exec(std::string("UPDATE characters SET stat_strength = stat_strength + $3, "
                                        "stat_dexterity = stat_dexterity + $4, stat_vitality = stat_vitality + $5, "
                                        "stat_intelligence = stat_intelligence + $6, stat_spirit = stat_spirit + $7 "
                                        "WHERE id = $1 AND account_id = $2 AND deleted_at IS NULL AND "
                                        "stat_strength + stat_dexterity + stat_vitality + stat_intelligence + "
                                        "stat_spirit + $3 + $4 + $5 + $6 + $7 <= $8 AND "
                                        "stat_strength + $3 + $9 <= 255 AND stat_dexterity + $4 + $10 <= 255 AND "
                                        "stat_vitality + $5 + $11 <= 255 AND stat_intelligence + $6 + $12 <= 255 AND "
                                        "stat_spirit + $7 + $13 <= 255 RETURNING ") +
                                kStatColumns,
                            pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account),
                                         static_cast<int>(p[0]), static_cast<int>(p[1]), static_cast<int>(p[2]),
                                         static_cast<int>(p[3]), static_cast<int>(p[4]),
                                         static_cast<std::int64_t>(allocation.budget), static_cast<int>(s[0]),
                                         static_cast<int>(s[1]), static_cast<int>(s[2]), static_cast<int>(s[3]),
                                         static_cast<int>(s[4])})
                        .opt_row();
            }
            catch (const pqxx::check_violation&)
            {
                VO_LOG_WARN("stats: allocation of character {} does not fit its stored level; rejected", character);
                return; // rolled back
            }
            if (!r)
                return; // not this account's live character, or over the budget / cap
            tx.commit();
            out.ok = true;
            out.allocated = toStatPoints(*r);
        });
        if (!ok)
            out = {};
        done(out);
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

// ---- Skills (docs/PROTOCOL_SKILLS.md §6) ----

void PostgresAccountService::learnSkill(AccountId account, CharacterId character, SkillLearn learn,
                                        std::function<void(SkillLearnOutcome)> done)
{
    auto job = [this, account, character, learn, done] {
        SkillLearnOutcome out;
        const bool ok = runDb("skill learn", [&](pqxx::connection& c) {
            out = {};
            pqxx::work tx(c);
            // The character row lock serialises learns with vo_admin reset-skills / set-level.
            const auto row = tx.exec("SELECT class, level FROM characters WHERE id = $1 AND account_id = $2 AND "
                                     "deleted_at IS NULL FOR UPDATE",
                                     pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account)})
                                 .opt_row();
            if (!row)
                return; // not this account's live character: ServerError
            const auto cls = static_cast<std::uint8_t>((*row)[0].as<int>());
            const auto storedLevel = static_cast<std::uint16_t>(std::clamp((*row)[1].as<int>(), 1, 65535));
            const game::SkillCatalog& catalog = game::skills();
            game::SkillRanks ranks = game::normalizeSkillRanks(catalog, cls, selectSkillRanks(tx, character));
            const std::uint16_t level = std::min(learn.level, storedLevel);
            out.result = cls == learn.charClass ? game::checkSkillLearn(catalog, cls, level, ranks, learn.skillId)
                                                : net::SkillLearnResult::UnknownSkill;
            if (out.result == net::SkillLearnResult::Ok)
            {
                ++ranks[learn.skillId];
                tx.exec("INSERT INTO character_skills (char_id, skill_id, rank) VALUES ($1, $2, $3) "
                        "ON CONFLICT (char_id, skill_id) DO UPDATE SET rank = EXCLUDED.rank",
                        pqxx::params{static_cast<std::int64_t>(character), static_cast<int>(learn.skillId),
                                     static_cast<int>(ranks[learn.skillId])});
            }
            tx.commit();
            out.ranks = ranks;
        });
        if (!ok)
            out = {};
        done(out);
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

void PostgresAccountService::setSkillBar(AccountId account, CharacterId character, SkillBarWrite write,
                                         std::function<void(SkillBarOutcome)> done)
{
    auto job = [this, account, character, write, done] {
        SkillBarOutcome out;
        const bool ok = runDb("skill bar", [&](pqxx::connection& c) {
            out = {};
            pqxx::work tx(c);
            const auto row = tx.exec("SELECT class, array_to_string(skill_bar, ',') FROM characters WHERE id = $1 AND "
                                     "account_id = $2 AND deleted_at IS NULL FOR UPDATE",
                                     pqxx::params{static_cast<std::int64_t>(character), static_cast<std::int64_t>(account)})
                                 .opt_row();
            if (!row)
                return;
            const auto cls = static_cast<std::uint8_t>((*row)[0].as<int>());
            const game::SkillCatalog& catalog = game::skills();
            out.ranks = game::normalizeSkillRanks(catalog, cls, selectSkillRanks(tx, character));
            if (cls == write.charClass && game::validSkillBar(catalog, cls, out.ranks, write.bar))
            {
                tx.exec("UPDATE characters SET skill_bar = $2::smallint[] WHERE id = $1",
                        pqxx::params{static_cast<std::int64_t>(character), skillBarLiteral(write.bar)});
                out.result = net::SkillBarResult::Ok;
                out.bar = write.bar;
            }
            else
            {
                const game::SkillBar stored = (*row)[1].is_null() ? game::defaultSkillBar(catalog, cls)
                                                                  : parseSkillBar((*row)[1].as<std::string>());
                out.result = net::SkillBarResult::NotLearned;
                out.bar = game::sanitizeSkillBar(catalog, cls, out.ranks, stored);
            }
            tx.commit();
        });
        if (!ok)
            out = {};
        done(out);
    };
    submitForAccount(account, std::move(job), [done] { done({}); });
}

AdminResult PostgresAccountService::resetSkills(std::string_view characterName, CharacterInfo& info, std::string& error)
{
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    bool updated = false;
    if (!runDb("reset skills", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto ch = static_cast<std::int64_t>(info.id);
            const auto r = tx.exec("UPDATE characters SET skill_bar = NULL WHERE id = $1 AND deleted_at IS NULL",
                                   pqxx::params{ch});
            tx.exec("DELETE FROM character_skills WHERE char_id = $1", pqxx::params{ch});
            tx.commit();
            updated = r.affected_rows() == 1;
        }, error))
        return AdminResult::Error;
    if (!updated)
        return AdminResult::NotFound;
    info.skillRanks = {};
    info.skillBar.reset();
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::loadSkills(std::string_view characterName, CharacterInfo& info, std::string& error)
{
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    if (!runDb("load skills", [&](pqxx::connection& c) {
            pqxx::read_transaction tx(c);
            info.skillRanks = selectSkillRanks(tx, info.id);
        }, error))
        return AdminResult::Error;
    return AdminResult::Ok;
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
                constexpr int n = kCharacterColumnCount;
                a.deleted = r[n].as<bool>();
                a.createdAt = r[n + 1].as<std::string>();
                a.deletedAt = r[n + 2].as<std::string>();
                a.lastPlayedAt = r[n + 3].as<std::string>();
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

AdminResult PostgresAccountService::findCharacterByName(std::string_view name, CharacterInfo& info, std::string& error)
{
    if (!game::isValidCharacterName(name))
        return AdminResult::Invalid;
    const std::string key = game::characterNameKey(name);
    std::optional<CharacterInfo> found;
    if (!runDb("character lookup", [&](pqxx::connection& c) {
            found.reset();
            pqxx::read_transaction tx(c);
            const auto r = tx.exec(std::string("SELECT ") + kCharacterColumns +
                                       " FROM characters WHERE name_key = $1 AND deleted_at IS NULL",
                                   pqxx::params{key})
                               .opt_row();
            tx.commit();
            if (r)
                found = toCharacter(*r);
        }, error))
        return AdminResult::Error;
    if (!found)
        return AdminResult::NotFound;
    info = *found;
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::giveItem(std::string_view characterName, game::ItemTemplateId templateId,
                                             game::StoredItem& given, std::string& error)
{
    if (templateId == 0)
        return AdminResult::Invalid;
    CharacterInfo info;
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    AdminResult result = AdminResult::NotFound;
    if (!runDb("item give", [&](pqxx::connection& c) {
            result = AdminResult::NotFound;
            pqxx::work tx(c);
            const auto ch = static_cast<std::int64_t>(info.id);
            if (tx.exec("SELECT 1 FROM characters WHERE id = $1 AND deleted_at IS NULL FOR UPDATE", pqxx::params{ch})
                    .empty())
                return;
            std::array<bool, game::kBagSlots> used{};
            for (auto [slot] : tx.query<int>("SELECT slot FROM character_items WHERE char_id = $1 AND container = 1",
                                             pqxx::params{ch}))
                if (slot >= 0 && slot < static_cast<int>(used.size()))
                    used[static_cast<std::size_t>(slot)] = true;
            const auto free = std::find(used.begin(), used.end(), false);
            if (free == used.end())
            {
                result = AdminResult::NoFreeSlot;
                return;
            }
            const auto slot = static_cast<std::uint8_t>(free - used.begin());
            const auto id = tx.query_value<std::int64_t>(
                "INSERT INTO character_items (char_id, container, slot, template_id, quantity) "
                "VALUES ($1, 1, $2, $3, 1) RETURNING item_id",
                pqxx::params{ch, static_cast<int>(slot), static_cast<int>(templateId)});
            tx.commit();
            given = {static_cast<game::ItemInstanceId>(id), game::ItemPlace::bag(slot), templateId, 1};
            result = AdminResult::Ok;
        }, error))
        return AdminResult::Error;
    return result;
}

AdminResult PostgresAccountService::giveItems(std::string_view characterName, const game::ItemCatalog& catalog,
                                              game::ItemTemplateId templateId, std::uint32_t count,
                                              std::vector<game::StoredItem>& changed, std::string& error)
{
    if (!catalog.find(templateId) || count == 0)
        return AdminResult::Invalid;
    CharacterInfo info;
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    AdminResult result = AdminResult::NotFound;
    if (!runDb("item give", [&](pqxx::connection& c) {
            result = AdminResult::NotFound;
            changed.clear();
            pqxx::work tx(c);
            const auto ch = static_cast<std::int64_t>(info.id);
            if (tx.exec("SELECT 1 FROM characters WHERE id = $1 AND deleted_at IS NULL FOR UPDATE", pqxx::params{ch})
                    .empty())
                return;
            game::Inventory inv;
            std::string why;
            if (!game::Inventory::fromItems(selectItems(tx, info.id), inv, why))
                throw std::runtime_error("items of the character are inconsistent: " + why);
            const game::ItemAddPlan plan = game::planItemAdd(catalog, inv, {{templateId, count}});
            if (plan.totalPlaced() != count)
            {
                result = AdminResult::NoFreeSlot;
                return;
            }
            ItemWrite write;
            write.quantities = plan.grown;
            write.inserts = plan.created;
            std::vector<game::ItemInstanceId> ids;
            if (!writeItems(tx, info.id, write, ids))
                throw std::runtime_error("the stacks changed during the write");
            tx.commit();
            for (const auto& ch2 : plan.changes(inv, ids))
                changed.push_back({ch2.content.id, ch2.place, ch2.content.templateId, ch2.content.quantity});
            result = AdminResult::Ok;
        }, error))
        return AdminResult::Error;
    return result;
}

AdminResult PostgresAccountService::setItemUpgrade(std::string_view characterName, game::ItemInstanceId itemId,
                                                   std::uint8_t level, game::StoredItem& item, std::string& error)
{
    if (level > game::kMaxUpgradeLevel || itemId == 0)
        return AdminResult::Invalid;
    CharacterInfo info;
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    AdminResult result = AdminResult::NotFound;
    if (!runDb("set upgrade", [&](pqxx::connection& c) {
            result = AdminResult::NotFound;
            pqxx::work tx(c);
            const auto ch = static_cast<std::int64_t>(info.id);
            if (tx.exec("SELECT 1 FROM characters WHERE id = $1 AND deleted_at IS NULL FOR UPDATE", pqxx::params{ch})
                    .empty())
                return;
            const auto row = tx.exec(std::string("SELECT ") + kItemColumns +
                                         " FROM character_items WHERE item_id = $1 AND char_id = $2",
                                     pqxx::params{static_cast<std::int64_t>(itemId), ch})
                                 .opt_row();
            if (!row)
                return;
            game::StoredItem s = toItem(*row);
            if (s.quantity != 1)
            {
                result = AdminResult::Invalid;
                return;
            }
            tx.exec("UPDATE character_items SET upgrade = $3 WHERE item_id = $1 AND char_id = $2",
                    pqxx::params{static_cast<std::int64_t>(itemId), ch, static_cast<int>(level)});
            tx.commit();
            s.upgrade = level;
            item = s;
            result = AdminResult::Ok;
        }, error))
        return AdminResult::Error;
    return result;
}
AdminResult PostgresAccountService::setCoins(std::string_view characterName, std::uint32_t coins, CharacterInfo& info,
                                             std::string& error)
{
    if (coins > net::kMaxCoins)
        return AdminResult::Invalid;
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    bool updated = false;
    if (!runDb("set coins", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto r = tx.exec("UPDATE characters SET coins = $2 WHERE id = $1 AND deleted_at IS NULL",
                                   pqxx::params{static_cast<std::int64_t>(info.id), static_cast<std::int64_t>(coins)});
            tx.commit();
            updated = r.affected_rows() == 1;
        }, error))
        return AdminResult::Error;
    if (!updated)
        return AdminResult::NotFound;
    info.coins = coins;
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::setPosition(std::string_view characterName, const CharacterPosition& position,
                                                CharacterInfo& info, std::string& error)
{
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        !std::isfinite(position.yaw) || position.zoneId == 0)
        return AdminResult::Invalid;
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    bool updated = false;
    if (!runDb("set position", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto r = tx.exec("UPDATE characters SET zone_id = $2, x = $3, y = $4, z = $5, yaw = $6 "
                                   "WHERE id = $1 AND deleted_at IS NULL",
                                   pqxx::params{static_cast<std::int64_t>(info.id), static_cast<int>(position.zoneId),
                                                position.x, position.y, position.z, position.yaw});
            tx.commit();
            updated = r.affected_rows() == 1;
        }, error))
        return AdminResult::Error;
    if (!updated)
        return AdminResult::NotFound;
    info.position = position;
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::setLevel(std::string_view characterName, std::uint16_t level, CharacterInfo& info,
                                             std::string& error, bool* skillsReset)
{
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    const auto clamped = std::clamp<std::uint16_t>(level, 1, game::kLevelCap);
    bool updated = false;
    bool skillsWereReset = false;
    std::optional<game::Attributes> stats;
    if (!runDb("set level", [&](pqxx::connection& c) {
            stats.reset();
            pqxx::work tx(c);
            // hp/mp NULL: full at the new level on the next entry. Stat points that no longer fit
            // the new level are reset (PROTOCOL_STATS.md §6.2; the characters_stat_budget CHECK).
            const auto r = tx.exec(std::string("UPDATE characters SET level = $2, xp = 0, hp = NULL, mp = NULL, "
                                               "stat_strength = CASE WHEN fits THEN stat_strength ELSE 0 END, "
                                               "stat_dexterity = CASE WHEN fits THEN stat_dexterity ELSE 0 END, "
                                               "stat_vitality = CASE WHEN fits THEN stat_vitality ELSE 0 END, "
                                               "stat_intelligence = CASE WHEN fits THEN stat_intelligence ELSE 0 END, "
                                               "stat_spirit = CASE WHEN fits THEN stat_spirit ELSE 0 END "
                                               "FROM (SELECT stat_strength + stat_dexterity + stat_vitality + "
                                               "stat_intelligence + stat_spirit <= 3 * ($2 - 1) AS fits "
                                               "FROM characters WHERE id = $1) AS f "
                                               "WHERE id = $1 AND deleted_at IS NULL RETURNING ") +
                                       kStatColumns,
                                   pqxx::params{static_cast<std::int64_t>(info.id), static_cast<int>(clamped)})
                               .opt_row();
            skillsWereReset = false;
            if (r)
            {
                // PROTOCOL_SKILLS.md §6.2: skills that no longer fit the new level are reset.
                const game::SkillCatalog& catalog = game::skills();
                const auto ranks = selectSkillRanks(tx, info.id);
                if (!game::skillRanksFitLevel(catalog, info.charClass, clamped, ranks))
                {
                    tx.exec("DELETE FROM character_skills WHERE char_id = $1",
                            pqxx::params{static_cast<std::int64_t>(info.id)});
                    tx.exec("UPDATE characters SET skill_bar = NULL WHERE id = $1",
                            pqxx::params{static_cast<std::int64_t>(info.id)});
                    skillsWereReset = true;
                }
            }
            tx.commit();
            updated = r.has_value();
            if (r)
                stats = toStatPoints(*r);
        }, error))
        return AdminResult::Error;
    if (!updated)
        return AdminResult::NotFound;
    if (skillsReset)
        *skillsReset = skillsWereReset;
    if (skillsWereReset)
    {
        info.skillRanks = {};
        info.skillBar.reset();
    }
    info.level = clamped;
    info.xp = 0;
    info.hp.reset();
    info.mp.reset();
    if (stats)
        info.statPoints = *stats;
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::resetStats(std::string_view characterName, CharacterInfo& info, std::string& error)
{
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    bool updated = false;
    if (!runDb("reset stats", [&](pqxx::connection& c) {
            pqxx::work tx(c);
            const auto r = tx.exec("UPDATE characters SET stat_strength = 0, stat_dexterity = 0, stat_vitality = 0, "
                                   "stat_intelligence = 0, stat_spirit = 0 WHERE id = $1 AND deleted_at IS NULL",
                                   pqxx::params{static_cast<std::int64_t>(info.id)});
            tx.commit();
            updated = r.affected_rows() == 1;
        }, error))
        return AdminResult::Error;
    if (!updated)
        return AdminResult::NotFound;
    info.statPoints = {};
    return AdminResult::Ok;
}

AdminResult PostgresAccountService::listItems(std::string_view characterName, CharacterInfo& info,
                                              std::vector<game::StoredItem>& items, std::string& error)
{
    if (const auto r = findCharacterByName(characterName, info, error); r != AdminResult::Ok)
        return r;
    if (!runDb("item list", [&](pqxx::connection& c) {
            pqxx::read_transaction tx(c);
            items = selectItems(tx, info.id);
            tx.commit();
        }, error))
        return AdminResult::Error;
    return AdminResult::Ok;
}

} // namespace vo::server::auth