// Stats end to end on an in-process server (MemoryAccountService, test terrain, real sockets and the
// real 20 Hz world thread): what the session adds to the shared rules (docs/PROTOCOL_STATS.md) -
// CHAR_STATS on entry, STAT_ALLOCATE_REQ checked against the live level and the committed allocation,
// stored before the answer, CHAR_STATS after it, kept across re-entering, a failed write, reset-stats,
// the 5/s flood cap, malformed and out-of-state requests.

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Combat.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace vn = vo::net;
namespace vg = vo::game;
using vo::tests::TestClient;
using vo::tests::TestServer;
using namespace std::chrono_literals;
using R = vn::StatAllocateResult;

namespace {

vn::StatAllocateRes allocate(TestClient& c, std::uint32_t seq, const vg::Attributes& points)
{
    c.send(vn::StatAllocateReq{seq, points});
    const auto res = c.waitFor<vn::StatAllocateRes>([&](const vn::StatAllocateRes& r) { return r.seq == seq; });
    REQUIRE(res);
    return *res;
}

void leave(TestClient& c)
{
    c.pumpFor(1100ms); // keeps every test below the 5/s STAT_ALLOCATE_REQ cap (the limiter is per connection)
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
}

} // namespace

TEST_CASE("Stats server: CHAR_STATS on entry, allocation stored before the answer, CHAR_STATS after it",
          "[server][stats]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("st_alloc", "Puancı");
    const auto charId = c.lastCharId();
    // Level 1: the warrior's start, nothing to spend.
    const vn::CharStats first = c.lastCharStats();
    CHECK(first.level == 1);
    CHECK(first.unspent == 0);
    CHECK(first.attributes[0].base == 12);
    CHECK(first.attributes[2].base == 11);
    CHECK(first.maxHp == e.maxHp);
    CHECK(first.attack == e.attack);
    CHECK(allocate(c, 1, {}).result == R::InvalidAmount);
    const auto none = allocate(c, 2, {1, 0, 0, 0, 0});
    CHECK(none.result == R::NotEnoughPoints);
    CHECK(none.unspent == 0);

    // Level 4 (as set while offline): 9 points.
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 4));
    c.enter(charId);
    CHECK(c.lastCharStats().level == 4);
    CHECK(c.lastCharStats().unspent == 9);
    CHECK(allocate(c, 3, {10, 0, 0, 0, 0}).result == R::NotEnoughPoints);
    const auto ok = allocate(c, 4, {5, 0, 4, 0, 0});
    CHECK(ok.result == R::Ok);
    CHECK(ok.unspent == 0);
    // Stored before the answer.
    const auto stored = server.accounts().characterInfo(charId);
    REQUIRE(stored);
    CHECK(stored->statPoints == vg::Attributes{5, 0, 4, 0, 0});
    // CHAR_STATS with the new values follows (and SELF_STATS with the new maximum).
    const auto stats = c.waitFor<vn::CharStats>([](const vn::CharStats& s) { return s.attributes[0].base == 17; });
    REQUIRE(stats);
    const vg::Stats expected = vg::deriveStats(1, 4, {5, 0, 4, 0, 0}, {6, 4});
    CHECK(stats->unspent == 0);
    CHECK(stats->attributes[2].base == 15);
    CHECK(stats->maxHp == expected.maxHp);
    CHECK(stats->attack == expected.attack);
    CHECK(stats->defense == expected.defense);
    CHECK(allocate(c, 5, {0, 1, 0, 0, 0}).result == R::NotEnoughPoints);

    // Kept after leaving and entering again.
    leave(c);
    const auto again = c.enter(charId);
    CHECK(c.lastCharStats().attributes[0].base == 17);
    CHECK(c.lastCharStats().unspent == 0);
    CHECK(again.maxHp == expected.maxHp);
    CHECK(again.attack == expected.attack);

    // A failed write changes nothing and is ServerError; the next one goes through.
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 5)); // 12 points, 9 spent (they fit)
    CHECK(server.accounts().characterInfo(charId)->statPoints == vg::Attributes{5, 0, 4, 0, 0});
    c.enter(charId);
    CHECK(c.lastCharStats().unspent == 3);
    server.accounts().setFailStatWritesForTesting(true);
    const auto failed = allocate(c, 6, {0, 0, 0, 0, 3});
    CHECK(failed.result == R::ServerError);
    CHECK(failed.unspent == 3);
    server.accounts().setFailStatWritesForTesting(false);
    CHECK(server.accounts().characterInfo(charId)->statPoints == vg::Attributes{5, 0, 4, 0, 0});
    const auto retried = allocate(c, 7, {0, 0, 0, 0, 3});
    CHECK(retried.result == R::Ok);
    CHECK(retried.unspent == 0);

    // vo_admin reset-stats (offline): every point unspent again on the next entry.
    leave(c);
    REQUIRE(server.accounts().resetStats(charId));
    c.enter(charId);
    CHECK(c.lastCharStats().unspent == 12);
    CHECK(c.lastCharStats().attributes[0].base == 12);
    // set-level to a lower level resets an allocation that no longer fits.
    CHECK(allocate(c, 8, {12, 0, 0, 0, 0}).result == R::Ok);
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 3));
    CHECK(server.accounts().characterInfo(charId)->statPoints == vg::Attributes{});
}

