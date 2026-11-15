// PostgreSQL-only account service tests (each on a fresh schema in vedatonline_test; SKIP without
// it): concurrency limits enforced by the database, persistence across a service restart, admin
// helpers, the slot-limit trigger, and DB failures mapped to ServerError.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "shared/net/Crypto.h"
#include "tests/db/TestDb.h"

#include <catch2/catch_test_macros.hpp>

#include <pqxx/pqxx>

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace va = vo::server::auth;
namespace vdb = vo::server::db;
namespace vn = vo::net;
using CR = vn::CharCreateResult;
using AR = va::PostgresAccountService::AdminResult;
using namespace std::chrono_literals;

namespace {

// One "server": its own pool and service on the shared test schema.
struct Node
{
    Node(const vo::tests::TestSchema& schema, bool autoRegister, std::size_t threads = 4)
        : pool(schema.settings(threads)), svc(pool, {autoRegister, threads})
    {
    }
    ~Node() { svc.stop(); }
    vdb::ConnectionPool pool;
    va::PostgresAccountService svc;
};

// Collects async results from worker threads; the test thread asserts afterwards.
template <class R>
class Collector
{
public:
    explicit Collector(std::size_t expected) : m_expected(expected) {}
    std::function<void(R)> callback()
    {
        return [this](R r) {
            const std::scoped_lock lock(m_mutex);
            m_results.push_back(std::move(r));
            if (m_results.size() == m_expected)
                m_done.set_value();
        };
    }
    std::vector<R> wait()
    {
        REQUIRE(m_future.wait_for(60s) == std::future_status::ready);
        const std::scoped_lock lock(m_mutex);
        return m_results;
    }

private:
    std::size_t m_expected;
    std::mutex m_mutex;
    std::vector<R> m_results;
    std::promise<void> m_done;
    std::future<void> m_future = m_done.get_future();
};

template <class R, class F>
R await(F&& call)
{
    Collector<R> c(1);
    call(c.callback());
    return c.wait().front();
}

std::int64_t scalar(const vo::tests::TestSchema& schema, const std::string& sql)
{
    std::string error;
    auto conn = vdb::openConnection(schema.settings(), schema.url(), error);
    REQUIRE(conn);
    pqxx::nontransaction tx(*conn);
    return tx.query_value<std::int64_t>(sql);
}

} // namespace

TEST_CASE("Postgres: parallel creates on one account never exceed 3 live characters", "[db][auth][concurrency]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    // Four independent "servers" hammer the same account at once (per-account ordering inside one
    // service would otherwise serialise the requests before they reach the database).
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 4; ++i)
        nodes.push_back(std::make_unique<Node>(*schema, false));
    const auto account = nodes[0]->svc.createAccount("slot_user", "password1");
    REQUIRE(account);

    constexpr int kPerNode = 4;
    Collector<va::CreateCharacterOutcome> results(nodes.size() * kPerNode);
    int n = 0;
    for (auto& node : nodes)
        for (int i = 0; i < kPerNode; ++i)
            node->svc.createCharacter(*account, {"Slot" + std::string(1, static_cast<char>('A' + n++)), 1, 1, 0, 0},
                                      results.callback());
    int ok = 0;
    int noSlot = 0;
    for (const auto& r : results.wait())
    {
        if (r.result == CR::Ok)
            ++ok;
        else if (r.result == CR::NoFreeSlot)
            ++noSlot;
    }
    CHECK(ok == 3);
    CHECK(noSlot == 13);
    CHECK(scalar(*schema, "SELECT count(*) FROM characters WHERE deleted_at IS NULL") == 3);
    CHECK(scalar(*schema, "SELECT count(*) FROM characters") == 3);
}

