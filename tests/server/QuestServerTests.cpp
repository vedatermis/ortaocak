// Quests end to end on an in-process server (MemoryAccountService, test terrain, real sockets and the real
// world thread; M17, docs/PROTOCOL_QUESTS.md): the world's NPC check (in view, range) and the session's
// (giver / turn-in, prerequisites, level, race, the limit, readiness, the bag) with their refusal codes,
// QUEST_UPDATE before QUEST_RES, a hand-in stored in one write (quest row, units taken, rewards, Akçe,
// experience) before its answer, kill credit from the world, the counters stored on leave and back on the
// next entry, abandon, a failed write changing nothing and the flood cap.
//
// The tests install their own quest catalog (game::installQuestCatalog is process-wide; nothing else in
// vo_tests reads the installed one).

#include "server/auth/MemoryAccountService.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/game/Npcs.h"
#include "shared/game/Quests.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace vn = vo::net;
namespace vg = vo::game;
using vo::tests::TestClient;
using vo::tests::TestServer;
using namespace std::chrono_literals;
using QR = vn::QuestResult;
using QA = vn::QuestAction;

namespace {

constexpr vg::ItemTemplateId kAsh = 9, kHam = 166, kOldLegs = 72;

// Two one-hit thralls next to spawn_aydor (100, 100), back 3 s after a kill; each drops 3 Kül Tozu.
std::shared_ptr<const vg::MonsterData> thralls()
{
    auto data = std::make_shared<vg::MonsterData>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseMonsterData(R"(
[[monster]]
id = 1
name = "Kül Çömezi"
model = "ash_thrall"
level = 1
hp = 1
attack = 0
defense = 0
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 0.0
leash_radius = 40.0
xp = 25
respawn_ms = 3000

[[spawn]]
zone = 1
monster = 1
x = 104.0
z = 100.0
radius = 0.0

[[spawn]]
zone = 1
monster = 1
x = 100.0
z = 104.0
radius = 0.0
)",
                                 "test.toml", *data, error));
    return data;
}

std::shared_ptr<const vg::LootTables> tables(const vg::MonsterData& m)
{
    auto t = std::make_shared<vg::LootTables>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseLootTables("[[table]]\nmonsters = [1]\n[[table.drop]]\nitem = 9\nchance = 1.0\ncount = [3, 3]\n",
                                "loot.toml", *vo::tests::testItemCatalog(), &m, *t, error));
    return t;
}

// Oruç (10) 4.2 m south of the spawn, Selvi (11) 4.2 m west, Toygar (12) 14 m south (in view, out of range).
std::shared_ptr<const vg::NpcCatalog> testNpcs()
{
    auto c = std::make_shared<vg::NpcCatalog>();
    std::string error;
    const auto npc = [](int id, const char* name, float x, float z) {
        return std::format("[[npc]]\nid = {}\nname = \"{}\"\ntitle = \"T\"\ngreeting = \"G\"\nmodel = \"npc_quest_{}\"\nzone = 1\n"
                           "x = {}\nz = {}\nyaw = 0.0\nkind = \"quest\"\n\n",
                           id, name, id, x, z);
    };
    const std::string text = npc(10, "Bilge Oruç", 100.0f, 95.8f) + npc(11, "Avcı Selvi", 95.8f, 100.0f) +
                             npc(12, "Karakol Başı Toygar", 100.0f, 86.0f);
    INFO(error);
    REQUIRE(vg::parseNpcCatalog(text, "npcs.toml", vo::tests::testItemCatalog().get(), *c, error));
    return c;
}