TEST_CASE("Stats server: a level-up in the world is spendable at once", "[server][stats]")
{
    // The session follows the live level from the world's batches; here the stored level 2 counts
    // as soon as the character enters.
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("st_level", "Kademeci");
    const auto charId = c.lastCharId();
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 2));
    c.enter(charId);
    // Several requests in a row are decided in order against the committed allocation.
    c.send(vn::StatAllocateReq{1, {1, 0, 0, 0, 0}});
    c.send(vn::StatAllocateReq{2, {1, 0, 0, 0, 0}});
    c.send(vn::StatAllocateReq{3, {1, 0, 0, 0, 0}});
    c.send(vn::StatAllocateReq{4, {1, 0, 0, 0, 0}});
    std::vector<vn::StatAllocateRes> answers;
    for (int i = 0; i < 4; ++i)
    {
        const auto r = c.waitFor<vn::StatAllocateRes>([](const vn::StatAllocateRes&) { return true; });
        REQUIRE(r);
        answers.push_back(*r);
    }
    CHECK(answers[0] == vn::StatAllocateRes{1, R::Ok, 2});
    CHECK(answers[1] == vn::StatAllocateRes{2, R::Ok, 1});
    CHECK(answers[2] == vn::StatAllocateRes{3, R::Ok, 0});
    CHECK(answers[3] == vn::StatAllocateRes{4, R::NotEnoughPoints, 0});
    CHECK(server.accounts().characterInfo(charId)->statPoints == vg::Attributes{3, 0, 0, 0, 0});
}

TEST_CASE("Stats server: flood cap, malformed and out-of-state STAT_ALLOCATE_REQ", "[server][stats]")
{
    TestServer server({});
    {
        // Not in the world: BadState.
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("st_state") == vn::LoginResult::Ok);
        c.send(vn::StatAllocateReq{1, {}});
        CHECK(c.expectError() == vn::ErrorCode::BadState);
        CHECK(c.waitClosed());
    }
    {
        // 5 per second are answered, the 6th closes with ERROR(Flood).
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("st_flood", "Selci");
        for (std::uint32_t i = 1; i <= 6; ++i)
            c.send(vn::StatAllocateReq{i, {}});
        int answered = 0;
        bool flood = false;
        for (;;)
        {
            const auto m = c.next(5s);
            if (!m)
                break;
            if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::StatAllocateRes))
                ++answered;
            if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::Error))
            {
                vn::Error err;
                REQUIRE(vn::decodeMessage(m->payload, err));
                flood = err.errorCode == vn::ErrorCode::Flood;
                break;
            }
        }
        CHECK(answered == 5);
        CHECK(flood);
        CHECK(c.waitClosed());
    }
    {
        // A truncated payload is a protocol violation.
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("st_malformed", "Bozukçu");
        c.sendRawPayload(vn::Opcode::StatAllocateReq, {1, 0, 0, 0, 1, 0});
        const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; });
        REQUIRE(err);
        CHECK(err->errorCode == vn::ErrorCode::ProtocolViolation);
        CHECK(c.waitClosed());
    }
}
