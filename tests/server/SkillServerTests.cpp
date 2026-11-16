// Skills end to end on an in-process server (MemoryAccountService, test terrain, real sockets and the
// real 20 Hz world thread): what the session adds to the shared rules (docs/PROTOCOL_SKILLS.md §3-§5)
// - SKILL_STATE and SKILL_BAR on entry (after COINS, before CHAR_STATS), SKILL_LEARN_REQ checked
// against the live level and the committed ranks, stored before the answer, SKILL_STATE after it, the
// world using the new rank, the bar stored and validated, a failed write, reset-skills and set-level,
// a level-up adding a point, the 10/s flood cap, malformed and out-of-state requests.

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Combat.h"
#include "shared/game/Skills.h"
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
using R = vn::SkillLearnResult;

namespace {

vn::SkillLearnRes learn(TestClient& c, std::uint32_t seq, std::uint16_t skill)
{
    c.send(vn::SkillLearnReq{seq, skill});
    const auto res = c.waitFor<vn::SkillLearnRes>([&](const vn::SkillLearnRes& r) { return r.seq == seq; });
    REQUIRE(res);
    return *res;
}

vn::SkillBarSetRes setBar(TestClient& c, std::uint32_t seq, const vn::SkillBarSlots& slots)
{
    c.send(vn::SkillBarSetReq{seq, slots});
    const auto res = c.waitFor<vn::SkillBarSetRes>([&](const vn::SkillBarSetRes& r) { return r.seq == seq; });
    REQUIRE(res);
    return *res;
}

void leave(TestClient& c)
{
    c.pumpFor(1100ms); // stays below the 10/s cap across re-entries (the limiter is per connection)
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
}

} // namespace

TEST_CASE("Skill server: SKILL_STATE / SKILL_BAR on entry, learning stored before the answer", "[server][skills]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("sk_learn", "Yetenekçi");
    const auto charId = c.lastCharId();
    // A new level-1 warrior: no points, the free skill at rank 1, the default bar.
    CHECK(c.lastSkillState() == vn::SkillState{0, 0, {{1, 1}}});
    CHECK(c.lastSkillBar().slots == vn::SkillBarSlots{1, 0, 0, 0, 0, 0, 0, 0});
    CHECK(learn(c, 1, 1).result == R::NotEnoughPoints);
    CHECK(learn(c, 2, 2).result == R::UnknownSkill); // a rogue skill

    // Level 4 (set while offline): 3 points, all unspent (a character from before M12).
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 4));
    c.enter(charId);
    CHECK(c.lastSkillState() == vn::SkillState{3, 3, {{1, 1}}});
    CHECK(learn(c, 3, 5).result == R::TierLocked); // Yarma needs 2 points in Hücum
    const auto r1 = learn(c, 4, 1);
    CHECK(r1 == vn::SkillLearnRes{4, 1, R::Ok, 2, 2});
    // Stored before the answer; SKILL_STATE follows.
    CHECK(server.accounts().characterInfo(charId)->skillRanks[1] == 2);
    const auto st = c.waitFor<vn::SkillState>([](const vn::SkillState& s) { return s.unspent == 2; });
    REQUIRE(st);
    CHECK(st->skills == std::vector<vn::SkillRankEntry>{{1, 2}});
    CHECK(learn(c, 5, 1) == vn::SkillLearnRes{5, 1, R::Ok, 3, 1});
    CHECK(learn(c, 6, 1).result == R::MaxRank);
    CHECK(learn(c, 7, 5) == vn::SkillLearnRes{7, 5, R::Ok, 1, 0}); // the gate is open now
    CHECK(learn(c, 8, 8).result == R::NotEnoughPoints);
    // The world uses the new skill (Yarma needs someone around: NoTarget proves it is learned).
    c.pumpFor(200ms);
    c.send(vn::SkillReq{5, 0});
    const auto fail = c.waitFor<vn::ActionFail>([](const vn::ActionFail& f) { return f.skillId == 5; });
    REQUIRE(fail);
    CHECK((fail->reason == vn::ActionFailReason::NoTarget || fail->reason == vn::ActionFailReason::InSafeZone));
    c.send(vn::SkillReq{8, 0});
    const auto notLearned = c.waitFor<vn::ActionFail>([](const vn::ActionFail& f) { return f.skillId == 8; });
    REQUIRE(notLearned);
    CHECK(notLearned->reason == vn::ActionFailReason::NotLearned);

    // The bar: learned skills only (duplicates allowed), stored, kept across re-entering.
    CHECK(setBar(c, 9, {5, 1, 0, 0, 0, 0, 0, 5}) == vn::SkillBarSetRes{9, vn::SkillBarResult::Ok, {5, 1, 0, 0, 0, 0, 0, 5}});
    CHECK(server.accounts().characterInfo(charId)->skillBar == vg::SkillBar{5, 1, 0, 0, 0, 0, 0, 5});
    CHECK(setBar(c, 10, {8, 0, 0, 0, 0, 0, 0, 0}) ==
          vn::SkillBarSetRes{10, vn::SkillBarResult::NotLearned, {5, 1, 0, 0, 0, 0, 0, 5}});
    CHECK(setBar(c, 11, {2, 0, 0, 0, 0, 0, 0, 0}).result == vn::SkillBarResult::NotLearned); // another class
    CHECK(setBar(c, 12, {999, 0, 0, 0, 0, 0, 0, 0}).result == vn::SkillBarResult::NotLearned);
    leave(c);
    c.enter(charId);
    CHECK(c.lastSkillState() == vn::SkillState{3, 0, {{1, 3}, {5, 1}}});
    CHECK(c.lastSkillBar().slots == vn::SkillBarSlots{5, 1, 0, 0, 0, 0, 0, 5});

    // A failed write: ServerError, nothing changed; the next goes through.
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 5)); // 4 points, 3 spent (they fit)
    c.enter(charId);
    server.accounts().setFailSkillWritesForTesting(true);
    const auto failed = learn(c, 13, 5);
    CHECK(failed == vn::SkillLearnRes{13, 5, R::ServerError, 1, 1});
    CHECK(setBar(c, 14, {}).result == vn::SkillBarResult::ServerError);
    server.accounts().setFailSkillWritesForTesting(false);
    CHECK(server.accounts().characterInfo(charId)->skillRanks[5] == 1);
    CHECK(learn(c, 15, 5) == vn::SkillLearnRes{15, 5, R::Ok, 2, 0});

    // reset-skills (offline): free skill only, default bar, every point unspent.
    leave(c);
    REQUIRE(server.accounts().resetSkills(charId));
    c.enter(charId);
    CHECK(c.lastSkillState() == vn::SkillState{4, 4, {{1, 1}}});
    CHECK(c.lastSkillBar().slots == vn::SkillBarSlots{1, 0, 0, 0, 0, 0, 0, 0});
    // set-level lower: ranks that no longer fit are reset.
    CHECK(learn(c, 16, 8).result == R::Ok);
    CHECK(learn(c, 17, 8).result == R::Ok);
    CHECK(learn(c, 18, 8).result == R::Ok);
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 3)); // 2 points, 3 spent
    CHECK(server.accounts().characterInfo(charId)->skillRanks == vg::SkillRanks{});
}