void installTestQuests()
{
    auto c = std::make_shared<vg::QuestCatalog>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseQuestCatalog(R"(
[rules]
max_active = 3

[[quest]]
id = 1
name = "Konuk"
giver = 10
turn_in = 11
story = "s"
done_text = "d"
[[quest.objective]]
type = "talk"
npc = 11
[quest.reward]
xp = 30
coins = 10

[[quest]]
id = 2
name = "Iki Comez"
giver = 10
after = 1
story = "s"
done_text = "d"
[[quest.objective]]
type = "kill"
monsters = [1]
count = 2
[quest.reward]
xp = 100
class_items = { warrior = 72, rogue = 77, mage = 82, priest = 72 }

[[quest]]
id = 3
name = "Toz"
giver = 11
story = "s"
done_text = "d"
[[quest.objective]]
type = "collect"
item = 9
count = 3
[quest.reward]
coins = 5
items = [[166, 2]]

[[quest]]
id = 4
name = "Sonra"
giver = 10
level = 5
story = "s"
done_text = "d"
[[quest.objective]]
type = "kill"
monsters = [1]
count = 1
[quest.reward]
xp = 5

[[quest]]
id = 5
name = "Kargan"
giver = 10
race = "kargan"
story = "s"
done_text = "d"
[[quest.objective]]
type = "kill"
monsters = [1]
count = 1
[quest.reward]
xp = 5

[[quest]]
id = 6
name = "Uzak"
giver = 12
story = "s"
done_text = "d"
[[quest.objective]]
type = "kill"
monsters = [1]
count = 1
[quest.reward]
xp = 5

[[quest]]
id = 7
name = "Nobet"
giver = 10
repeatable = true
cooldown_s = 60
story = "s"
done_text = "d"
[[quest.objective]]
type = "kill"
monsters = [1]
count = 1
[quest.reward]
coins = 3

[[quest]]
id = 8
name = "Sayac"
giver = 10
story = "s"
done_text = "d"
[[quest.objective]]
type = "kill"
monsters = [1]
count = 2
[quest.reward]
xp = 7
)",
                                  "quests.toml", vo::tests::testItemCatalog().get(), nullptr, nullptr, *c, error));
    vg::installQuestCatalog(std::move(c));
}

TestServer::Options options()
{
    installTestQuests();
    TestServer::Options o;
    o.npcs = testNpcs();
    o.monsters = thralls();
    o.loot = tables(*o.monsters);
    o.world.statsInterval = 0s;
    o.world.zone.spawnSpread = 0.0f;
    return o;
}

struct Npcs
{
    std::uint32_t oruc = 0, selvi = 0, toygar = 0;
    std::uint32_t east = 0, north = 0; // the thralls (0 = not seen: dead)
};

// The NPC and monster spawns that follow an entry (in whatever order they come).
Npcs npcsOf(TestClient& c, bool withThralls = false)
{
    Npcs n;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((n.oruc == 0 || n.selvi == 0 || n.toygar == 0 || (withThralls && (n.east == 0 || n.north == 0))) &&
           std::chrono::steady_clock::now() < deadline)
    {
        const auto s = c.waitFor<vn::EntitySpawn>([](const vn::EntitySpawn&) { return true; }, 2s);
        if (!s)
            break;
        if (s->entityType == vn::kEntityTypeNpc)
            (s->templateId == 10 ? n.oruc : s->templateId == 11 ? n.selvi : n.toygar) = s->entityId;
        else if (s->entityType == vn::kEntityTypeMonster)
            (s->x == 104.0f ? n.east : n.north) = s->entityId;
    }
    REQUIRE(n.oruc != 0);
    REQUIRE(n.selvi != 0);
    REQUIRE(n.toygar != 0);
    if (withThralls)
    {
        REQUIRE(n.east != 0);
        REQUIRE(n.north != 0);
    }
    return n;
}

struct Answer
{
    vn::QuestRes res;
    std::vector<vn::QuestUpdate> updates;
    std::vector<vn::ItemUpdate> items;
    std::vector<vn::Coins> coins;
    std::vector<vn::XpGain> xp;
    bool resBeforeUpdate = false;
};