TEST_CASE("Postgres: parallel creates of the same name yield exactly one Ok", "[db][auth][concurrency]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 4; ++i)
        nodes.push_back(std::make_unique<Node>(*schema, false));
    std::vector<va::AccountId> accounts;
    for (int i = 0; i < 8; ++i)
    {
        const auto id = nodes[0]->svc.createAccount("name_user" + std::to_string(i), "password1");
        REQUIRE(id);
        accounts.push_back(*id);
    }
    // Different spellings that fold to the same key, from different accounts on different nodes.
    const std::vector<std::string> spellings = {"Irmak", "IRMAK", "ırmak", "irmak", "İrmak", "IrMaK", "ıRMAK", "Irmak"};
    Collector<va::CreateCharacterOutcome> results(accounts.size());
    for (std::size_t i = 0; i < accounts.size(); ++i)
        nodes[i % nodes.size()]->svc.createCharacter(accounts[i], {spellings[i], 2, 4, 1, 1}, results.callback());
    int ok = 0;
    int taken = 0;
    for (const auto& r : results.wait())
    {
        if (r.result == CR::Ok)
            ++ok;
        else if (r.result == CR::NameTaken)
            ++taken;
    }
    CHECK(ok == 1);
    CHECK(taken == 7);
    CHECK(scalar(*schema, "SELECT count(*) FROM characters") == 1);
}

TEST_CASE("Postgres: parallel auto-registration of one username yields one account", "[db][auth][concurrency]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 4; ++i)
        nodes.push_back(std::make_unique<Node>(*schema, true));
    constexpr int kLogins = 8;
    Collector<va::LoginOutcome> results(kLogins);
    for (int i = 0; i < kLogins; ++i)
        nodes[static_cast<std::size_t>(i) % nodes.size()]->svc.login(i % 2 ? "RaceUser" : "raceuser", "same-password",
                                                                     "10.1.2.3", results.callback());
    std::set<va::AccountId> ids;
    int ok = 0;
    for (const auto& r : results.wait())
    {
        if (r.result == vn::LoginResult::Ok)
        {
            ++ok;
            ids.insert(r.accountId);
        }
    }
    CHECK(ok == kLogins);
    CHECK(ids.size() == 1);
    CHECK(scalar(*schema, "SELECT count(*) FROM accounts") == 1);
    CHECK(scalar(*schema, "SELECT count(*) FROM accounts WHERE last_login_ip = '10.1.2.3'") == 1);

    // Concurrent registration with different passwords: one wins, the others are rejected.
    Collector<va::LoginOutcome> mixed(4);
    for (int i = 0; i < 4; ++i)
        nodes[static_cast<std::size_t>(i)]->svc.login("Mixed_User", "password-" + std::to_string(i), "",
                                                     mixed.callback());
    int mixedOk = 0;
    int mixedInvalid = 0;
    for (const auto& r : mixed.wait())
    {
        mixedOk += r.result == vn::LoginResult::Ok;
        mixedInvalid += r.result == vn::LoginResult::InvalidCredentials;
    }
    CHECK(mixedOk == 1);
    CHECK(mixedInvalid == 3);
    CHECK(scalar(*schema, "SELECT count(*) FROM accounts") == 2);
}

