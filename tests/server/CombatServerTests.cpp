// Combat end to end on an in-process server (MemoryAccountService, test terrain, real sockets and
// the real 20 Hz world thread): what the session adds to the world's rules (docs/PROTOCOL_COMBAT.md)
// - the v3 ENTER_WORLD_RES fields and monster spawns, the combined 10/s flood cap, malformed and
// out-of-state requests, SELF_STATS after an equipment change, item moves refused while dead,
// RESPAWN_REQ timing, routing to a third client, progress stored on leave, set-level.

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Combat.h"
#include "shared/game/Monsters.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <thread>

namespace vn = vo::net;
namespace vg = vo::game;
using vo::tests::TestClient;
using vo::tests::TestServer;
using namespace std::chrono_literals;

namespace {

// A training dummy (5000 hp, harmless, never aggressive) 4 m from the Aydor spawn (100/100) and,
// optionally, an aggressive elite that kills a level-1 warrior in two hits.
std::shared_ptr<const vg::MonsterData> testMonsters(bool killer)
{
    std::string toml = R"(
[[monster]]
id = 2
name = "Talim Kuklası"
model = "ash_thrall"
level = 1
hp = 5000
attack = 0
defense = 4
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 0.0
leash_radius = 40.0
xp = 1
respawn_ms = 10000

[[monster]]
id = 5
name = "Kor Bekçisi"
model = "ember_warden"
level = 6
hp = 900
attack = 60
defense = 14
attack_range = 2.6
attack_interval_ms = 1000
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 12.0
leash_radius = 40.0
xp = 300
respawn_ms = 90000
elite = true

[[spawn]]
zone = 1
monster = 2
x = 104.0
z = 100.0
radius = 0.0
)";
    if (killer)
        toml += "\n[[spawn]]\nzone = 1\nmonster = 5\nx = 108.0\nz = 100.0\nradius = 0.0\n";
    auto data = std::make_shared<vg::MonsterData>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseMonsterData(toml, "test.toml", *data, error));
    return data;
}

TestServer::Options withMonsters(bool killer = false)
{
    TestServer::Options o;
    o.monsters = testMonsters(killer);
    o.world.statsInterval = 0s;
    return o;
}

// Waits until the client has seen the ENTITY_SPAWN of a monster with `templateId`.
std::uint32_t monsterId(TestClient& c, std::uint16_t templateId)
{
    const auto s = c.waitFor<vn::EntitySpawn>([&](const vn::EntitySpawn& s) {
        return s.entityType == vn::kEntityTypeMonster && s.templateId == templateId;
    });
    REQUIRE(s);
    return s->entityId;
}

// One running MOVE_REQ to (x, z) after the movement allowance filled up (legal up to ~7 m).
void runTo(TestClient& c, std::uint32_t& seq, float x, float z)
{
    std::this_thread::sleep_for(1100ms);
    c.send(vn::MoveReq{++seq, vn::kMoveFlagMoving | vn::kMoveFlagRunning, x, 0.0f, z, 0.0f});
    c.send(vn::MoveReq{++seq, vn::kMoveFlagRunning, x, 0.0f, z, 0.0f});
}

} // namespace

TEST_CASE("Combat server: ENTER_WORLD_RES carries the combat state, monsters spawn after INVENTORY",
          "[server][combat]")
{
    TestServer server({}, withMonsters());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("cmb_enter", "Girişçi");
    const vg::Stats s = vg::deriveStats(1, 1, {6, 4});
    CHECK(e.level == 1);
    CHECK(e.xp == 0);
    CHECK(e.xpToNext == 80);
    CHECK(e.hp == s.maxHp);
    CHECK(e.maxHp == s.maxHp);
    CHECK(e.mp == s.maxMp);
    CHECK(e.maxMp == s.maxMp);
    CHECK(e.attack == s.attack);
    CHECK(e.defense == s.defense);
    CHECK_FALSE(e.isDead);
    const auto spawn = c.expect<vn::EntitySpawn>(); // the first world packet after INVENTORY
    CHECK(spawn.entityType == vn::kEntityTypeMonster);
    CHECK(spawn.templateId == 2);
    CHECK(spawn.name == "Talim Kuklası");
    CHECK(spawn.hp == 5000);
    CHECK(spawn.maxHp == 5000);
}