// Sends one quest request (paced below the 5/s cap) and collects up to its QUEST_RES, then a little more
// (the XP_GAIN of a hand-in comes from the world after it).
template <class Req>
Answer request(TestClient& c, const Req& req, std::chrono::milliseconds after = 0ms)
{
    c.pumpFor(220ms);
    c.send(req);
    Answer a;
    const auto take = [&](const vo::tests::Msg& m) {
        if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::QuestUpdate))
        {
            vn::QuestUpdate u;
            REQUIRE(vn::decodeMessage(m.payload, u));
            a.updates.push_back(u);
        }
        else if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::ItemUpdate))
        {
            vn::ItemUpdate u;
            REQUIRE(vn::decodeMessage(m.payload, u));
            a.items.push_back(u);
        }
        else if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::Coins))
        {
            vn::Coins x;
            REQUIRE(vn::decodeMessage(m.payload, x));
            a.coins.push_back(x);
        }
        else if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::XpGain))
        {
            vn::XpGain x;
            REQUIRE(vn::decodeMessage(m.payload, x));
            a.xp.push_back(x);
        }
    };
    for (;;)
    {
        auto m = c.next();
        REQUIRE(m);
        if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::QuestRes))
        {
            vn::QuestRes r;
            REQUIRE(vn::decodeMessage(m->payload, r));
            if (r.seq == req.seq)
            {
                a.res = r;
                break;
            }
            continue;
        }
        take(*m);
    }
    const auto deadline = std::chrono::steady_clock::now() + after;
    while (std::chrono::steady_clock::now() < deadline)
        if (auto m = c.next(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())))
        {
            if (m->opcode == static_cast<std::uint16_t>(vn::Opcode::QuestUpdate))
                a.resBeforeUpdate = true;
            take(*m);
        }
    return a;
}

// Kills the thrall at (mx, mz) from 1.5 m away (one hit) and returns the QUEST_UPDATEs that follow it.
std::vector<vn::QuestUpdate> killThrall(TestClient& c, std::uint32_t& seq, std::uint32_t monsterId, float mx, float mz,
                                        bool expectUpdate = true)
{
    std::this_thread::sleep_for(1100ms); // a full movement allowance
    const float px = mx == 100.0f ? 100.0f : mx - 1.5f;
    const float pz = mz == 100.0f ? 100.0f : mz - 1.5f;
    c.send(vn::MoveReq{++seq, vn::kMoveFlagMoving | vn::kMoveFlagRunning, px, 0.0f, pz, 0.0f});
    c.send(vn::MoveReq{++seq, 0, px, 0.0f, pz, 0.0f});
    c.send(vn::AttackReq{monsterId});
    REQUIRE(c.waitFor<vn::EntityDeath>([&](const vn::EntityDeath& d) { return d.entityId == monsterId; }));
    std::vector<vn::QuestUpdate> out;
    if (expectUpdate)
    {
        const auto u = c.waitFor<vn::QuestUpdate>([](const vn::QuestUpdate&) { return true; });
        REQUIRE(u);
        out.push_back(*u);
    }
    // Back to the spawn (in talk range of Oruç and Selvi again).
    std::this_thread::sleep_for(1100ms);
    c.send(vn::MoveReq{++seq, vn::kMoveFlagMoving | vn::kMoveFlagRunning, 100.0f, 0.0f, 100.0f, 0.0f});
    c.send(vn::MoveReq{++seq, 0, 100.0f, 0.0f, 100.0f, 0.0f});
    c.pumpFor(150ms);
    return out;
}

const vg::QuestRecord* storedQuest(const std::vector<vg::QuestRecord>& rows, vg::QuestId id)
{
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const vg::QuestRecord& r) { return r.quest == id; });
    return it == rows.end() ? nullptr : &*it;
}

std::uint32_t storedUnits(const std::vector<vg::StoredItem>& items, std::uint16_t templateId)
{
    std::uint32_t n = 0;
    for (const auto& i : items)
        if (i.templateId == templateId)
            n += i.quantity;
    return n;
}

void leave(TestClient& c)
{
    c.send(vn::LeaveWorldReq{});
    REQUIRE(c.waitFor<vn::LeaveWorldRes>([](const vn::LeaveWorldRes&) { return true; }));
}

} // namespace