TEST_CASE("Postgres: data survives a service restart", "[db][auth][persistence]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    va::AccountId account = 0;
    va::CharacterId kept = 0;
    va::CharacterId deleted = 0;
    {
        Node node(*schema, true);
        const auto login = await<va::LoginOutcome>([&](auto cb) { node.svc.login("Persist_Me", "password1", "::1", cb); });
        REQUIRE(login.result == vn::LoginResult::Ok);
        account = login.accountId;
        const auto c1 = await<va::CreateCharacterOutcome>(
            [&](auto cb) { node.svc.createCharacter(account, {"Kalıcı", 2, 3, 1, 2}, cb); });
        REQUIRE(c1.result == CR::Ok);
        kept = c1.character.id;
        const auto c2 =
            await<va::CreateCharacterOutcome>([&](auto cb) { node.svc.createCharacter(account, {"Silinen", 1, 1, 0, 0}, cb); });
        REQUIRE(c2.result == CR::Ok);
        deleted = c2.character.id;
        REQUIRE(await<va::DeleteCharacterOutcome>([&](auto cb) { node.svc.deleteCharacter(account, deleted, cb); })
                    .result == vn::CharDeleteResult::Ok);
        // Fire-and-forget save right before shutdown: stop() drains it.
        node.svc.saveCharacterPosition(account, kept, {1, 100.5f, 3.25f, 200.75f, -1.0f}, nullptr);
    } // service stopped, pool closed

    Node node(*schema, false); // auto-registration off: the account must really exist
    const auto login = await<va::LoginOutcome>([&](auto cb) { node.svc.login("persist_me", "password1", "", cb); });
    REQUIRE(login.result == vn::LoginResult::Ok);
    CHECK(login.accountId == account);
    CHECK(await<va::LoginOutcome>([&](auto cb) { node.svc.login("persist_me", "wrong-pass", "", cb); }).result ==
          vn::LoginResult::InvalidCredentials);
    const auto list = await<va::CharacterListOutcome>([&](auto cb) { node.svc.listCharacters(account, cb); });
    REQUIRE(list.ok);
    REQUIRE(list.characters.size() == 1);
    const auto& c = list.characters[0];
    CHECK(c.id == kept);
    CHECK(c.name == "Kalıcı");
    CHECK(c.race == 2);
    CHECK(c.charClass == 3);
    CHECK(c.face == 1);
    CHECK(c.hair == 2);
    CHECK(c.level == 1);
    const auto loaded = await<va::LoadCharacterOutcome>([&](auto cb) { node.svc.loadCharacterForWorld(account, kept, cb); });
    REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
    CHECK(loaded.character.position.x == 100.5f);
    CHECK(loaded.character.position.y == 3.25f);
    CHECK(loaded.character.position.z == 200.75f);
    CHECK(loaded.character.position.yaw == -1.0f);
    // The deleted name is still reserved after the restart.
    CHECK(await<va::CreateCharacterOutcome>([&](auto cb) { node.svc.createCharacter(account, {"SİLİNEN", 1, 1, 0, 0}, cb); })
              .result == CR::NameTaken);
    CHECK(scalar(*schema, "SELECT count(*) FROM characters WHERE last_played_at IS NOT NULL") == 1);
    CHECK(scalar(*schema, "SELECT count(*) FROM accounts WHERE last_login_at IS NOT NULL") == 1);
}

