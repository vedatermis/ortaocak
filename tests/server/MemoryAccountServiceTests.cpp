#include "server/auth/LoginRateLimiter.h"
#include "server/auth/MemoryAccountService.h"
#include "shared/net/Crypto.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace va = vo::server::auth;
namespace vn = vo::net;
using CR = vn::CharCreateResult;

namespace {

// Calls an async service method and blocks until its callback fires (on the worker thread).
template <class R, class F>
R await(F&& call)
{
    std::promise<R> promise;
    auto future = promise.get_future();
    call([&promise](R result) { promise.set_value(std::move(result)); });
    REQUIRE(future.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    return future.get();
}

va::LoginOutcome login(va::MemoryAccountService& s, const std::string& user, const std::string& pw)
{
    return await<va::LoginOutcome>([&](auto done) { s.login(user, pw, done); });
}

va::CreateCharacterOutcome create(va::MemoryAccountService& s, va::AccountId acc, const std::string& name,
                                  std::uint8_t race = 1, std::uint8_t cls = 1, std::uint8_t face = 0,
                                  std::uint8_t hair = 0)
{
    return await<va::CreateCharacterOutcome>(
        [&](auto done) { s.createCharacter(acc, {name, race, cls, face, hair}, done); });
}

va::CharacterListOutcome list(va::MemoryAccountService& s, va::AccountId acc)
{
    return await<va::CharacterListOutcome>([&](auto done) { s.listCharacters(acc, done); });
}

va::DeleteCharacterOutcome remove(va::MemoryAccountService& s, va::AccountId acc, va::CharacterId id)
{
    return await<va::DeleteCharacterOutcome>([&](auto done) { s.deleteCharacter(acc, id, done); });
}

va::LoadCharacterOutcome load(va::MemoryAccountService& s, va::AccountId acc, va::CharacterId id)
{
    return await<va::LoadCharacterOutcome>([&](auto done) { s.loadCharacterForWorld(acc, id, done); });
}

} // namespace

TEST_CASE("MemoryAccountService login without auto-registration", "[server][auth]")
{
    REQUIRE(vn::initCrypto());
    va::MemoryAccountService svc({false});
    CHECK(login(svc, "nobody", "password1").result == vn::LoginResult::InvalidCredentials);
    CHECK(svc.accountCount() == 0);

    const auto id = svc.createAccount("Vedat_01", "password1");
    REQUIRE(id.has_value());
    CHECK_FALSE(svc.createAccount("vedat_01", "otherpass").has_value()); // case-insensitive duplicate
    CHECK_FALSE(svc.createAccount("bad name", "password1").has_value());
    CHECK_FALSE(svc.createAccount("gooduser", "short").has_value());

    auto ok = login(svc, "vedat_01", "password1"); // case-insensitive username
    CHECK(ok.result == vn::LoginResult::Ok);
    CHECK(ok.accountId == *id);
    CHECK(login(svc, "VEDAT_01", "password1").result == vn::LoginResult::Ok);

    const auto wrong = login(svc, "Vedat_01", "password2");
    CHECK(wrong.result == vn::LoginResult::InvalidCredentials);
    CHECK(wrong.accountId == 0);
    CHECK(login(svc, "abc", "password1").result == vn::LoginResult::InvalidCredentials);  // malformed name
    CHECK(login(svc, "Vedat_01", "pw").result == vn::LoginResult::InvalidCredentials);    // malformed password

    svc.setBanned(*id, true);
    CHECK(login(svc, "Vedat_01", "password1").result == vn::LoginResult::Banned);
    CHECK(login(svc, "Vedat_01", "nope-nope").result == vn::LoginResult::InvalidCredentials); // no ban leak
    svc.setBanned(*id, false);
    CHECK(login(svc, "Vedat_01", "password1").result == vn::LoginResult::Ok);
}

TEST_CASE("MemoryAccountService auto-registration", "[server][auth]")
{
    REQUIRE(vn::initCrypto());
    va::MemoryAccountService svc({true});
    const auto first = login(svc, "NewPlayer", "secret123");
    REQUIRE(first.result == vn::LoginResult::Ok);
    CHECK(first.accountId != 0);
    CHECK(svc.accountCount() == 1);

    const auto again = login(svc, "newplayer", "secret123");
    CHECK(again.result == vn::LoginResult::Ok);
    CHECK(again.accountId == first.accountId);
    CHECK(login(svc, "NEWPLAYER", "different1").result == vn::LoginResult::InvalidCredentials);

    CHECK(login(svc, "x", "secret123").result == vn::LoginResult::InvalidCredentials);
    CHECK(login(svc, "validname", "12345").result == vn::LoginResult::InvalidCredentials);
    CHECK(svc.accountCount() == 1);
}

TEST_CASE("MemoryAccountService character rules", "[server][auth]")
{
    REQUIRE(vn::initCrypto());
    va::MemoryAccountService svc({false});
    const auto a = *svc.createAccount("player_a", "password1");
    const auto b = *svc.createAccount("player_b", "password1");

    SECTION("validation order and result codes")
    {
        CHECK(create(svc, a, "1bad", 9, 9, 9, 9).result == CR::NameInvalid);
        CHECK(create(svc, a, "Al").result == CR::NameInvalid);
        CHECK(create(svc, a, "Ali", 0, 1).result == CR::InvalidRaceOrClass);
        CHECK(create(svc, a, "Ali", 3, 1).result == CR::InvalidRaceOrClass);
        CHECK(create(svc, a, "Ali", 1, 5, 9, 9).result == CR::InvalidRaceOrClass);
        CHECK(create(svc, a, "Ali", 2, 4, 4, 0).result == CR::InvalidAppearance);
        CHECK(create(svc, a, "Ali", 2, 4, 0, 4).result == CR::InvalidAppearance);
        CHECK(list(svc, a).characters.empty());
    }

    SECTION("create, list, three slots, soft delete keeps the name reserved")
    {
        const auto c1 = create(svc, a, "Irmak", 1, 1, 3, 3);
        REQUIRE(c1.result == CR::Ok);
        CHECK(c1.character.name == "Irmak");
        CHECK(c1.character.level == 1);
        CHECK(c1.character.position.zoneId == 1);
        CHECK(c1.character.face == 3);
        REQUIRE(create(svc, a, "Çağrı", 2, 2).result == CR::Ok);
        REQUIRE(create(svc, a, "İnci", 1, 3).result == CR::Ok);

        auto l = list(svc, a);
        REQUIRE(l.ok);
        CHECK(l.characters.size() == 3);
        CHECK(create(svc, a, "Dördüncü").result == CR::NoFreeSlot);
        CHECK(list(svc, b).characters.empty());

        // Turkish-aware uniqueness, across accounts.
        CHECK(create(svc, b, "IRMAK").result == CR::NameTaken);
        CHECK(create(svc, b, "ırmak").result == CR::NameTaken);
        CHECK(create(svc, b, "ÇAĞRI").result == CR::NameTaken);
        CHECK(create(svc, b, "inci").result == CR::NameTaken);
        CHECK(create(svc, b, "irmak").result == CR::Ok); // dotted i: a different name

        // Delete: other account cannot, owner can once.
        CHECK(remove(svc, b, c1.character.id).result == vn::CharDeleteResult::NotFound);
        CHECK(remove(svc, a, c1.character.id).result == vn::CharDeleteResult::Ok);
        CHECK(remove(svc, a, c1.character.id).result == vn::CharDeleteResult::NotFound);
        CHECK(remove(svc, a, 999999).result == vn::CharDeleteResult::NotFound);
        CHECK(list(svc, a).characters.size() == 2);

        // The slot is free again, but the deleted name stays reserved.
        CHECK(create(svc, a, "Irmak").result == CR::NameTaken);
        CHECK(create(svc, a, "IRMAK").result == CR::NameTaken);
        CHECK(create(svc, a, "Yeni").result == CR::Ok);
        CHECK(load(svc, a, c1.character.id).result == vn::EnterWorldResult::NotFound);
    }

    SECTION("load for world and save position")
    {
        const auto c = create(svc, a, "Gezgin");
        REQUIRE(c.result == CR::Ok);
        const auto loaded = load(svc, a, c.character.id);
        REQUIRE(loaded.result == vn::EnterWorldResult::Ok);
        CHECK(loaded.character.name == "Gezgin");
        CHECK(load(svc, b, c.character.id).result == vn::EnterWorldResult::NotFound);

        const va::CharacterPosition pos{1, 10.0f, 2.0f, 30.0f, 1.5f};
        CHECK(await<bool>([&](auto done) { svc.saveCharacterPosition(a, c.character.id, pos, done); }));
        CHECK_FALSE(await<bool>([&](auto done) { svc.saveCharacterPosition(b, c.character.id, pos, done); }));
        const auto after = load(svc, a, c.character.id);
        CHECK(after.character.position.x == 10.0f);
        CHECK(after.character.position.z == 30.0f);
        CHECK(after.character.position.yaw == 1.5f);
    }
}

TEST_CASE("MemoryAccountService completes on its worker thread and after stop()", "[server][auth]")
{
    REQUIRE(vn::initCrypto());
    va::MemoryAccountService svc({false});
    const auto testThread = std::this_thread::get_id();
    const auto cbThread = await<std::thread::id>(
        [&](auto done) { svc.listCharacters(1, [done](va::CharacterListOutcome) { done(std::this_thread::get_id()); }); });
    CHECK(cbThread != testThread);

    svc.stop();
    bool called = false;
    svc.login("someone", "password1", [&](va::LoginOutcome o) {
        called = true;
        CHECK(o.result == vn::LoginResult::ServerError);
    });
    CHECK(called); // synchronous ServerError once stopped
    svc.stop();    // idempotent
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