TEST_CASE("Combat server: unequipping changes the stats at once and sends SELF_STATS", "[server][combat][items]")
{
    TestServer server({}, withMonsters());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("cmb_equip", "Kuşanıcı");
    REQUIRE(c.moveItem(1, 0, 1, 1, 5) == vn::ItemMoveResult::Ok); // the shield into the bag
    auto stats = c.waitFor<vn::SelfStats>([](const vn::SelfStats&) { return true; });
    REQUIRE(stats);
    CHECK(stats->defense == 3);
    CHECK(stats->attack == 18);
    REQUIRE(c.moveItem(2, 1, 0, 0, 0) == vn::ItemMoveResult::Ok); // the axe (two-handed) into MainHand
    stats = c.waitFor<vn::SelfStats>([](const vn::SelfStats&) { return true; });
    REQUIRE(stats);
    CHECK(stats->attack == 22);
    CHECK(stats->defense == 3);
    // A bag-only move changes nothing: no SELF_STATS.
    REQUIRE(c.moveItem(3, 1, 5, 1, 9) == vn::ItemMoveResult::Ok);
    CHECK_FALSE(c.waitFor<vn::SelfStats>([](const vn::SelfStats&) { return true; }, 600ms));
}

TEST_CASE("Combat server: 10 combat requests per second are fine, the 11th is Flood", "[server][combat][flood]")
{
    TestServer server({}, withMonsters());
    TestClient ok;
    REQUIRE(ok.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    ok.enterNew("cmb_rate", "Sakinci");
    for (int i = 0; i < 5; ++i)
    {
        ok.send(vn::TargetReq{0});
        ok.send(vn::StanceReq{vn::kStanceStand});
    }
    ok.send(vn::AttackStopReq{}); // not counted
    ok.send(vn::RespawnReq{});    // not counted: answered NotDead
    const auto res = ok.waitFor<vn::RespawnRes>([](const vn::RespawnRes&) { return true; });
    REQUIRE(res);
    CHECK(res->result == vn::RespawnResult::NotDead);
    CHECK_FALSE(ok.closed());

    TestClient flood;
    REQUIRE(flood.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    flood.enterNew("cmb_flood", "Selci");
    for (int i = 0; i < 6; ++i)
        flood.send(vn::AttackReq{0});
    for (int i = 0; i < 5; ++i)
        flood.send(vn::SkillReq{1, 0});
    const auto err = flood.waitFor<vn::Error>([](const vn::Error&) { return true; });
    REQUIRE(err);
    CHECK(err->errorCode == vn::ErrorCode::Flood);
    CHECK(flood.waitClosed());
}

TEST_CASE("Combat server: malformed requests are protocol violations, outside the world BadState",
          "[server][combat]")
{
    TestServer server({}, withMonsters());
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("cmb_bad1", "Bozukcu");
        c.sendRawPayload(vn::Opcode::StanceReq, {2}); // stance 2 is not defined
        CHECK(c.waitFor<vn::Error>([](const vn::Error&) { return true; })->errorCode == vn::ErrorCode::ProtocolViolation);
    }
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.enterNew("cmb_bad2", "Kısacı");
        c.sendRawPayload(vn::Opcode::SkillReq, {1, 0, 0}); // truncated
        CHECK(c.waitFor<vn::Error>([](const vn::Error&) { return true; })->errorCode == vn::ErrorCode::ProtocolViolation);
    }
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(c.login("cmb_bad3") == vn::LoginResult::Ok);
        c.send(vn::AttackReq{1});
        CHECK(c.expect<vn::Error>().errorCode == vn::ErrorCode::BadState);
        CHECK(c.waitClosed());
    }
}