TEST_CASE("Quest server: accept refusals, a talk quest, a kill quest with kill credit, rewards stored before the answer",
          "[server][quests]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("quest_flow", "Görevci"); // an Aydor warrior at level 1
    const auto charId = c.lastCharId();
    CHECK(c.lastQuestList().quests.empty());
    const Npcs npc = npcsOf(c, true);
    const Npcs& th = npc;
    std::uint32_t seq = 0;
    const auto accept = [&](std::uint32_t npcId, std::uint16_t q) { return request(c, vn::QuestAcceptReq{++seq, npcId, q}); };

    // Refusals (nothing stored).
    CHECK(accept(npc.oruc, 999).res.result == QR::UnknownQuest);
    CHECK(accept(npc.oruc, 2).res.result == QR::NeedsQuest);
    CHECK(accept(npc.selvi, 1).res.result == QR::WrongNpc); // Selvi does not give it
    CHECK(accept(npc.toygar, 6).res.result == QR::OutOfRange);
    CHECK(accept(npc.oruc, 4).res.result == QR::LevelTooLow);
    CHECK(accept(npc.oruc, 5).res.result == QR::WrongRace);
    CHECK(accept(0x12345, 1).res.result == QR::WrongNpc); // no such NPC in view
    CHECK(server.accounts().questsOf(charId).empty());

    // The talk quest: QUEST_UPDATE (active), then QUEST_RES; stored before the answer.
    const Answer a1 = accept(npc.oruc, 1);
    CHECK(a1.res == vn::QuestRes{a1.res.seq, QA::Accept, 1, QR::Ok, 0, 0});
    REQUIRE(a1.updates.size() == 1);
    CHECK(a1.updates[0].quest == vn::QuestEntry{1, vn::QuestState::Active, {0, 0, 0}, 0});
    REQUIRE(storedQuest(server.accounts().questsOf(charId), 1));
    CHECK(storedQuest(server.accounts().questsOf(charId), 1)->state == vn::QuestState::Active);
    CHECK(accept(npc.oruc, 1).res.result == QR::AlreadyActive);
    // Handed in to Selvi, not to Oruç.
    CHECK(request(c, vn::QuestHandInReq{++seq, npc.oruc, 1, vn::kQuestNoChoice}).res.result == QR::WrongNpc);
    CHECK(request(c, vn::QuestHandInReq{++seq, npc.selvi, 1, 0}).res.result == QR::BadChoice); // no choice offered
    const Answer h1 = request(c, vn::QuestHandInReq{++seq, npc.selvi, 1, vn::kQuestNoChoice}, 600ms);
    CHECK(h1.res == vn::QuestRes{h1.res.seq, QA::HandIn, 1, QR::Ok, 30, 10});
    REQUIRE(h1.coins.size() == 1);
    CHECK(h1.coins[0] == vn::Coins{10, 10});
    REQUIRE(h1.updates.size() == 1);
    CHECK(h1.updates[0].quest.state == vn::QuestState::Done);
    REQUIRE(h1.xp.size() == 1); // from the world, after the answer
    CHECK(h1.xp[0].amount == 30);
    {
        const auto rows = server.accounts().questsOf(charId);
        REQUIRE(storedQuest(rows, 1));
        CHECK(storedQuest(rows, 1)->state == vn::QuestState::Done);
        CHECK(storedQuest(rows, 1)->completions == 1);
        CHECK(storedQuest(rows, 1)->completedAt > 0);
        const auto info = server.accounts().characterInfo(charId);
        CHECK(info->coins == 10);
        CHECK(info->xp == 30); // in the same write as the quest row
    }
    CHECK(accept(npc.oruc, 1).res.result == QR::AlreadyDone);

    // The kill quest: the thralls' deaths count (QUEST_UPDATE with the counter), the hand-in waits for both.
    REQUIRE(accept(npc.oruc, 2).res.result == QR::Ok);
    CHECK(request(c, vn::QuestHandInReq{++seq, npc.oruc, 2, vn::kQuestNoChoice}).res.result == QR::NotReady);
    const auto k1 = killThrall(c, seq, th.east, 104.0f, 100.0f);
    REQUIRE(k1.size() == 1);
    CHECK(k1[0].quest == vn::QuestEntry{2, vn::QuestState::Active, {1, 0, 0}, 0});
    const auto k2 = killThrall(c, seq, th.north, 100.0f, 104.0f);
    REQUIRE(k2.size() == 1);
    CHECK(k2[0].quest.progress[0] == 2);
    const Answer h2 = request(c, vn::QuestHandInReq{++seq, npc.oruc, 2, vn::kQuestNoChoice}, 600ms);
    CHECK(h2.res.result == QR::Ok);
    CHECK(h2.res.xp == 100);
    // The warrior's class item (Eski Dizlik) in the first free bag slot.
    bool legs = false;
    for (const auto& u : h2.items)
        for (const auto& e : u.items)
            legs = legs || e.templateId == kOldLegs;
    CHECK(legs);
    CHECK(storedUnits(server.accounts().itemsOf(charId), kOldLegs) == 1);
    CHECK(storedQuest(server.accounts().questsOf(charId), 2)->state == vn::QuestState::Done);

    // Abandon: NotActive for a done quest; a fresh accept goes away again (no row left).
    CHECK(request(c, vn::QuestAbandonReq{++seq, 2}).res.result == QR::NotActive);
    REQUIRE(accept(npc.oruc, 8).res.result == QR::Ok);
    const Answer ab = request(c, vn::QuestAbandonReq{++seq, 8});
    CHECK(ab.res == vn::QuestRes{ab.res.seq, QA::Abandon, 8, QR::Ok, 0, 0});
    REQUIRE(ab.updates.size() == 1);
    CHECK(ab.updates[0].quest == vn::QuestEntry{8, vn::QuestState::None, {}, 0});
    CHECK_FALSE(storedQuest(server.accounts().questsOf(charId), 8));
}

