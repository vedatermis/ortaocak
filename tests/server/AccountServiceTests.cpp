// Behavioural suite for IAccountService, run against BOTH implementations: MemoryAccountService and
// PostgresAccountService (on a fresh schema in vedatonline_test; those variants SKIP without a
// test database, see tests/db/TestDb.h). Both must give identical results for every rule of
// docs/PROTOCOL.md §5.2-5.3. PostgreSQL-only behaviour (concurrency, persistence, pool recovery,
// migrations) lives in tests/db/.

#include "server/auth/LoginRateLimiter.h"
#include "server/auth/MemoryAccountService.h"
#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "shared/net/Crypto.h"
#include "tests/db/TestDb.h"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace va = vo::server::auth;
namespace vdb = vo::server::db;
namespace vn = vo::net;
using CR = vn::CharCreateResult;

namespace {

// ---- Backends: same helper surface for both implementations ----

struct MemoryBackend
{
    explicit MemoryBackend(bool autoRegister, va::NewCharacterSpawns spawns = {})
        : svc(va::MemoryAccountService::Options{autoRegister, spawns})
    {
    }

    va::IAccountService& service() { return svc; }
    std::optional<va::AccountId> createAccount(const std::string& u, const std::string& p)
    {
        return svc.createAccount(u, p);
    }
    void setBanned(va::AccountId id, bool banned) { svc.setBanned(id, banned); }
    std::size_t accountCount() { return svc.accountCount(); }

    va::MemoryAccountService svc;
};

struct PostgresBackend
{
    explicit PostgresBackend(bool autoRegister, va::NewCharacterSpawns spawns = {})
    {
        VO_TEST_SCHEMA(s);
        schema = std::move(s);
        pool = std::make_unique<vdb::ConnectionPool>(schema->settings());
        svc = std::make_unique<va::PostgresAccountService>(*pool,
                                                           va::PostgresAccountService::Options{autoRegister, 4, spawns});
    }
    ~PostgresBackend()
    {
        if (svc)
            svc->stop();
        svc.reset();
        pool.reset();
    }
    PostgresBackend(const PostgresBackend&) = delete;
    PostgresBackend& operator=(const PostgresBackend&) = delete;

    va::IAccountService& service() { return *svc; }
    std::optional<va::AccountId> createAccount(const std::string& u, const std::string& p)
    {
        return svc->createAccount(u, p);
    }
    void setBanned(va::AccountId id, bool banned) { svc->setBanned(id, banned); }
    std::size_t accountCount() { return svc->accountCount(); }