TEST_CASE("Postgres: administration helpers", "[db][auth][admin]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    Node node(*schema, false);
    auto& svc = node.svc;
    std::string error;
    va::AccountId id = 0;
    REQUIRE(svc.createAccount("Admin_Test", "password1", id, error) == AR::Ok);
    CHECK(svc.createAccount("admin_test", "password2", id, error) == AR::Taken);
    CHECK(svc.createAccount("no", "password2", id, error) == AR::Invalid);

    CHECK(svc.setBannedByName("ADMIN_TEST", true, "spamming the market", error) == AR::Ok);
    CHECK(svc.setBannedByName("nobody_here", true, "", error) == AR::NotFound);
    CHECK(scalar(*schema, "SELECT count(*) FROM accounts WHERE banned AND ban_reason = 'spamming the market'") == 1);
    CHECK(await<va::LoginOutcome>([&](auto cb) { svc.login("admin_test", "password1", "", cb); }).result ==
          vn::LoginResult::Banned);
    CHECK(svc.setBannedByName("admin_test", false, "", error) == AR::Ok);
    CHECK(scalar(*schema, "SELECT count(*) FROM accounts WHERE NOT banned AND ban_reason IS NULL") == 1);

    CHECK(svc.setPassword("admin_test", "new-password", error) == AR::Ok);
    CHECK(svc.setPassword("admin_test", "x", error) == AR::Invalid);
    CHECK(await<va::LoginOutcome>([&](auto cb) { svc.login("admin_test", "password1", "", cb); }).result ==
          vn::LoginResult::InvalidCredentials);
    CHECK(await<va::LoginOutcome>([&](auto cb) { svc.login("admin_test", "new-password", "", cb); }).result ==
          vn::LoginResult::Ok);

    std::vector<va::CharacterId> ids;
    for (const char* name : {"Birinci", "İkinci", "Üçüncü"})
    {
        const auto c = await<va::CreateCharacterOutcome>([&](auto cb) { svc.createCharacter(id, {name, 1, 2, 0, 0}, cb); });
        REQUIRE(c.result == CR::Ok);
        ids.push_back(c.character.id);
    }
    REQUIRE(await<va::DeleteCharacterOutcome>([&](auto cb) { svc.deleteCharacter(id, ids[0], cb); }).result ==
            vn::CharDeleteResult::Ok);

    std::vector<va::PostgresAccountService::AdminCharacter> chars;
    REQUIRE(svc.listAllCharacters("admin_test", chars, error) == AR::Ok);
    REQUIRE(chars.size() == 3);
    CHECK(chars[0].deleted);
    CHECK_FALSE(chars[0].deletedAt.empty());
    CHECK_FALSE(chars[1].deleted);
    CHECK(chars[1].createdAt.ends_with("Z"));

    va::PostgresAccountService::Counts counts;
    REQUIRE(svc.counts(counts, error) == AR::Ok);
    CHECK(counts.accounts == 1);
    CHECK(counts.liveCharacters == 2);
    CHECK(counts.deletedCharacters == 1);

    // Restore respects the slot limit (trigger), rename respects uniqueness, purge frees the name.
    REQUIRE(await<va::CreateCharacterOutcome>([&](auto cb) { svc.createCharacter(id, {"Dolduran", 1, 1, 0, 0}, cb); })
                .result == CR::Ok);
    CHECK(svc.restoreCharacter("admin_test", ids[0], error) == AR::NoFreeSlot);
    CHECK(svc.restoreCharacter("admin_test", ids[1], error) == AR::NotFound); // not deleted
    CHECK(svc.renameCharacter("admin_test", ids[1], "ÜÇÜNCÜ", error) == AR::Taken);
    CHECK(svc.renameCharacter("admin_test", ids[1], "1x", error) == AR::Invalid);
    CHECK(svc.renameCharacter("admin_test", ids[1], "Yeniİsim", error) == AR::Ok);
    CHECK(await<va::CreateCharacterOutcome>([&](auto cb) { svc.createCharacter(id, {"İkinci", 1, 1, 0, 0}, cb); })
              .result == CR::NoFreeSlot);
    CHECK(svc.purgeCharacter("admin_test", ids[1], error) == AR::NotFound); // only deleted ones
    CHECK(svc.purgeCharacter("admin_test", ids[0], error) == AR::Ok);
    REQUIRE(await<va::DeleteCharacterOutcome>([&](auto cb) { svc.deleteCharacter(id, ids[2], cb); }).result ==
            vn::CharDeleteResult::Ok);
    CHECK(await<va::CreateCharacterOutcome>([&](auto cb) { svc.createCharacter(id, {"BİRİNCİ", 1, 1, 0, 0}, cb); })
              .result == CR::Ok); // purged name is free again
    CHECK(svc.restoreCharacter("admin_test", ids[2], error) == AR::NoFreeSlot);

    // Deleting the account removes it with every character and releases the names.
    std::int64_t removed = 0;
    CHECK(svc.deleteAccount("nobody_here", removed, error) == AR::NotFound);
    REQUIRE(svc.deleteAccount("ADMIN_TEST", removed, error) == AR::Ok);
    CHECK(removed == 4);
    CHECK(scalar(*schema, "SELECT count(*) FROM accounts") == 0);
    CHECK(scalar(*schema, "SELECT count(*) FROM characters") == 0);
    va::AccountId again = 0;
    REQUIRE(svc.createAccount("admin_test", "password1", again, error) == AR::Ok);
    CHECK(again != id);
    CHECK(await<va::CreateCharacterOutcome>([&](auto cb) { svc.createCharacter(again, {"Üçüncü", 1, 1, 0, 0}, cb); })
              .result == CR::Ok);
}