TEST_CASE("Skill server: an admin reset while in the world is picked up by the next learn", "[server][skills]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("sk_reset", "Sıfırcı");
    const auto charId = c.lastCharId();
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 3));
    c.enter(charId);
    CHECK(learn(c, 1, 8).result == R::Ok);
    REQUIRE(server.accounts().resetSkills(charId)); // behind the session's back
    // The session thinks Kalkan Duvarı is rank 1; the stored truth says 0: the job decides on it.
    const auto r = learn(c, 2, 8);
    CHECK(r == vn::SkillLearnRes{2, 8, R::Ok, 1, 1});
    const auto st = c.waitFor<vn::SkillState>([](const vn::SkillState& s) { return s.unspent == 1; });
    REQUIRE(st);
    CHECK(st->skills == std::vector<vn::SkillRankEntry>{{1, 1}, {8, 1}});
}

TEST_CASE("Skill server: requests in a row are decided in order; a level-up adds a point", "[server][skills]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("sk_order", "Sıralı");
    const auto charId = c.lastCharId();
    leave(c);
    REQUIRE(server.accounts().setLevel(charId, 3));
    c.enter(charId);
    c.send(vn::SkillLearnReq{1, 1});
    c.send(vn::SkillLearnReq{2, 1});
    c.send(vn::SkillLearnReq{3, 1});
    c.send(vn::SkillBarSetReq{4, {1, 0, 0, 0, 0, 0, 0, 0}});
    std::vector<vn::SkillLearnRes> answers;
    for (int i = 0; i < 3; ++i)
    {
        const auto r = c.waitFor<vn::SkillLearnRes>([](const vn::SkillLearnRes&) { return true; });
        REQUIRE(r);
        answers.push_back(*r);
    }
    CHECK(answers[0] == vn::SkillLearnRes{1, 1, R::Ok, 2, 1});
    CHECK(answers[1] == vn::SkillLearnRes{2, 1, R::Ok, 3, 0});
    CHECK(answers[2] == vn::SkillLearnRes{3, 1, R::MaxRank, 3, 0});
    REQUIRE(c.waitFor<vn::SkillBarSetRes>([](const vn::SkillBarSetRes& r) { return r.seq == 4; }));
}

TEST_CASE("Skill server: flood cap, malformed and out-of-state skill requests", "[server][skills]")
{
    TestServer server({});
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("sk_state") == vn::LoginResult::Ok);
        c.send(vn::SkillLearnReq{1, 1});
        CHECK(c.expectError() == vn::ErrorCode::BadState);
        CHECK(c.waitClosed());
    }
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("sk_state2") == vn::LoginResult::Ok);
        c.send(vn::SkillBarSetReq{1, {}});
        CHECK(c.expectError() == vn::ErrorCode::BadState);
        CHECK(c.waitClosed());
    }
    {
        // 10 per second (learns and bar changes together) are answered, the 11th closes with Flood.
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("sk_flood", "Selli");
        for (std::uint32_t i = 1; i <= 11; ++i)
        {
            if (i % 2 == 0)
                c.send(vn::SkillLearnReq{i, 1});
            else
                c.send(vn::SkillBarSetReq{i, {1, 0, 0, 0, 0, 0, 0, 0}});
        }
        int answered = 0;
        bool flood = false;
        for (;;)
        {
            const auto m = c.next(5s);
            if (!m)
                break;
            if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::SkillLearnRes) ||
                m->opcode == static_cast<std::uint16_t>(vn::Opcode::SkillBarSetRes))
                ++answered;
            if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::Error))
            {
                vn::Error err;
                REQUIRE(vn::decodeMessage(m->payload, err));
                flood = err.errorCode == vn::ErrorCode::Flood;
                break;
            }
        }
        CHECK(answered <= 10);
        CHECK(flood);
        CHECK(c.waitClosed());
    }
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("sk_malformed", "Eğri");
        c.sendRawPayload(vn::Opcode::SkillBarSetReq, {1, 0, 0, 0, 1, 0});
        const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; });
        REQUIRE(err);
        CHECK(err->errorCode == vn::ErrorCode::ProtocolViolation);
        CHECK(c.waitClosed());
    }
}