    std::unique_ptr<vo::tests::TestSchema> schema; // declared first: dropped last
    std::unique_ptr<vdb::ConnectionPool> pool;
    std::unique_ptr<va::PostgresAccountService> svc;
};

// Calls an async service method and blocks until its callback fires (on a worker thread).
template <class R, class F>
R await(F&& call)
{
    std::promise<R> promise;
    auto future = promise.get_future();
    call([&promise](R result) { promise.set_value(std::move(result)); });
    REQUIRE(future.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    return future.get();
}

va::LoginOutcome login(va::IAccountService& s, const std::string& user, const std::string& pw)
{
    return await<va::LoginOutcome>([&](auto done) { s.login(user, pw, "127.0.0.1", done); });
}

va::CreateCharacterOutcome create(va::IAccountService& s, va::AccountId acc, const std::string& name,
                                  std::uint8_t race = 1, std::uint8_t cls = 1, std::uint8_t face = 0,
                                  std::uint8_t hair = 0)
{
    return await<va::CreateCharacterOutcome>(
        [&](auto done) { s.createCharacter(acc, {name, race, cls, face, hair}, done); });
}

va::CharacterListOutcome list(va::IAccountService& s, va::AccountId acc)
{
    return await<va::CharacterListOutcome>([&](auto done) { s.listCharacters(acc, done); });
}

va::DeleteCharacterOutcome remove(va::IAccountService& s, va::AccountId acc, va::CharacterId id)
{
    return await<va::DeleteCharacterOutcome>([&](auto done) { s.deleteCharacter(acc, id, done); });
}

va::LoadCharacterOutcome load(va::IAccountService& s, va::AccountId acc, va::CharacterId id)
{
    return await<va::LoadCharacterOutcome>([&](auto done) { s.loadCharacterForWorld(acc, id, done); });
}

bool save(va::IAccountService& s, va::AccountId acc, va::CharacterId id, va::CharacterPosition pos)
{
    return await<bool>([&](auto done) { s.saveCharacterPosition(acc, id, pos, done); });
}

} // namespace

TEMPLATE_TEST_CASE("AccountService login without auto-registration", "[server][auth]", MemoryBackend,
                   PostgresBackend)
{
    REQUIRE(vn::initCrypto());
    TestType b(false);
    auto& svc = b.service();
    CHECK(login(svc, "nobody", "password1").result == vn::LoginResult::InvalidCredentials);
    CHECK(b.accountCount() == 0); // auto-registration is off: nothing was created

    const auto id = b.createAccount("Vedat_01", "password1");
    REQUIRE(id.has_value());
    CHECK(*id != 0);
    CHECK_FALSE(b.createAccount("vedat_01", "otherpass").has_value()); // case-insensitive duplicate
    CHECK_FALSE(b.createAccount("bad name", "password1").has_value());
    CHECK_FALSE(b.createAccount("gooduser", "short").has_value());
    CHECK(b.accountCount() == 1);

    auto ok = login(svc, "vedat_01", "password1"); // case-insensitive username
    CHECK(ok.result == vn::LoginResult::Ok);
    CHECK(ok.accountId == *id);
    CHECK(login(svc, "VEDAT_01", "password1").result == vn::LoginResult::Ok);

    const auto wrong = login(svc, "Vedat_01", "password2");
    CHECK(wrong.result == vn::LoginResult::InvalidCredentials);
    CHECK(wrong.accountId == 0);
    CHECK(login(svc, "abc", "password1").result == vn::LoginResult::InvalidCredentials); // malformed name
    CHECK(login(svc, "Vedat_01", "pw").result == vn::LoginResult::InvalidCredentials);   // malformed password
    CHECK(login(svc, "Vedat_01", std::string(65, 'x')).result == vn::LoginResult::InvalidCredentials);

    b.setBanned(*id, true);
    CHECK(login(svc, "Vedat_01", "password1").result == vn::LoginResult::Banned);
    CHECK(login(svc, "Vedat_01", "password1").accountId == 0);
    CHECK(login(svc, "Vedat_01", "nope-nope").result == vn::LoginResult::InvalidCredentials); // no ban leak
    b.setBanned(*id, false);
    CHECK(login(svc, "Vedat_01", "password1").result == vn::LoginResult::Ok);
    CHECK(b.accountCount() == 1);
}

TEMPLATE_TEST_CASE("AccountService auto-registration", "[server][auth]", MemoryBackend, PostgresBackend)
{
    REQUIRE(vn::initCrypto());
    TestType b(true);
    auto& svc = b.service();
    const auto first = login(svc, "NewPlayer", "secret123");
    REQUIRE(first.result == vn::LoginResult::Ok);
    CHECK(first.accountId != 0);
    CHECK(b.accountCount() == 1);

    const auto again = login(svc, "newplayer", "secret123");
    CHECK(again.result == vn::LoginResult::Ok);
    CHECK(again.accountId == first.accountId);
    CHECK(login(svc, "NEWPLAYER", "different1").result == vn::LoginResult::InvalidCredentials);

    CHECK(login(svc, "x", "secret123").result == vn::LoginResult::InvalidCredentials);
    CHECK(login(svc, "validname", "12345").result == vn::LoginResult::InvalidCredentials);
    CHECK(login(svc, "with space", "secret123").result == vn::LoginResult::InvalidCredentials);
    CHECK(b.accountCount() == 1);

    // A banned auto-registered account stays banned (no re-registration).
    b.setBanned(first.accountId, true);
    CHECK(login(svc, "NewPlayer", "secret123").result == vn::LoginResult::Banned);
    CHECK(b.accountCount() == 1);

    const auto second = login(svc, "Other_Player", "secret456");
    REQUIRE(second.result == vn::LoginResult::Ok);
    CHECK(second.accountId != first.accountId);
    CHECK(b.accountCount() == 2);
}

TEMPLATE_TEST_CASE("AccountService character rules", "[server][auth]", MemoryBackend, PostgresBackend)
{
    REQUIRE(vn::initCrypto());
    TestType b(false);
    auto& svc = b.service();
    const auto a = *b.createAccount("player_a", "password1");
    const auto bb = *b.createAccount("player_b", "password1");

    SECTION("validation order and result codes")
    {
        CHECK(create(svc, a, "1bad", 9, 9, 9, 9).result == CR::NameInvalid);
        CHECK(create(svc, a, "Al").result == CR::NameInvalid);
        CHECK(create(svc, a, "Abcdefghijklmnopq").result == CR::NameInvalid); // 17 characters
        CHECK(create(svc, a, "Ali Veli").result == CR::NameInvalid);
        CHECK(create(svc, a, "Ali", 0, 1).result == CR::InvalidRaceOrClass);
        CHECK(create(svc, a, "Ali", 3, 1).result == CR::InvalidRaceOrClass);
        CHECK(create(svc, a, "Ali", 1, 5, 9, 9).result == CR::InvalidRaceOrClass);
        CHECK(create(svc, a, "Ali", 2, 4, 4, 0).result == CR::InvalidAppearance);
        CHECK(create(svc, a, "Ali", 2, 4, 0, 4).result == CR::InvalidAppearance);
        CHECK(list(svc, a).characters.empty());
        // Unknown account: storage-level error, as both implementations treat it.
        CHECK(create(svc, 999999, "Kimse").result == CR::ServerError);
    }

    SECTION("create, list, three slots, soft delete keeps the name reserved")
    {
        const auto c1 = create(svc, a, "Irmak", 1, 1, 3, 3);
        REQUIRE(c1.result == CR::Ok);
        CHECK(c1.character.id != 0);
        CHECK(c1.character.name == "Irmak");
        CHECK(c1.character.level == 1);
        CHECK(c1.character.position.zoneId == 1);
        CHECK(c1.character.position.x == 256.0f);
        CHECK(c1.character.position.z == 256.0f);
        CHECK(c1.character.face == 3);
        CHECK(c1.character.hair == 3);
        const auto c2 = create(svc, a, "Çağrı", 2, 2);
        REQUIRE(c2.result == CR::Ok);
        CHECK(c2.character.name == "Çağrı");
        REQUIRE(create(svc, a, "İnci", 1, 3).result == CR::Ok);

        auto l = list(svc, a);
        REQUIRE(l.ok);
        REQUIRE(l.characters.size() == 3);
        CHECK(l.characters[0].name == "Irmak"); // ordered by id (creation order)
        CHECK(l.characters[1].name == "Çağrı");
        CHECK(l.characters[2].name == "İnci");
        CHECK(l.characters[0].race == 1);
        CHECK(l.characters[1].race == 2);
        CHECK(l.characters[1].charClass == 2);
        CHECK(create(svc, a, "Dördüncü").result == CR::NoFreeSlot);
        CHECK(create(svc, a, "Irmak").result == CR::NoFreeSlot); // slot check comes before the name check
        auto lb = list(svc, bb);
        CHECK(lb.ok);
        CHECK(lb.characters.empty());

        // Turkish-aware uniqueness, across accounts: all four i-like letters fold to one key.
        CHECK(create(svc, bb, "IRMAK").result == CR::NameTaken);
        CHECK(create(svc, bb, "ırmak").result == CR::NameTaken);
        CHECK(create(svc, bb, "İrmak").result == CR::NameTaken);
        CHECK(create(svc, bb, "irmak").result == CR::NameTaken);
        CHECK(create(svc, bb, "ÇAĞRI").result == CR::NameTaken);
        CHECK(create(svc, bb, "çağri").result == CR::NameTaken);
        CHECK(create(svc, bb, "inci").result == CR::NameTaken);
        CHECK(create(svc, bb, "INCI").result == CR::NameTaken);
        CHECK(create(svc, bb, "Pinar").result == CR::Ok);
        CHECK(create(svc, bb, "PINAR").result == CR::NameTaken);
        CHECK(create(svc, bb, "Pınar").result == CR::NameTaken);

        // Delete: the other account cannot, the owner can once.
        CHECK(remove(svc, bb, c1.character.id).result == vn::CharDeleteResult::NotFound);
        CHECK(remove(svc, a, c1.character.id).result == vn::CharDeleteResult::Ok);
        CHECK(remove(svc, a, c1.character.id).result == vn::CharDeleteResult::NotFound);
        CHECK(remove(svc, a, 999999).result == vn::CharDeleteResult::NotFound);
        l = list(svc, a);
        REQUIRE(l.characters.size() == 2);
        CHECK(l.characters[0].name == "Çağrı");

        // The slot is free again, but the deleted name stays reserved (for every account).
        CHECK(create(svc, a, "Irmak").result == CR::NameTaken);
        CHECK(create(svc, a, "IRMAK").result == CR::NameTaken);
        CHECK(create(svc, bb, "ırmak").result == CR::NameTaken);
        CHECK(create(svc, a, "Yeni").result == CR::Ok);
        CHECK(create(svc, a, "Fazla").result == CR::NoFreeSlot);
        CHECK(load(svc, a, c1.character.id).result == vn::EnterWorldResult::NotFound);
        CHECK_FALSE(save(svc, a, c1.character.id, {1, 1.0f, 2.0f, 3.0f, 0.0f}));
    }

    SECTION("load for world and save position, cross-account isolation")
    {
        const auto c = create(svc, a, "Gezgin");
        REQUIRE(c.result == CR::Ok);
        const auto loaded = load(svc, a, c.character.id);
        REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
        CHECK(loaded.character.name == "Gezgin");
        CHECK(loaded.character.id == c.character.id);
        CHECK(load(svc, bb, c.character.id).result == vn::EnterWorldResult::NotFound);
        CHECK(load(svc, a, 999999).result == vn::EnterWorldResult::NotFound);

        const va::CharacterPosition pos{1, 10.25f, 2.0f, 30.5f, 1.5f};
        CHECK(save(svc, a, c.character.id, pos));
        CHECK_FALSE(save(svc, bb, c.character.id, {1, 99.0f, 0.0f, 99.0f, 0.0f}));
        CHECK_FALSE(save(svc, a, 999999, pos));
        const auto after = load(svc, a, c.character.id);
        REQUIRE(after.result == vn::EnterWorldResult::Ok);
        CHECK(after.character.position.zoneId == 1);
        CHECK(after.character.position.x == 10.25f);
        CHECK(after.character.position.y == 2.0f);
        CHECK(after.character.position.z == 30.5f);
        CHECK(after.character.position.yaw == 1.5f);
        // Fire-and-forget save (empty callback) must be accepted too.
        svc.saveCharacterPosition(a, c.character.id, {1, 11.0f, 2.0f, 31.0f, 0.5f}, nullptr);
        CHECK(load(svc, a, c.character.id).character.position.x == 11.0f); // FIFO per account
        CHECK(remove(svc, bb, c.character.id).result == vn::CharDeleteResult::NotFound);
        CHECK(list(svc, a).characters.size() == 1);
    }
}

TEMPLATE_TEST_CASE("AccountService new characters start at their race's spawn point", "[server][auth][world]",
                   MemoryBackend, PostgresBackend)
{
    REQUIRE(vn::initCrypto());
    va::NewCharacterSpawns spawns;
    spawns.aydor = {1, 238.0f, 21.5f, 271.0f, 2.25f};
    spawns.kargan = {1, 281.0f, 19.25f, 236.0f, -0.75f};
    TestType b(false, spawns);
    auto& svc = b.service();
    const auto acc = *b.createAccount("spawn_test", "password1");
    const auto aydor = create(svc, acc, "Aydorlu", 1, 1);
    const auto kargan = create(svc, acc, "Karganlı", 2, 3);
    REQUIRE(aydor.result == CR::Ok);
    REQUIRE(kargan.result == CR::Ok);
    const auto checkAt = [&](va::CharacterId id, const va::CharacterPosition& expected) {
        const auto loaded = load(svc, acc, id);
        REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
        const auto& p = loaded.character.position;
        CHECK(p.zoneId == expected.zoneId);
        CHECK(p.x == expected.x);
        CHECK(p.y == expected.y);
        CHECK(p.z == expected.z);
        CHECK(p.yaw == expected.yaw);
    };
    checkAt(aydor.character.id, spawns.aydor);
    checkAt(kargan.character.id, spawns.kargan);
    CHECK(aydor.character.position.x == 238.0f);
    CHECK(kargan.character.position.z == 236.0f);
}

TEMPLATE_TEST_CASE("AccountService completes on its worker thread and after stop()", "[server][auth]",
                   MemoryBackend, PostgresBackend)
{
    REQUIRE(vn::initCrypto());
    TestType b(false);
    auto& svc = b.service();
    const auto testThread = std::this_thread::get_id();
    const auto cbThread = await<std::thread::id>([&](auto done) {
        svc.listCharacters(1, [done](va::CharacterListOutcome) { done(std::this_thread::get_id()); });
    });
    CHECK(cbThread != testThread);

    svc.stop();
    bool called = false;
    svc.login("someone", "password1", "", [&](va::LoginOutcome o) {
        called = true;
        CHECK(o.result == vn::LoginResult::ServerError);
    });
    CHECK(called); // synchronous ServerError once stopped
    bool listed = false;
    svc.listCharacters(1, [&](va::CharacterListOutcome o) {
        listed = true;
        CHECK_FALSE(o.ok);
    });
    CHECK(listed);
    bool created = false;
    svc.createCharacter(1, {"Sonra", 1, 1, 0, 0}, [&](va::CreateCharacterOutcome o) {
        created = true;
        CHECK(o.result == CR::ServerError);
    });
    CHECK(created);
    svc.stop(); // idempotent
}

TEST_CASE("LoginRateLimiter allows 5 failures per address per minute", "[server][auth]")
{
    auto now = vo::core::monoNow();
    va::LoginRateLimiter limiter(5, std::chrono::minutes(1), [&] { return now; });

    for (int i = 0; i < 5; ++i)
    {
        CHECK_FALSE(limiter.isBlocked("10.0.0.1"));
        limiter.recordFailure("10.0.0.1");
        now += std::chrono::seconds(1);
    }
    CHECK(limiter.isBlocked("10.0.0.1"));
    CHECK_FALSE(limiter.isBlocked("10.0.0.2")); // per address

    // Sliding window: the first failure (t=0) expires at t=60 s.
    now += std::chrono::seconds(54); // t = 59 s
    CHECK(limiter.isBlocked("10.0.0.1"));
    now += std::chrono::seconds(1); // t = 60 s
    CHECK_FALSE(limiter.isBlocked("10.0.0.1"));
    limiter.recordFailure("10.0.0.1");
    CHECK(limiter.isBlocked("10.0.0.1"));

    now += std::chrono::minutes(2);
    CHECK_FALSE(limiter.isBlocked("10.0.0.1"));
    CHECK(limiter.trackedAddresses() == 0);
}