TEST_CASE("Postgres: schema constraints back the protocol rules", "[db][auth][schema]")
{
    REQUIRE(vn::initCrypto());
    VO_TEST_SCHEMA(schema);
    Node node(*schema, false);
    const auto account = node.svc.createAccount("schema_user", "password1");
    REQUIRE(account);
    std::string error;
    auto conn = vdb::openConnection(schema->settings(), schema->url(), error);
    REQUIRE(conn);
    const auto insertFails = [&](const std::string& values) {
        try
        {
            pqxx::work tx(*conn);
            tx.exec("INSERT INTO characters (account_id, name, name_key, race, class, face, hair, zone_id, x, y, z, "
                    "yaw) VALUES " +
                    values);
            tx.commit();
            return false;
        }
        catch (const pqxx::sql_error&)
        {
            return true;
        }
    };
    const std::string acc = std::to_string(*account);
    CHECK_FALSE(insertFails("(" + acc + ", 'Geçerli', 'geçerli', 1, 1, 0, 0, 1, 0, 0, 0, 0)"));
    CHECK(insertFails("(" + acc + ", 'Geçerli2', 'geçerli', 1, 1, 0, 0, 1, 0, 0, 0, 0)")); // name_key unique
    CHECK(insertFails("(" + acc + ", '1abc', '1abc', 1, 1, 0, 0, 1, 0, 0, 0, 0)"));         // starts with a digit
    CHECK(insertFails("(" + acc + ", 'Ab', 'ab', 1, 1, 0, 0, 1, 0, 0, 0, 0)"));             // too short
    CHECK(insertFails("(" + acc + ", 'Irkı', 'ırkı', 3, 1, 0, 0, 1, 0, 0, 0, 0)"));         // race
    CHECK(insertFails("(" + acc + ", 'Sınıf', 'sınıf', 1, 5, 0, 0, 1, 0, 0, 0, 0)"));       // class
    CHECK(insertFails("(" + acc + ", 'Yüzü', 'yüzü', 1, 1, 4, 0, 1, 0, 0, 0, 0)"));         // face
    CHECK(insertFails("(" + acc + ", 'Konum', 'konum', 1, 1, 0, 0, 1, 'NaN', 0, 0, 0)"));   // NaN position
    CHECK_FALSE(insertFails("(" + acc + ", 'İkinci', 'ikinci', 1, 1, 0, 0, 1, 0, 0, 0, 0)"));
    CHECK_FALSE(insertFails("(" + acc + ", 'Üçüncü', 'üçüncü', 1, 1, 0, 0, 1, 0, 0, 0, 0)"));
    CHECK(insertFails("(" + acc + ", 'Dördüncü', 'dördüncü', 1, 1, 0, 0, 1, 0, 0, 0, 0)")); // slot-limit trigger
    try
    {
        pqxx::work tx(*conn);
        tx.exec("INSERT INTO accounts (username, username_key, password_hash) VALUES ('Bad_Key', 'Bad_Key', "
                "'$argon2id$x')");
        tx.commit();
        FAIL("username_key must be the lower-case username");
    }
    catch (const pqxx::check_violation&)
    {
    }
}

TEST_CASE("Postgres: database failures map to ServerError and the service stays usable", "[db][auth]")
{
    REQUIRE(vn::initCrypto());
    // Unreachable database (refused port): every request completes with the ServerError result.
    vdb::DbSettings s;
    s.connectionString = "host=127.0.0.1 port=1 dbname=vedatonline_test user=nobody connect_timeout=2";
    s.poolSize = 1;
    vdb::ConnectionPool pool(s);
    va::PostgresAccountService svc(pool, {true, 1});
    CHECK(await<va::LoginOutcome>([&](auto cb) { svc.login("someone", "password1", "", cb); }).result ==
          vn::LoginResult::ServerError);
    CHECK_FALSE(await<va::CharacterListOutcome>([&](auto cb) { svc.listCharacters(1, cb); }).ok);
    CHECK(await<va::CreateCharacterOutcome>([&](auto cb) { svc.createCharacter(1, {"Hata", 1, 1, 0, 0}, cb); }).result ==
          CR::ServerError);
    CHECK(await<va::DeleteCharacterOutcome>([&](auto cb) { svc.deleteCharacter(1, 1, cb); }).result ==
          vn::CharDeleteResult::ServerError);
    CHECK(await<va::LoadCharacterOutcome>([&](auto cb) { svc.loadCharacterForWorld(1, 1, cb); }).result ==
          vn::EnterWorldResult::ServerError);
    CHECK_FALSE(await<bool>([&](auto cb) { svc.saveCharacterPosition(1, 1, {}, cb); }));
    // Validation still happens before any DB access.
    CHECK(await<va::CreateCharacterOutcome>([&](auto cb) { svc.createCharacter(1, {"x", 1, 1, 0, 0}, cb); }).result ==
          CR::NameInvalid);
    CHECK(await<va::LoginOutcome>([&](auto cb) { svc.login("ab", "password1", "", cb); }).result ==
          vn::LoginResult::InvalidCredentials);
    svc.stop();
}