TEST_CASE("Quest server: counters stored on leave and listed on entry; the active limit; cooldown of a repeatable",
          "[server][quests]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("quest_relog", "Sayıcı");
    const auto charId = c.lastCharId();
    Npcs npc = npcsOf(c, true);
    std::uint32_t seq = 0;
    REQUIRE(request(c, vn::QuestAcceptReq{++seq, npc.oruc, 8}).res.result == QR::Ok);
    REQUIRE(killThrall(c, seq, npc.east, 104.0f, 100.0f).size() == 1);
    // Live only until the leave (the save cadence is 60 s).
    CHECK(storedQuest(server.accounts().questsOf(charId), 8)->progress[0] == 0);
    leave(c);
    CHECK(storedQuest(server.accounts().questsOf(charId), 8)->progress[0] == 1);
    c.enter(charId);
    const auto& list = c.lastQuestList().quests;
    REQUIRE(list.size() == 1);
    CHECK(list[0] == vn::QuestEntry{8, vn::QuestState::Active, {1, 0, 0}, 0});

    // The limit (3 in the test catalog): 8, 3 and 7 active, then 1 is refused.
    npc = npcsOf(c); // (the east thrall is still dead)
    REQUIRE(request(c, vn::QuestAcceptReq{++seq, npc.selvi, 3}).res.result == QR::Ok);
    REQUIRE(request(c, vn::QuestAcceptReq{++seq, npc.oruc, 7}).res.result == QR::Ok);
    CHECK(request(c, vn::QuestAcceptReq{++seq, npc.oruc, 1}).res.result == QR::TooManyActive);
    // The repeatable one: kill, hand in, then it waits out its cooldown (60 s).
    // The thrall killed above is back (3 s).
    const auto back = c.waitFor<vn::EntitySpawn>(
        [](const vn::EntitySpawn& e) { return e.entityType == vn::kEntityTypeMonster && e.x == 104.0f; }, 6s);
    REQUIRE(back);
    const auto k = killThrall(c, seq, back->entityId, 104.0f, 100.0f, false);
    (void)k;
    c.pumpFor(300ms);
    const Answer h = request(c, vn::QuestHandInReq{++seq, npc.oruc, 7, vn::kQuestNoChoice});
    REQUIRE(h.res.result == QR::Ok);
    REQUIRE_FALSE(h.updates.empty());
    CHECK(h.updates.back().quest.state == vn::QuestState::Done);
    CHECK(h.updates.back().quest.cooldownS > 55);
    CHECK(h.updates.back().quest.cooldownS <= 60);
    CHECK(request(c, vn::QuestAcceptReq{++seq, npc.oruc, 7}).res.result == QR::OnCooldown);
    // As if the hand-in had been long ago (vo_admin set-quest): it can be taken again.
    vg::QuestRecord past{7, vn::QuestState::Done, {}, 1, 1000};
    REQUIRE(server.accounts().setQuest(charId, past, vn::QuestState::Done));
    leave(c);
    c.enter(charId);
    npc = npcsOf(c);
    CHECK(request(c, vn::QuestAcceptReq{++seq, npc.oruc, 7}).res.result == QR::Ok);
    // Abandoning a repeat keeps its hand-in record (Done again, completions 1).
    REQUIRE(request(c, vn::QuestAbandonReq{++seq, 7}).res.result == QR::Ok);
    const auto rows = server.accounts().questsOf(charId);
    const auto* row = storedQuest(rows, 7);
    REQUIRE(row);
    CHECK(row->state == vn::QuestState::Done);
    CHECK(row->completions == 1);
}