TEST_CASE("Combat server: COMBAT_EVENT reaches a third player in view, not one out of view",
          "[server][combat][aoi]")
{
    TestServer server({}, withMonsters());
    TestClient a, b, distant;
    REQUIRE(a.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(b.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(distant.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto ea = a.enterNew("cmb_route_a", "Vurucu");
    b.enterNew("cmb_route_b", "Seyirci");
    distant.enterNew("cmb_route_c", "Uzakçı", 2); // Kargan spawn: 400/400
    const auto dummy = monsterId(a, 2);
    std::uint32_t seq = 0;
    runTo(a, seq, 102.0f, 100.0f);
    a.send(vn::AttackReq{dummy});
    const auto hit = a.waitFor<vn::CombatEvent>([&](const vn::CombatEvent& e) { return e.sourceId == ea.entityId; });
    REQUIRE(hit);
    CHECK(hit->targetId == dummy);
    CHECK(hit->targetHp == 5000 - hit->amount);
    const auto seen = b.waitFor<vn::CombatEvent>([&](const vn::CombatEvent& e) { return e.sourceId == ea.entityId; });
    REQUIRE(seen);
    CHECK(*seen == *hit);
    CHECK_FALSE(distant.waitFor<vn::CombatEvent>([](const vn::CombatEvent&) { return true; }, 1500ms));
    // In combat now: ENTITY_STATE with the in_combat bit to the observer.
    CHECK(b.waitFor<vn::EntityState>([&](const vn::EntityState& s) {
        return s.entityId == ea.entityId && (s.state & vn::kEntityStateInCombat) != 0;
    }));
}

TEST_CASE("Combat server: death - item moves refused, RESPAWN_REQ TooEarly then Ok after 5 s",
          "[server][combat][items]")
{
    TestServer server({}, withMonsters(true));
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("cmb_dead", "Ölümlü");
    // The elite 8 m away notices the newcomer and kills it.
    const auto death = c.waitFor<vn::EntityDeath>([&](const vn::EntityDeath& d) { return d.entityId == e.entityId; },
                                                   10s);
    REQUIRE(death);
    const auto diedAt = std::chrono::steady_clock::now();
    CHECK(c.moveItem(1, 0, 1, 1, 5) == vn::ItemMoveResult::SlotBlocked);
    c.send(vn::RespawnReq{});
    auto res = c.waitFor<vn::RespawnRes>([](const vn::RespawnRes&) { return true; });
    REQUIRE(res);
    CHECK(res->result == vn::RespawnResult::TooEarly);
    std::this_thread::sleep_until(diedAt + 5200ms);
    c.send(vn::RespawnReq{});
    res = c.waitFor<vn::RespawnRes>([](const vn::RespawnRes&) { return true; });
    REQUIRE(res);
    CHECK(res->result == vn::RespawnResult::Ok);
    CHECK(std::hypot(res->x - 100.0f, res->z - 100.0f) <= 2.1f);
    CHECK(res->hp == 45);
    // Alive again: item moves work (before the elite comes back for it).
    CHECK(c.moveItem(2, 0, 1, 1, 5) == vn::ItemMoveResult::Ok);
}

TEST_CASE("Combat server: progress is stored on leave; set-level applies on the next entry", "[server][combat]")
{
    TestServer server({}, withMonsters());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("cmb_store", "Saklayan");
    const auto charId = c.lastCharId();
    const auto dummy = monsterId(c, 2);
    std::uint32_t seq = 0;
    runTo(c, seq, 102.0f, 100.0f);
    c.send(vn::SkillReq{vg::kSkillHeavyBlow, dummy});
    const auto hit = c.waitFor<vn::CombatEvent>([&](const vn::CombatEvent& ev) { return ev.sourceId == e.entityId; });
    REQUIRE(hit);
    CHECK(hit->skillId == vg::kSkillHeavyBlow);
    const auto self = c.waitFor<vn::SelfStats>([](const vn::SelfStats& s) { return s.mp == 34 - 12; });
    REQUIRE(self);
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    const auto stored = server.accounts().characterInfo(charId);
    REQUIRE(stored);
    REQUIRE(stored->mp);
    CHECK(*stored->mp <= 34u);
    CHECK(*stored->mp >= 22u);
    CHECK(stored->hp.value_or(0) > 0);
    CHECK(stored->level == 1);

    REQUIRE(server.accounts().setLevel(charId, 99));
    const auto again = c.enter(charId);
    const vg::Stats s10 = vg::deriveStats(1, vg::kLevelCap, {6, 4});
    CHECK(again.level == vg::kLevelCap);
    CHECK(again.xp == 0);
    CHECK(again.xpToNext == 0);
    CHECK(again.hp == s10.maxHp);
    CHECK(again.mp == s10.maxMp);
    CHECK(again.attack == s10.attack);
    // The level decides item requirements now: the level-3 sword fits (given while offline).
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
    REQUIRE(server.accounts().giveItem(charId, 7));
    c.enter(charId);
    std::uint8_t swordSlot = 255;
    for (const auto& it : c.lastInventory().items)
        if (it.templateId == 7)
            swordSlot = it.slot;
    REQUIRE(swordSlot != 255);
    CHECK(c.moveItem(9, 1, swordSlot, 0, 0) == vn::ItemMoveResult::Ok);
}