TEST_CASE("Quest server: collect hand-in takes the units; a full bag and a failed write change nothing", "[server][quests]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("quest_bag", "Torbacı");
    const auto charId = c.lastCharId();
    Npcs npc = npcsOf(c);
    std::uint32_t seq = 0;
    REQUIRE(request(c, vn::QuestAcceptReq{++seq, npc.selvi, 3}).res.result == QR::Ok);
    CHECK(request(c, vn::QuestHandInReq{++seq, npc.selvi, 3, vn::kQuestNoChoice}).res.result == QR::NotReady);
    leave(c);
    // 4 Kül Tozu (3 are taken, one stays) and the rest of the bag full of swords: the Ham stones do not fit.
    REQUIRE(server.accounts().giveItem(charId, kAsh, 4));
    while (server.accounts().giveItem(charId, 1))
    {
    }
    c.enter(charId);
    npc = npcsOf(c);
    const Answer full = request(c, vn::QuestHandInReq{++seq, npc.selvi, 3, vn::kQuestNoChoice});
    CHECK(full.res.result == QR::BagFull);
    CHECK(full.items.empty());
    CHECK(storedUnits(server.accounts().itemsOf(charId), kAsh) == 4);
    CHECK(storedQuest(server.accounts().questsOf(charId), 3)->state == vn::QuestState::Active);
    // A failed write (simulated storage error): ServerError, no QUEST_UPDATE, the row as it was.
    server.accounts().setFailQuestWritesForTesting(true);
    const Answer failed = request(c, vn::QuestAbandonReq{++seq, 3});
    CHECK(failed.res.result == QR::ServerError);
    CHECK(failed.updates.empty());
    server.accounts().setFailQuestWritesForTesting(false);
    CHECK(storedQuest(server.accounts().questsOf(charId), 3)->state == vn::QuestState::Active);
}

TEST_CASE("Quest server: a collect hand-in removes the units and places the reward in their slot", "[server][quests]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("quest_take", "Toplayıcı");
    const auto charId = c.lastCharId();
    leave(c);
    REQUIRE(server.accounts().giveItem(charId, kAsh, 3));
    c.enter(charId);
    Npcs npc = npcsOf(c);
    std::uint32_t seq = 0;
    REQUIRE(request(c, vn::QuestAcceptReq{++seq, npc.selvi, 3}).res.result == QR::Ok);
    const Answer h = request(c, vn::QuestHandInReq{++seq, npc.selvi, 3, vn::kQuestNoChoice});
    REQUIRE(h.res.result == QR::Ok);
    CHECK(h.res.coins == 5);
    // ITEM_UPDATE: the Kül Tozu slot now holds the two Ham stones.
    REQUIRE(h.items.size() == 1);
    REQUIRE(h.items[0].items.size() == 1);
    CHECK(h.items[0].items[0].templateId == kHam);
    CHECK(h.items[0].items[0].quantity == 2);
    const auto items = server.accounts().itemsOf(charId);
    CHECK(storedUnits(items, kAsh) == 0);
    CHECK(storedUnits(items, kHam) == 2);
    CHECK(server.accounts().characterInfo(charId)->coins == 5);
}

TEST_CASE("Quest server: flood cap and malformed quest requests", "[server][quests][flood]")
{
    TestServer server({}, options());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.enterNew("quest_flood", "Acele");
    (void)npcsOf(c);
    for (std::uint32_t i = 1; i <= 6; ++i)
        c.send(vn::QuestAbandonReq{i, 1});
    const auto err = c.waitFor<vn::Error>([](const vn::Error&) { return true; });
    REQUIRE(err);
    CHECK(err->errorCode == vn::ErrorCode::Flood);
    CHECK(c.waitClosed());

    TestClient d;
    REQUIRE(d.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    d.enterNew("quest_bad", "Bozuk");
    d.sendRawPayload(vn::Opcode::QuestAcceptReq, {1, 2, 3});
    const auto bad = d.waitFor<vn::Error>([](const vn::Error&) { return true; });
    REQUIRE(bad);
    CHECK(bad->errorCode == vn::ErrorCode::ProtocolViolation);
}
