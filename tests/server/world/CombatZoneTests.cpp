// Combat in the zone (server/world/ZoneCombat.cpp, docs/PROTOCOL_COMBAT.md) on synthetic terrain with
// a fake clock and fake links: no sockets, no threads, fixed RNG seed. Every rule of the contract
// that the world decides: monster spawn / AOI, auto-attack (range notice, interval, formula,
// killing blow, death, experience, despawn, respawn), skills (class, mp, cooldown, global cooldown,
// cast + interrupt by moving, heal and its threat), the safe zone, NotAttackable players, the AI
// state machine (aggro, chase around obstacles, attack, leash / return heal, wander radius), player
// death and respawn timing, experience reduction and level-up, routing to observers, stance,
// regeneration and its rate limits, equipment changing the attack, persistence of the progress.

#include "server/world/Zone.h"
#include "shared/game/Combat.h"
#include "shared/game/Monsters.h"
#include "tests/server/world/FakeLink.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>

namespace vw = vo::server::world;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using vo::tests::FakeLink;
using K = vw::CombatCommand::Kind;
using R = vn::ActionFailReason;

namespace {

// One monster kind (id 1, level 1) with configurable aggro, a training dummy (id 2: 5000 hp, attack 0,
// never aggressive), one elite (id 5, level 6), a safe zone
// around the Aydor spawn (100, 100) of the test terrain, and the given spawn lines.
std::shared_ptr<const vg::MonsterData> monsters(const std::string& spawns, float aggro = 0.0f, float leash = 20.0f,
                                                std::uint32_t attack = 11, std::uint32_t respawnMs = 10000)
{
    const std::string toml = std::format(R"(
[[monster]]
id = 1
name = "Kül Çömezi"
model = "ash_thrall"
level = 1
hp = 80
attack = {}
defense = 4
attack_range = 2.0
attack_interval_ms = 1500
move_speed = 4.0
wander_speed = 1.3
aggro_radius = {}
leash_radius = {}
xp = 25
respawn_ms = {}

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
aggro_radius = 0.0
leash_radius = 30.0
xp = 300
respawn_ms = 90000
elite = true

[[safe_zone]]
zone = 1
x = 100.0
z = 100.0
radius = 20.0
{}
)",
                                         attack, aggro, leash, respawnMs, spawns);
    auto data = std::make_shared<vg::MonsterData>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseMonsterData(toml, "test.toml", *data, error));
    return data;
}

std::string spawnAt(int monster, float x, float z, float radius = 0.0f, int count = 1)
{
    return std::format("\n[[spawn]]\nzone = 1\nmonster = {}\nx = {}\nz = {}\nradius = {}\ncount = {}\n", monster, x, z,
                       radius, count);
}

struct Harness
{
    std::unique_ptr<vw::Zone> zone;
    std::map<vw::SessionKey, std::shared_ptr<FakeLink>> links;
    vo::core::MonoTime t0 = vo::core::monoNow();
    std::uint64_t tick = 0;
    std::map<vw::SessionKey, std::uint32_t> seq;
    vw::EntityId nextMonsterId = 1000;
    std::vector<std::pair<va::CharacterId, va::CharacterProgress>> saves;

    explicit Harness(std::shared_ptr<const vg::MonsterData> data, vo::assets::TerrainData terrain = vo::tests::makeTestTerrain())
    {
        vw::ZoneSettings s;
        s.savePeriodTicks = 0;
        s.seed = 4242;
        std::string error;
        zone = vw::Zone::create(1, std::move(terrain), s, error);
        REQUIRE(zone);
        zone->beginTick(now(), ms());
        REQUIRE(zone->installMonsters(std::move(data), [this] { return nextMonsterId++; }, now(), error));
    }

    [[nodiscard]] vo::core::MonoTime now() const { return t0 + tick * 50ms; }
    [[nodiscard]] std::uint32_t ms() const { return static_cast<std::uint32_t>(tick * 50); }

    std::shared_ptr<FakeLink> enter(vw::SessionKey s, float x, float z, std::uint8_t cls = 1, std::uint16_t level = 1,
                                    vg::EquipBonus equip = {6, 4}, std::optional<std::uint32_t> hp = std::nullopt,
                                    vg::Attributes statPoints = {})
    {
        auto link = std::make_shared<FakeLink>();
        links[s] = link;
        vw::Zone::EnterParams p;
        p.entityId = static_cast<vw::EntityId>(s);
        p.session = s;
        p.generation = 1;
        p.link = link;
        p.account = static_cast<va::AccountId>(s);
        p.character.id = static_cast<va::CharacterId>(s);
        p.character.name = "Player" + std::to_string(s);
        p.character.race = 1;
        p.character.charClass = cls;
        p.character.level = level;
        p.character.hp = hp;
        p.character.position = {1, x, 0.0f, z, 0.0f};
        p.character.statPoints = statPoints;
        p.equip = equip;
        zone->beginTick(now(), ms());
        zone->enter(std::move(p), now(), ms());
        return link;
    }

    std::optional<vw::MoveVerdict> move(vw::SessionKey s, float x, float z, std::uint8_t flags = vg::kMoveMoving)
    {
        vn::MoveReq r;
        r.seq = ++seq[s];
        r.flags = flags;
        r.x = x;
        r.z = z;
        r.y = zone->terrain().heightAt(x, z);
        zone->beginTick(now(), ms());
        return zone->move(s, r, now() + 10s, ms()); // much later: the distance budget is full
    }

    void cmd(vw::SessionKey s, K kind, std::uint32_t target = 0, std::uint16_t skill = 0, std::uint8_t stance = 0)
    {
        zone->beginTick(now(), ms());
        zone->combat(vw::CombatCommand{s, kind, target, skill, stance});
    }

    void step(int n = 1)
    {
        for (int i = 0; i < n; ++i)
        {
            zone->beginTick(now(), ms());
            zone->update(now(), tick, [this](va::AccountId, va::CharacterId c, va::CharacterPosition,
                                             va::CharacterProgress g, std::function<void(bool)> done) {
                saves.emplace_back(c, g);
                if (done)
                    done(true);
            });
            zone->flush();
            ++tick;
        }
    }
    void stepFor(std::chrono::milliseconds d) { step(static_cast<int>(d / 50ms)); }

    [[nodiscard]] std::vector<vw::EntityId> monsterIds() const
    {
        std::vector<vw::EntityId> ids;
        zone->forEachMonster([&](const vw::Zone::Monster& m) { ids.push_back(m.id); });
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    [[nodiscard]] const vw::Zone::Monster& monster(vw::EntityId id) const { return *zone->findMonster(id); }
    [[nodiscard]] const vw::Zone::PlayerCombat& combat(vw::SessionKey s) const { return zone->findPlayer(s)->combat; }
    [[nodiscard]] const vw::Zone::Player& player(vw::SessionKey s) const { return *zone->findPlayer(s); }

    // M in known(P) <=> P in observers(M) <=> M in the grid and in view; FakeLink views agree.
    void checkInvariants() const
    {
        zone->forEachMonster([&](const vw::Zone::Monster& m) {
            zone->forEachPlayer([&](const vw::Zone::Player& p) {
                const bool known = p.known.contains(m.id);
                const bool observer = m.observers.contains(p.id);
                const bool inView = m.inGrid && zone->grid().inView(p.cell, m.cell);
                INFO("monster " << m.id << " player " << p.id);
                CHECK(known == observer);
                CHECK(known == inView);
                CHECK(links.at(p.session)->visible().contains(m.id) == known);
            });
        });
        for (const auto& [s, link] : links)
            if (zone->findPlayer(s))
                CHECK(link->errors().empty());
    }
};

template <class T, class P>
std::vector<T> filter(const FakeLink& link, P&& pred)
{
    std::vector<T> out;
    for (const auto& m : link.all<T>())
        if (pred(m))
            out.push_back(m);
    return out;
}

std::vector<vn::ActionFail> fails(const FakeLink& link)
{
    return link.all<vn::ActionFail>();
}

bool hasFail(const FakeLink& link, R reason)
{
    const auto f = fails(link);
    return std::any_of(f.begin(), f.end(), [&](const vn::ActionFail& a) { return a.reason == reason; });
}

} // namespace

TEST_CASE("Combat zone: monsters spawn as entity type 2 and follow the AOI rules", "[server][world][combat]")
{
    Harness h(monsters(spawnAt(1, 140.0f, 100.0f) + spawnAt(5, 300.0f, 300.0f)));
    REQUIRE(h.zone->monsterCount() == 2);
    const auto ids = h.monsterIds();
    auto a = h.enter(1, 130.0f, 100.0f);
    h.step();
    const auto spawns = a->all<vn::EntitySpawn>();
    REQUIRE(spawns.size() == 1); // the elite is out of view
    const auto& s = spawns[0];
    CHECK(s.entityId == ids[0]);
    CHECK(s.entityType == vn::kEntityTypeMonster);
    CHECK(s.name == "Kül Çömezi");
    CHECK(s.templateId == 1);
    CHECK(s.level == 1);
    CHECK(s.hp == 80);
    CHECK(s.maxHp == 80);
    CHECK(s.state == 0);
    CHECK(s.race == 0);
    CHECK(s.charClass == 0);
    CHECK(s.x == 140.0f);
    const auto entered = a->entered();
    REQUIRE(entered);
    CHECK(entered->level == 1);
    CHECK(entered->hp == 90);
    CHECK(entered->maxHp == 90);
    CHECK(entered->mp == 34);
    CHECK(entered->attack == 18);
    CHECK(entered->defense == 7);
    CHECK(entered->xpToNext == 80);
    CHECK_FALSE(entered->dead);
    h.checkInvariants();

    // Out of view and back: despawn, spawn; the elite comes into view on the way.
    h.zone->placeEntity(1, 300.0f, 290.0f);
    h.step();
    CHECK(a->visible() == std::set<std::uint32_t>{ids[1]});
    h.checkInvariants();
    h.zone->placeEntity(1, 130.0f, 100.0f);
    h.step();
    CHECK(a->visible() == std::set<std::uint32_t>{ids[0]});
    h.checkInvariants();
    // A second player sees the monster and the first player (players and monsters share the grid).
    auto b = h.enter(2, 132.0f, 100.0f);
    h.step();
    CHECK(b->visible() == std::set<std::uint32_t>{1, ids[0]});
    h.checkInvariants();
    CHECK(h.zone->leave(2));
    CHECK(h.monster(ids[0]).observers == std::unordered_set<vw::EntityId>{1});
}

TEST_CASE("Combat zone: auto-attack, OutOfRange once, interval, formula, kill, experience, despawn, respawn",
          "[server][world][combat]")
{
    Harness h(monsters(spawnAt(1, 140.0f, 100.0f), 0.0f, 20.0f, 0));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 130.0f, 100.0f);
    h.step();
    h.cmd(1, K::Attack, m);
    h.step(10);
    auto f = fails(*a);
    REQUIRE(f.size() == 1); // OutOfRange exactly once, however long it stays out of range
    CHECK(f[0] == vn::ActionFail{vn::CombatAction::Attack, 0, R::OutOfRange});
    CHECK(a->count<vn::CombatEvent>() == 0);

    // Into range (2.5 m + 0.5 m tolerance): it resumes by itself; swings every 1600 ms.
    h.zone->placeEntity(1, 137.2f, 100.0f);
    const auto start = h.tick;
    h.stepFor(3350ms); // swings at 0, 1.6 and 3.2 s
    const auto mine = [&] {
        return filter<vn::CombatEvent>(*a, [&](const vn::CombatEvent& e) { return e.sourceId == 1 && e.targetId == m; });
    };
    auto events = mine();
    REQUIRE(events.size() == 3);
    std::uint32_t hp = 80;
    for (const auto& e : events)
    {
        CHECK(e.kind == vn::kCombatKindDamage);
        CHECK(e.skillId == 0);
        // attack 18, defense 4: round(18 * [0.9, 1.1] - 2), x1.5 when critical.
        if (e.flags & vn::kCombatFlagCritical)
            CHECK((e.amount >= 21 && e.amount <= 27));
        else
            CHECK((e.amount >= 14 && e.amount <= 18));
        CHECK(e.targetHp == hp - e.amount);
        hp = e.targetHp;
    }
    CHECK(events[1].serverTimeMs - events[0].serverTimeMs == 1600);
    CHECK(events[0].serverTimeMs == static_cast<std::uint32_t>((start) * 50));
    // The monster fought back with attack 0: 1 damage per hit (the minimum).
    const auto taken = filter<vn::CombatEvent>(*a, [](const vn::CombatEvent& e) { return e.targetId == 1; });
    REQUIRE_FALSE(taken.empty());
    for (const auto& e : taken)
        CHECK(e.amount == 1);
    CHECK(h.combat(1).hp == 90 - taken.size());

    // Until it dies: killing blow at 0, ENTITY_DEATH, XP_GAIN, dead state; the attack ends.
    for (int i = 0; i < 200 && a->count<vn::EntityDeath>() == 0; ++i)
        h.step();
    events = mine();
    REQUIRE(events.back().targetHp == 0);
    CHECK((events.back().flags & vn::kCombatFlagKillingBlow) != 0);
    std::uint32_t sum = 0;
    for (const auto& e : events)
        sum += e.amount;
    CHECK(sum == 80);
    const auto deaths = a->all<vn::EntityDeath>();
    REQUIRE(deaths.size() == 1);
    CHECK(deaths[0] == vn::EntityDeath{m, 1});
    const auto xp = a->all<vn::XpGain>();
    REQUIRE(xp.size() == 1);
    CHECK(xp[0] == vn::XpGain{25, 25, 80, m});
    CHECK(h.combat(1).xp == 25);
    CHECK(h.combat(1).attackTarget == 0);
    h.step();
    const auto states = filter<vn::EntityState>(*a, [&](const vn::EntityState& s) { return s.entityId == m; });
    REQUIRE_FALSE(states.empty());
    CHECK(states.back().state == vn::kEntityStateDead);

    // Corpse for 3 s, then despawned; back after respawn_ms (10 s from death) at full hp.
    a->clear();
    h.stepFor(2900ms);
    CHECK(a->count<vn::EntityDespawn>() == 0);
    h.stepFor(200ms);
    CHECK(a->count<vn::EntityDespawn>() == 1);
    CHECK(h.monster(m).state == vw::Zone::Monster::State::Gone);
    h.stepFor(6800ms);
    CHECK(a->count<vn::EntitySpawn>() == 0);
    h.stepFor(400ms);
    const auto again = a->all<vn::EntitySpawn>();
    REQUIRE(again.size() == 1);
    CHECK(again[0].entityId == m);
    CHECK(again[0].hp == 80);
    CHECK(again[0].state == 0);
    h.checkInvariants();
    CHECK(h.zone->counters().monsterKills == 1);
    CHECK(h.zone->counters().monsterRespawns == 1);
}

TEST_CASE("Combat zone: attacks on players, unknown and dead targets, dead attackers", "[server][world][combat]")
{
    Harness h(monsters(spawnAt(1, 140.0f, 100.0f) + spawnAt(1, 400.0f, 400.0f)));
    const auto ids = h.monsterIds();
    auto a = h.enter(1, 138.0f, 100.0f);
    auto b = h.enter(2, 139.0f, 101.0f);
    h.step();
    h.cmd(1, K::Attack, 2); // no PvP
    h.cmd(1, K::Attack, 0);
    h.cmd(1, K::Attack, ids[1]); // not spawned to the requester
    h.cmd(1, K::Attack, 77777);
    h.cmd(1, K::Skill, 2, vg::kSkillHeavyBlow);
    h.step();
    const auto f = fails(*a);
    REQUIRE(f.size() == 5);
    CHECK(f[0].reason == R::NotAttackable);
    CHECK(f[1].reason == R::NoTarget);
    CHECK(f[2].reason == R::NoTarget);
    CHECK(f[3].reason == R::NoTarget);
    CHECK(f[4] == vn::ActionFail{vn::CombatAction::Skill, vg::kSkillHeavyBlow, R::NotAttackable});
    CHECK(h.combat(1).attackTarget == 0);
    CHECK(b->count<vn::CombatEvent>() == 0);
}

TEST_CASE("Combat zone: skills - class, mp, cooldown, global cooldown, instant multiplier", "[server][world][combat]")
{
    Harness h(monsters(spawnAt(2, 140.0f, 100.0f)));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 138.0f, 100.0f); // warrior, in melee range
    h.step();
    h.cmd(1, K::Skill, m, vg::kSkillEmberBolt); // a mage skill
    h.cmd(1, K::Skill, m, 99);
    h.cmd(1, K::Skill, m, vg::kSkillHeavyBlow);
    h.cmd(1, K::Skill, m, vg::kSkillHeavyBlow); // global cooldown (500 ms)
    h.step();
    auto f = fails(*a);
    REQUIRE(f.size() == 3);
    CHECK(f[0] == vn::ActionFail{vn::CombatAction::Skill, vg::kSkillEmberBolt, R::UnknownSkill});
    CHECK(f[1] == vn::ActionFail{vn::CombatAction::Skill, 99, R::UnknownSkill});
    CHECK(f[2] == vn::ActionFail{vn::CombatAction::Skill, vg::kSkillHeavyBlow, R::OnCooldown});
    const auto hits = filter<vn::CombatEvent>(*a, [](const vn::CombatEvent& e) { return e.sourceId == 1; });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].skillId == vg::kSkillHeavyBlow);
    // round(18 * 2.2 * [0.9, 1.1] - 2) = 34..42 (x1.5 critical)
    if (hits[0].flags & vn::kCombatFlagCritical)
        CHECK((hits[0].amount >= 51 && hits[0].amount <= 63));
    else
        CHECK((hits[0].amount >= 34 && hits[0].amount <= 42));
    CHECK(h.combat(1).mp == 34 - 12);
    // Its own cooldown is 6 s.
    a->clear();
    h.stepFor(600ms);
    h.cmd(1, K::Skill, m, vg::kSkillHeavyBlow);
    h.step();
    CHECK(hasFail(*a, R::OnCooldown));
    h.stepFor(5500ms);
    a->clear();
    h.cmd(1, K::Skill, m, vg::kSkillHeavyBlow);
    h.step();
    CHECK(fails(*a).empty());
    CHECK(h.combat(1).mp >= 34 - 24); // (regeneration may have started)
    // Not enough mana.
    h.zone->setVitalsForTesting(1, 90, 5);
    h.stepFor(6500ms);
    a->clear();
    h.cmd(1, K::Skill, m, vg::kSkillHeavyBlow);
    h.step();
    CHECK(hasFail(*a, R::NotEnoughMp));
}

TEST_CASE("Combat zone: casts complete after cast_ms, moving interrupts them for free", "[server][world][combat]")
{
    Harness h(monsters(spawnAt(2, 140.0f, 100.0f)));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 128.0f, 100.0f, 3, 1, {7, 0}); // mage, 12 m away (Köz Oku: 16 m)
    auto watcher = h.enter(2, 125.0f, 104.0f);
    h.step();
    const std::uint32_t castAt = h.ms();
    h.cmd(1, K::Skill, m, vg::kSkillEmberBolt);
    h.step();
    auto starts = a->all<vn::CastStart>();
    REQUIRE(starts.size() == 1);
    CHECK(starts[0] == vn::CastStart{1, vg::kSkillEmberBolt, m, 600});
    CHECK(watcher->all<vn::CastStart>().size() == 1); // observers see it too
    h.cmd(1, K::Skill, m, vg::kSkillEmberBolt);
    h.step();
    CHECK(hasFail(*a, R::Busy));
    h.stepFor(550ms);
    auto ends = a->all<vn::CastEnd>();
    REQUIRE(ends.size() == 1);
    CHECK(ends[0] == vn::CastEnd{1, vg::kSkillEmberBolt, vn::kCastCompleted});
    const auto hit = filter<vn::CombatEvent>(*a, [](const vn::CombatEvent& e) { return e.sourceId == 1; });
    REQUIRE(hit.size() == 1);
    CHECK(hit[0].skillId == vg::kSkillEmberBolt);
    CHECK(hit[0].serverTimeMs == castAt + 600); // applied when the cast completes
    // round(18 * 2.0 * [0.9, 1.1] - 2) = 30..38 (x1.5 critical)
    CHECK((hit[0].amount >= 30 && hit[0].amount <= 57));
    CHECK(h.combat(1).mp == 70 - 14);

    // A second cast, interrupted by moving: CAST_END(interrupted) + ACTION_FAIL(Interrupted), no
    // mana spent, no cooldown started.
    h.stepFor(3100ms);
    a->clear();
    watcher->clear();
    const auto mpBefore = h.combat(1).mp;
    h.cmd(1, K::Skill, m, vg::kSkillEmberBolt);
    h.step(4);
    REQUIRE(h.move(1, 127.0f, 100.0f) == vw::MoveVerdict::Accepted);
    h.step();
    ends = a->all<vn::CastEnd>();
    REQUIRE(ends.size() == 1);
    CHECK(ends[0].result == vn::kCastInterrupted);
    CHECK(watcher->all<vn::CastEnd>().size() == 1);
    CHECK(fails(*a) == std::vector<vn::ActionFail>{{vn::CombatAction::Skill, vg::kSkillEmberBolt, R::Interrupted}});
    CHECK(h.combat(1).mp >= mpBefore);
    CHECK(filter<vn::CombatEvent>(*a, [](const vn::CombatEvent& e) { return e.sourceId == 1; }).empty());
    REQUIRE(h.move(1, 127.0f, 100.0f, 0) == vw::MoveVerdict::Accepted); // stop
    h.stepFor(600ms);
    a->clear();
    h.cmd(1, K::Skill, m, vg::kSkillEmberBolt); // no cooldown was started
    h.step();
    CHECK(fails(*a).empty());
    CHECK(a->all<vn::CastStart>().size() == 1);
}

TEST_CASE("Combat zone: heals on self and friendly players, threat x0.5 towards the healer", "[server][world][combat]")
{
    Harness h(monsters(spawnAt(2, 140.0f, 100.0f)));
    const auto m = h.monsterIds()[0];
    auto priest = h.enter(1, 130.0f, 100.0f, 4, 1, {5, 0});
    auto tank = h.enter(2, 138.0f, 100.0f);
    auto distant = h.enter(3, 300.0f, 300.0f);
    h.step();
    h.cmd(1, K::Skill, m, vg::kSkillPaleSun); // a monster: wrong target type
    h.cmd(1, K::Skill, 3, vg::kSkillPaleSun); // not spawned to the priest
    h.step();
    auto f = fails(*priest);
    REQUIRE(f.size() == 2);
    CHECK(f[0].reason == R::WrongTargetType);
    CHECK(f[1].reason == R::NoTarget);

    // The tank pulls the dummy (1 damage per hit) and is low on hp.
    h.cmd(2, K::Attack, m);
    h.stepFor(2000ms);
    REQUIRE(h.combat(2).hp < 90);
    h.zone->setVitalsForTesting(2, 30, 34);
    priest->clear();
    tank->clear();
    h.cmd(1, K::Skill, 2, vg::kSkillPaleSun);
    h.stepFor(850ms);
    const auto heals = filter<vn::CombatEvent>(*tank, [](const vn::CombatEvent& e) { return e.kind == vn::kCombatKindHeal; });
    REQUIRE(heals.size() == 1);
    CHECK(heals[0].sourceId == 1);
    CHECK(heals[0].targetId == 2);
    CHECK(heals[0].skillId == vg::kSkillPaleSun);
    CHECK(heals[0].amount == 22); // PROTOCOL_STATS.md §2: 19 % x (1 + 18 / 60) of 90 (rounded)
    CHECK(priest->all<vn::CastEnd>().size() == 1);
    CHECK(h.combat(1).mp == 64 - 16);
    // The healer is on the monster's threat list now, with half the healing.
    const auto& threats = h.monster(m).threats;
    const auto it = std::find_if(threats.begin(), threats.end(), [](const auto& t) { return t.player == 1; });
    REQUIRE(it != threats.end());
    CHECK(it->threat == 0.5f * static_cast<float>(heals[0].amount));
    CHECK(it->damage == 0);
    // Target 0 heals the caster itself.
    h.zone->setVitalsForTesting(1, 40, 64);
    h.stepFor(4000ms);
    priest->clear();
    h.cmd(1, K::Skill, 0, vg::kSkillPaleSun);
    h.stepFor(850ms);
    const auto self = filter<vn::CombatEvent>(*priest, [](const vn::CombatEvent& e) { return e.kind == vn::kCombatKindHeal; });
    REQUIRE(self.size() == 1);
    CHECK(self[0].targetId == 1);
    CHECK(self[0].amount == 23); // 19 % x 1.3 of 95
    CHECK(distant->count<vn::CombatEvent>() == 0);
}

TEST_CASE("Combat zone: the safe zone - nobody is attacked inside, monsters never enter", "[server][world][combat]")
{
    // A monster right outside the safe zone (radius 20 around 100/100), aggressive.
    Harness h(monsters(spawnAt(1, 121.0f, 100.0f), 6.0f, 25.0f));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 117.0f, 100.0f); // inside, 4 m from it
    h.step(10);
    CHECK(h.monster(m).state == vw::Zone::Monster::State::Idle); // no aggro on someone in the safe zone
    // Attacking from inside (in range): InSafeZone (once), no damage; skills too.
    h.zone->placeEntity(1, 119.0f, 100.0f);
    h.cmd(1, K::Attack, m);
    h.step(5);
    CHECK(fails(*a) == std::vector<vn::ActionFail>{{vn::CombatAction::Attack, 0, R::InSafeZone}});
    h.cmd(1, K::Skill, m, vg::kSkillHeavyBlow);
    h.step();
    CHECK(hasFail(*a, R::InSafeZone));
    CHECK(a->count<vn::CombatEvent>() == 0);

    // Step outside: it aggroes; the player runs back in; the monster never follows into the zone,
    // gives up and walks home at full hp.
    h.cmd(1, K::AttackStop);
    h.zone->placeEntity(1, 126.0f, 100.0f);
    h.step(4);
    CHECK(h.monster(m).state == vw::Zone::Monster::State::Chase);
    h.zone->placeEntity(1, 119.0f, 100.0f);
    for (int i = 0; i < 100; ++i)
    {
        h.step();
        const auto& mon = h.monster(m);
        CHECK(std::hypot(mon.x - 100.0f, mon.z - 100.0f) > 20.0f);
    }
    CHECK(h.monster(m).state == vw::Zone::Monster::State::Idle);
    CHECK(h.monster(m).hp == 80);
    CHECK(h.combat(1).hp == 90); // never hit inside
}

TEST_CASE("Combat zone: AI - aggro, chase around obstacles, attack interval, leash and return heal",
          "[server][world][combat][ai]")
{
    // A wall between the player's spot and the monster: the chase must walk around it.
    auto terrain = vo::tests::makeTestTerrain();
    for (float z = 90.0f; z <= 112.0f; z += 1.0f)
        vo::tests::blockCell(terrain, 150.5f, z);
    Harness h(monsters(spawnAt(1, 155.0f, 100.0f), 6.0f, 25.0f), std::move(terrain));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 145.0f, 100.0f); // 10 m: out of aggro range (and of skill range)
    h.step(10);
    CHECK(h.monster(m).state == vw::Zone::Monster::State::Idle);
    h.cmd(1, K::Skill, m, vg::kSkillHeavyBlow);
    h.step();
    CHECK(hasFail(*a, R::OutOfRange));
    CHECK(h.monster(m).state == vw::Zone::Monster::State::Idle);
    // Within 6 m on its side of the wall: aggro. Then behind the wall: it walks around it, never
    // onto a blocked cell.
    h.zone->placeEntity(1, 153.0f, 105.0f);
    h.step(5);
    REQUIRE(h.monster(m).state == vw::Zone::Monster::State::Chase);
    h.zone->placeEntity(1, 146.0f, 100.0f);
    bool reached = false;
    for (int i = 0; i < 200 && !reached; ++i)
    {
        h.step();
        const auto& mon = h.monster(m);
        CHECK(h.zone->terrain().isWalkable(mon.x, mon.z));
        reached = std::hypot(mon.x - 146.0f, mon.z - 100.0f) <= 2.5f;
    }
    REQUIRE(reached);
    CHECK(h.zone->counters().pathSearches > 0);
    // Attacks every 1.5 s once in range.
    a->clear();
    h.stepFor(4600ms);
    const auto hits = filter<vn::CombatEvent>(*a, [&](const vn::CombatEvent& e) { return e.sourceId == m; });
    CHECK((hits.size() == 3 || hits.size() == 4));
    for (std::size_t i = 1; i < hits.size(); ++i)
        CHECK(hits[i].serverTimeMs - hits[i - 1].serverTimeMs == 1500);
    // Damage it, then run beyond the leash (25 m from its spawn): it returns home, heals to full and
    // ignores damage on the way.
    h.cmd(1, K::Attack, m);
    h.stepFor(1700ms);
    REQUIRE(h.monster(m).hp < 80);
    h.zone->placeEntity(1, 120.5f, 140.0f); // > 25 m from 155/100, outside the safe zone
    h.step(3);
    REQUIRE(h.monster(m).state == vw::Zone::Monster::State::Return);
    CHECK(h.monster(m).hp == 80);
    h.step();
    const auto stats = filter<vn::EntityStats>(*a, [&](const vn::EntityStats& s) { return s.entityId == m; });
    REQUIRE_FALSE(stats.empty());
    CHECK(stats.back().hp == 80);
    h.cmd(1, K::AttackStop);
    h.stepFor(600ms); // past the global cooldown of the last swing
    REQUIRE(h.monster(m).state == vw::Zone::Monster::State::Return);
    h.zone->placeEntity(1, h.monster(m).x - 1.0f, h.monster(m).z);
    a->clear();
    h.cmd(1, K::Skill, m, vg::kSkillHeavyBlow);
    h.step();
    CHECK(hasFail(*a, R::NotAttackable));
    h.zone->placeEntity(1, 120.5f, 140.0f);
    for (int i = 0; i < 400 && h.monster(m).state != vw::Zone::Monster::State::Idle; ++i)
        h.step();
    CHECK(h.monster(m).state == vw::Zone::Monster::State::Idle);
    CHECK(std::hypot(h.monster(m).x - 155.0f, h.monster(m).z - 100.0f) <= 0.6f);
    CHECK(h.monster(m).hp == 80);
    h.checkInvariants();
}

TEST_CASE("Combat zone: idle monsters wander inside their radius on walkable cells", "[server][world][combat][ai]")
{
    auto terrain = vo::tests::makeTestTerrain();
    vo::tests::blockCell(terrain, 202.5f, 200.5f);
    vo::tests::blockCell(terrain, 200.5f, 203.5f);
    Harness h(monsters(spawnAt(1, 200.0f, 200.0f, 5.0f, 3)), std::move(terrain));
    REQUIRE(h.zone->monsterCount() == 3);
    // Nobody looks: nobody wanders.
    std::map<vw::EntityId, std::pair<float, float>> start;
    h.zone->forEachMonster([&](const vw::Zone::Monster& m) { start[m.id] = {m.x, m.z}; });
    h.stepFor(20000ms);
    h.zone->forEachMonster([&](const vw::Zone::Monster& m) {
        CHECK(m.x == start[m.id].first);
        CHECK(m.z == start[m.id].second);
    });
    auto a = h.enter(1, 180.0f, 180.0f);
    bool moved = false;
    for (int i = 0; i < 1200; ++i)
    {
        h.step();
        h.zone->forEachMonster([&](const vw::Zone::Monster& m) {
            CHECK(std::hypot(m.x - 200.0f, m.z - 200.0f) <= 5.0f + 1e-3f);
            CHECK(h.zone->terrain().isWalkable(m.x, m.z));
            moved = moved || m.x != start[m.id].first;
        });
    }
    CHECK(moved);
    const auto moves = a->all<vn::EntityMove>();
    CHECK_FALSE(moves.empty());
    h.checkInvariants();
}

TEST_CASE("Combat zone: player death, dead rules, respawn after 5 s at the race spawn with 50 %",
          "[server][world][combat]")
{
    Harness h(monsters(spawnAt(5, 160.0f, 100.0f)));
    const auto elite = h.monsterIds()[0];
    auto a = h.enter(1, 158.0f, 100.0f);
    auto watcher = h.enter(2, 150.0f, 104.0f);
    h.step();
    h.cmd(1, K::Attack, elite);
    for (int i = 0; i < 200 && !h.combat(1).dead; ++i)
        h.step();
    REQUIRE(h.combat(1).dead);
    h.step();
    const auto deaths = watcher->all<vn::EntityDeath>();
    REQUIRE(deaths.size() == 1);
    CHECK(deaths[0] == vn::EntityDeath{1, elite});
    const auto killing = filter<vn::CombatEvent>(*a, [](const vn::CombatEvent& e) { return e.targetId == 1; });
    REQUIRE_FALSE(killing.empty());
    CHECK(killing.back().targetHp == 0);
    CHECK((killing.back().flags & vn::kCombatFlagKillingBlow) != 0);
    const auto states = filter<vn::EntityState>(*watcher, [](const vn::EntityState& s) { return s.entityId == 1; });
    REQUIRE_FALSE(states.empty());
    CHECK(states.back().state == vn::kEntityStateDead);
    const auto self = a->all<vn::SelfStats>();
    REQUIRE_FALSE(self.empty());
    CHECK(self.back().hp == 0);
    // Dead: cannot move, attack or be a target; the monster goes home.
    CHECK(h.move(1, 157.0f, 100.0f) == vw::MoveVerdict::Dead);
    h.step();
    CHECK(a->count<vn::PositionCorrect>() == 1);
    a->clear();
    h.cmd(1, K::Attack, elite);
    h.cmd(1, K::Skill, elite, vg::kSkillHeavyBlow);
    h.cmd(1, K::Stance, 0, 0, vn::kStanceSit);
    h.cmd(1, K::Respawn);
    h.step();
    auto f = fails(*a);
    REQUIRE(f.size() == 3);
    CHECK(f[0].reason == R::YouAreDead);
    CHECK(f[1].reason == R::YouAreDead);
    CHECK(f[2] == vn::ActionFail{vn::CombatAction::Stance, 0, R::Busy});
    auto res = a->all<vn::RespawnRes>();
    REQUIRE(res.size() == 1);
    CHECK(res[0].result == vn::RespawnResult::TooEarly);
    h.stepFor(1000ms);
    CHECK(h.monster(elite).state != vw::Zone::Monster::State::Chase);
    // After 5 s: at the race spawn (spread <= 2 m), 50 % hp/mp; observers get ENTITY_STATE + ENTITY_MOVE.
    h.stepFor(3800ms);
    a->clear();
    watcher->clear();
    h.cmd(1, K::Respawn);
    h.step();
    res = a->all<vn::RespawnRes>();
    REQUIRE(res.size() == 1);
    CHECK(res[0].result == vn::RespawnResult::Ok);
    CHECK(std::hypot(res[0].x - 100.0f, res[0].z - 100.0f) <= 2.0f + 1e-3f);
    CHECK(res[0].hp == 45);
    CHECK(res[0].mp == 17);
    CHECK_FALSE(h.combat(1).dead);
    const auto cleared = filter<vn::EntityState>(*watcher, [](const vn::EntityState& s) { return s.entityId == 1; });
    REQUIRE(cleared.size() == 1);
    CHECK(cleared[0].state == 0);
    h.step(3);
    const auto moved = filter<vn::EntityMove>(*watcher, [](const vn::EntityMove& m) { return m.entityId == 1; });
    REQUIRE_FALSE(moved.empty());
    CHECK(moved.back().x == res[0].x);
    // Alive: a respawn request is NotDead, moving works again.
    a->clear();
    h.cmd(1, K::Respawn);
    h.step();
    CHECK(a->all<vn::RespawnRes>().at(0).result == vn::RespawnResult::NotDead);
    CHECK(h.move(1, res[0].x + 0.5f, res[0].z) == vw::MoveVerdict::Accepted);
    CHECK(h.zone->counters().playerDeaths == 1);
    h.checkInvariants();
}

TEST_CASE("Combat zone: experience reduction by level difference, level-up broadcast and save",
          "[server][world][combat]")
{
    Harness h(monsters(spawnAt(1, 140.0f, 100.0f) + spawnAt(1, 160.0f, 100.0f), 0.0f, 20.0f, 0));
    const auto ids = h.monsterIds();
    // Level 5 vs a level-1 monster: 4 levels above -> 2 beyond the free 2 -> 60 % of 25 = 15.
    auto a = h.enter(1, 138.0f, 100.0f, 1, 5);
    auto watcher = h.enter(2, 150.0f, 104.0f);
    h.step();
    h.cmd(1, K::Attack, ids[0]);
    for (int i = 0; i < 400 && a->count<vn::XpGain>() == 0; ++i)
        h.step();
    REQUIRE(a->count<vn::XpGain>() == 1);
    CHECK(a->all<vn::XpGain>()[0].amount == 15);
    // A level-1 character with 70 xp kills one: 95 >= 80 -> level 2 with 15 xp, full hp, saved now.
    auto b = h.enter(3, 162.0f, 100.0f);
    h.step();
    const_cast<vw::Zone::PlayerCombat&>(h.combat(3)).xp = 70; // (as if loaded with 70 xp)
    h.zone->setVitalsForTesting(3, 50, 10);
    h.saves.clear();
    watcher->clear();
    h.cmd(3, K::Attack, ids[1]);
    for (int i = 0; i < 400 && b->count<vn::LevelUp>() == 0; ++i)
        h.step();
    REQUIRE(b->count<vn::LevelUp>() == 1);
    CHECK(b->all<vn::LevelUp>()[0] == vn::LevelUp{3, 2});
    CHECK(watcher->all<vn::LevelUp>() == std::vector<vn::LevelUp>{{3, 2}});
    const auto xp = b->all<vn::XpGain>();
    REQUIRE(xp.size() == 1);
    CHECK(xp[0] == vn::XpGain{25, 15, 240, ids[1]});
    CHECK(h.player(3).character.level == 2);
    const vg::Stats l2 = vg::deriveStats(1, 2, {6, 4});
    CHECK(h.combat(3).hp == l2.maxHp);
    CHECK(h.combat(3).mp == l2.maxMp);
    h.stepFor(300ms);
    const auto self = b->all<vn::SelfStats>();
    REQUIRE_FALSE(self.empty());
    CHECK(self.back().maxHp == l2.maxHp);
    CHECK(self.back().attack == l2.attack);
    const auto saved = std::find_if(h.saves.begin(), h.saves.end(), [](const auto& s) { return s.first == 3; });
    REQUIRE(saved != h.saves.end());
    CHECK(saved->second.level == 2);
    CHECK(saved->second.xp == 15);
}

TEST_CASE("Combat zone: COMBAT_EVENT reaches observers of the source or the target, not others",
          "[server][world][combat][aoi]")
{
    // Attacker A at x 130, monster at 160 (x cells 4 and 5 of 32 m), observer B sees only the monster
    // (x 230, cell 7), observer C sees only A (x 70, cell 2), D sees neither.
    Harness h(monsters(spawnAt(2, 160.0f, 100.0f)));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 130.0f, 100.0f, 3, 1, {7, 0}); // mage: hits from range
    auto b = h.enter(2, 230.0f, 100.0f);
    auto c = h.enter(3, 70.0f, 100.0f);
    auto d = h.enter(4, 400.0f, 400.0f);
    h.step();
    REQUIRE(h.player(2).known.contains(m));
    REQUIRE_FALSE(h.player(2).known.contains(1));
    REQUIRE(h.player(3).known.contains(1));
    REQUIRE_FALSE(h.player(3).known.contains(m));
    h.zone->placeEntity(1, 146.0f, 100.0f);
    h.cmd(1, K::Skill, m, vg::kSkillEmberBolt);
    h.stepFor(700ms);
    const auto pred = [&](const vn::CombatEvent& e) { return e.sourceId == 1 && e.targetId == m; };
    CHECK(filter<vn::CombatEvent>(*a, pred).size() == 1);
    CHECK(filter<vn::CombatEvent>(*b, pred).size() == 1);
    CHECK(filter<vn::CombatEvent>(*c, pred).size() == 1);
    CHECK(d->count<vn::CombatEvent>() == 0);
    CHECK(b->all<vn::CastStart>().size() == 1);
    CHECK(c->all<vn::CastEnd>().size() == 1);
    CHECK(d->count<vn::CastStart>() == 0);
}

TEST_CASE("Combat zone: stance - sitting rules, standing up, doubled regeneration", "[server][world][combat]")
{
    Harness h(monsters(spawnAt(2, 140.0f, 100.0f)));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 130.0f, 100.0f);
    auto watcher = h.enter(2, 131.0f, 103.0f);
    h.step();
    h.cmd(1, K::Stance, 0, 0, vn::kStanceSit);
    h.step();
    CHECK(h.combat(1).sitting);
    auto st = filter<vn::EntityState>(*watcher, [](const vn::EntityState& s) { return s.entityId == 1; });
    REQUIRE(st.size() == 1);
    CHECK(st[0].state == vn::kEntityStateSitting);
    // A move with the moving bit stands up.
    REQUIRE(h.move(1, 130.5f, 100.0f) == vw::MoveVerdict::Accepted);
    h.step();
    CHECK_FALSE(h.combat(1).sitting);
    // Refused while moving.
    a->clear();
    h.cmd(1, K::Stance, 0, 0, vn::kStanceSit);
    h.step();
    CHECK(fails(*a) == std::vector<vn::ActionFail>{{vn::CombatAction::Stance, 0, R::Busy}});
    REQUIRE(h.move(1, 130.5f, 100.0f, 0) == vw::MoveVerdict::Accepted);
    // An attack request stands up; in combat sitting is refused.
    h.cmd(1, K::Stance, 0, 0, vn::kStanceSit);
    h.step();
    REQUIRE(h.combat(1).sitting);
    h.cmd(1, K::Attack, m);
    CHECK_FALSE(h.combat(1).sitting);
    h.zone->placeEntity(1, 138.0f, 100.0f);
    h.stepFor(2000ms);
    a->clear();
    h.cmd(1, K::Stance, 0, 0, vn::kStanceSit);
    h.step();
    CHECK(fails(*a) == std::vector<vn::ActionFail>{{vn::CombatAction::Stance, 0, R::Busy}});
    st = filter<vn::EntityState>(*watcher, [](const vn::EntityState& s) { return s.entityId == 1; });
    CHECK((st.back().state & vn::kEntityStateInCombat) != 0);

    // Regeneration: none in combat; 6 s after the last damage 2 %/s, doubled while sitting.
    h.cmd(1, K::AttackStop);
    h.zone->placeEntity(1, 118.0f, 100.0f); // into the safe zone: the monster gives up
    h.stepFor(1000ms);
    h.zone->setVitalsForTesting(1, 30, 0);
    const auto lowHp = h.combat(1).hp;
    h.stepFor(3000ms);
    CHECK(h.combat(1).hp == lowHp); // still in combat
    h.stepFor(4000ms);
    CHECK(h.combat(1).hp > lowHp);
    st = filter<vn::EntityState>(*watcher, [](const vn::EntityState& s) { return s.entityId == 1; });
    CHECK((st.back().state & vn::kEntityStateInCombat) == 0);
    h.zone->setVitalsForTesting(1, 30, 0);
    h.stepFor(1000ms);
    const auto standing0 = h.combat(1).hp;
    h.stepFor(5000ms);
    const auto standingGain = h.combat(1).hp - standing0; // 1.8 x 1.11 hp/s (VIT 11): 10 in 5 s
    h.zone->setVitalsForTesting(1, 30, 0);
    h.cmd(1, K::Stance, 0, 0, vn::kStanceSit);
    h.stepFor(1000ms);
    const auto sitting0 = h.combat(1).hp;
    h.stepFor(5000ms);
    const auto sittingGain = h.combat(1).hp - sitting0; // 4.0 hp/s: 20 in 5 s
    CHECK((standingGain >= 9 && standingGain <= 11));
    CHECK((sittingGain >= 19 && sittingGain <= 21));
    // Taking damage stands the player up.
    h.zone->placeEntity(1, 138.0f, 100.0f);
    h.cmd(1, K::Attack, m);
    CHECK_FALSE(h.combat(1).sitting); // (the attack request already did)
}

TEST_CASE("Combat zone: SELF_STATS at most 4 Hz, ENTITY_STATS at most 2 Hz", "[server][world][combat]")
{
    Harness h(monsters(""));
    auto a = h.enter(1, 130.0f, 100.0f);
    auto watcher = h.enter(2, 131.0f, 103.0f);
    h.step();
    for (int i = 0; i < 40; ++i)
    {
        h.zone->setVitalsForTesting(1, 10 + static_cast<std::uint32_t>(i), 1);
        const_cast<vw::Zone::PlayerCombat&>(h.combat(1)).entityStatsDirty = true;
        h.step();
    }
    h.stepFor(300ms); // the last change still goes out
    const auto self = a->all<vn::SelfStats>();
    CHECK((self.size() >= 8 && self.size() <= 10)); // 2.3 s at <= 4 Hz
    for (std::size_t i = 1; i < self.size(); ++i)
        CHECK(self[i].hp != self[i - 1].hp);
    const auto others = filter<vn::EntityStats>(*watcher, [](const vn::EntityStats& s) { return s.entityId == 1; });
    CHECK((others.size() >= 4 && others.size() <= 6)); // at <= 2 Hz
    CHECK(self.back().hp >= 49);
}

TEST_CASE("Combat zone: equipment bonuses change attack/defense and SELF_STATS", "[server][world][combat]")
{
    Harness h(monsters(""));
    auto a = h.enter(1, 130.0f, 100.0f, 1, 1, {6, 4});
    h.step();
    CHECK(h.combat(1).stats.attack == 18);
    h.zone->setEquipment(1, 6, 0, {10, 0}); // two-handed axe, no shield
    h.step();
    CHECK(h.combat(1).stats.attack == 24); // PROTOCOL_STATS.md §2: 2 + 2 + 10 x (1 + 12 / 20) + 12 / 3
    CHECK(h.combat(1).stats.defense == 3);
    const auto self = a->all<vn::SelfStats>();
    REQUIRE(self.size() == 1);
    CHECK(self[0].attack == 24);
    CHECK(self[0].defense == 3);
    CHECK(a->count<vn::EntityEquip>() == 1);
    // Bare-handed is allowed: no bonus.
    h.stepFor(300ms);
    h.zone->setEquipment(1, 0, 0, {});
    h.step();
    CHECK(h.combat(1).stats.attack == 8); // 2 + 2 + 12 / 3: weapons matter more since M11
}

TEST_CASE("Combat zone: progress on leave; a character that left dead enters at its race spawn with 50 %",
          "[server][world][combat]")
{
    Harness h(monsters(""));
    h.enter(1, 130.0f, 100.0f, 2, 3);
    h.zone->setVitalsForTesting(1, 40, 7);
    const_cast<vw::Zone::PlayerCombat&>(h.combat(1)).xp = 123;
    const auto left = h.zone->leave(1);
    REQUIRE(left);
    CHECK(left->progress == va::CharacterProgress{3, 123, 40, 7});

    auto b = h.enter(2, 300.0f, 300.0f, 1, 1, {6, 4}, 0u); // stored hp 0: left dead
    auto c = h.enter(3, 300.0f, 300.0f, 1, 1, {6, 4}, 500u); // above the maximum: clamped
    h.step();
    const auto e = b->entered();
    REQUIRE(e);
    CHECK(std::hypot(e->x - 100.0f, e->z - 100.0f) <= 2.0f + 1e-3f);
    CHECK(e->hp == 45);
    CHECK(e->mp == 17);
    CHECK_FALSE(e->dead);
    REQUIRE(c->entered());
    CHECK(c->entered()->hp == 90);
    CHECK(c->entered()->x == 300.0f);
}

// ---- Stats (docs/PROTOCOL_STATS.md) ----

namespace {

void checkCharStats(const vn::CharStats& s, const vg::Stats& e, std::uint16_t level)
{
    CHECK(s.level == level);
    for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
    {
        CHECK(s.attributes[i].base == e.base[i]);
        CHECK(s.attributes[i].bonus == e.bonus[i]);
    }
    CHECK(s.unspent == e.unspent);
    CHECK(s.maxHp == e.maxHp);
    CHECK(s.maxMp == e.maxMp);
    CHECK(s.attack == e.attack);
    CHECK(s.defense == e.defense);
    CHECK(s.critPermille == e.critPermille);
    CHECK(s.dodgePermille == e.dodgePermille);
    CHECK(s.hpRegenTenths == static_cast<std::uint16_t>(std::lround(e.hpRegen * 10.0f)));
    CHECK(s.mpRegenTenths == static_cast<std::uint16_t>(std::lround(e.mpRegen * 10.0f)));
    CHECK(s.attackIntervalMs == e.attackIntervalMs);
    CHECK(s.healPermille == vg::healPermille(*vg::findSkill(vg::kSkillPaleSun), e.healMultiplier));
}

} // namespace

TEST_CASE("Stats zone: CHAR_STATS on entry with the stored allocation, before any ENTITY_SPAWN",
          "[server][world][stats]")
{
    Harness h(monsters(spawnAt(2, 140.0f, 100.0f)));
    auto a = h.enter(1, 130.0f, 100.0f, 1, 5, {6, 4}, std::nullopt, {6, 0, 6, 0, 0}); // 12 of 12 points
    h.step();
    const auto pa = a->packets();
    REQUIRE(pa.size() >= 3);
    CHECK(pa[0].opcode == static_cast<std::uint16_t>(vn::Opcode::EnterWorldRes));
    CHECK(pa[1].opcode == static_cast<std::uint16_t>(vn::Opcode::CharStats));
    const auto stats = a->all<vn::CharStats>();
    REQUIRE(stats.size() == 1);
    const vg::Stats expected = vg::deriveStats(1, 5, {6, 0, 6, 0, 0}, {6, 4});
    checkCharStats(stats[0], expected, 5);
    CHECK(stats[0].attributes[0].base == 18);
    CHECK(stats[0].attributes[2].base == 17);
    CHECK(stats[0].unspent == 0);
    // ENTER_WORLD_RES / the zone's combat state use the same derived values.
    CHECK(a->entered()->maxHp == expected.maxHp);
    CHECK(a->entered()->attack == expected.attack);
    CHECK(h.combat(1).stats == expected);
    // A character from before M11 (nothing allocated) at level 7: 18 points unspent.
    auto b = h.enter(2, 132.0f, 100.0f, 3, 7, {7, 0});
    h.step();
    REQUIRE(b->count<vn::CharStats>() == 1);
    CHECK(b->all<vn::CharStats>()[0].unspent == 18);
    CHECK(b->all<vn::CharStats>()[0].attributes[3].base == 13);
}

TEST_CASE("Stats zone: a new allocation recomputes the stats, CHAR_STATS and SELF_STATS follow, hp stays",
          "[server][world][stats]")
{
    Harness h(monsters(""));
    auto a = h.enter(1, 130.0f, 100.0f, 1, 4); // 9 points unspent
    auto watcher = h.enter(2, 131.0f, 102.0f);
    h.step();
    const auto hpBefore = h.combat(1).hp;
    const auto maxBefore = h.combat(1).stats.maxHp;
    a->clear();
    watcher->clear();
    h.zone->beginTick(h.now(), h.ms());
    h.zone->setAllocation(1, {3, 0, 6, 0, 0});
    h.step();
    const auto stats = a->all<vn::CharStats>();
    REQUIRE(stats.size() == 1);
    const vg::Stats expected = vg::deriveStats(1, 4, {3, 0, 6, 0, 0}, {6, 4});
    checkCharStats(stats[0], expected, 4);
    CHECK(stats[0].unspent == 0);
    CHECK(h.combat(1).stats == expected);
    CHECK(h.combat(1).stats.maxHp == maxBefore + 30);
    CHECK(h.combat(1).hp == hpBefore); // a higher maximum does not fill hp
    h.stepFor(300ms);
    const auto self = a->all<vn::SelfStats>();
    REQUIRE_FALSE(self.empty());
    CHECK(self.back().maxHp == expected.maxHp);
    CHECK(self.back().attack == expected.attack);
    // Observers see the new maximum on the hp bar; they never get CHAR_STATS.
    const auto others = filter<vn::EntityStats>(*watcher, [](const vn::EntityStats& s) { return s.entityId == 1; });
    REQUIRE_FALSE(others.empty());
    CHECK(others.back().maxHp == expected.maxHp);
    CHECK(watcher->count<vn::CharStats>() == 0);
    // The same allocation again: nothing. A reset (vo_admin reset-stats picked up later): clamped hp.
    a->clear();
    h.zone->setAllocation(1, {3, 0, 6, 0, 0});
    h.step();
    CHECK(a->count<vn::CharStats>() == 0);
    h.zone->setVitalsForTesting(1, h.combat(1).stats.maxHp, 0);
    h.zone->setAllocation(1, {});
    h.step();
    REQUIRE(a->count<vn::CharStats>() == 1);
    CHECK(a->all<vn::CharStats>()[0].unspent == 9);
    CHECK(h.combat(1).hp == vg::deriveStats(1, 4, {6, 4}).maxHp);
    // Unknown sessions are ignored.
    h.zone->setAllocation(99, {1, 0, 0, 0, 0});
}

TEST_CASE("Stats zone: a level-up grants 3 points and sends CHAR_STATS after LEVEL_UP", "[server][world][stats]")
{
    Harness h(monsters(spawnAt(1, 140.0f, 100.0f), 0.0f, 20.0f, 0));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 138.0f, 100.0f, 1, 1, {6, 4}, std::nullopt);
    h.step();
    const_cast<vw::Zone::PlayerCombat&>(h.combat(1)).xp = 70;
    a->clear();
    h.cmd(1, K::Attack, m);
    for (int i = 0; i < 400 && a->count<vn::LevelUp>() == 0; ++i)
        h.step();
    REQUIRE(a->count<vn::LevelUp>() == 1);
    const auto stats = a->all<vn::CharStats>();
    REQUIRE(stats.size() == 1);
    checkCharStats(stats[0], vg::deriveStats(1, 2, {6, 4}), 2);
    CHECK(stats[0].unspent == 3);
    // Order: LEVEL_UP, then CHAR_STATS.
    const auto pa = a->packets();
    const auto at = [&](vn::Opcode op) {
        return std::find_if(pa.begin(), pa.end(), [&](const auto& p) { return p.opcode == static_cast<std::uint16_t>(op); }) -
               pa.begin();
    };
    CHECK(at(vn::Opcode::LevelUp) < at(vn::Opcode::CharStats));
}

TEST_CASE("Stats zone: dexterity makes monster attacks miss (COMBAT_EVENT kind Miss, 0 damage)", "[server][world][stats]")
{
    Harness h(monsters(spawnAt(1, 140.0f, 100.0f), 8.0f, 30.0f, 11));
    // A rogue with a stored allocation far beyond what its level gives (the zone trusts the stored
    // values): Çeviklik 213 -> dodge capped at 25 %.
    auto a = h.enter(1, 138.5f, 100.0f, 2, 10, {8, 0}, std::nullopt, {0, 200, 0, 0, 0});
    auto watcher = h.enter(2, 130.0f, 104.0f);
    h.step();
    REQUIRE(h.combat(1).stats.dodgePermille == vg::kMaxDodgePermille);
    // Lots of hp: survive long enough for a good sample.
    std::uint32_t lastHp = h.combat(1).hp;
    int misses = 0, hits = 0;
    for (int round = 0; round < 12; ++round)
    {
        h.zone->setVitalsForTesting(1, h.combat(1).stats.maxHp, 0);
        a->clear();
        h.stepFor(15000ms);
        lastHp = h.combat(1).stats.maxHp;
        for (const auto& e : a->all<vn::CombatEvent>())
        {
            if (e.targetId != 1)
                continue;
            if (e.kind == vn::kCombatKindMiss)
            {
                ++misses;
                CHECK(e.amount == 0);
                CHECK(e.flags == 0);
                CHECK(e.skillId == 0);
                CHECK(e.targetHp == lastHp); // unchanged
            }
            else
            {
                REQUIRE(e.kind == vn::kCombatKindDamage);
                ++hits;
                CHECK(e.amount > 0);
                lastHp = e.targetHp;
            }
        }
        if (h.combat(1).dead)
            break;
    }
    INFO("misses " << misses << " hits " << hits);
    REQUIRE(misses + hits >= 80);
    const double rate = static_cast<double>(misses) / (misses + hits);
    CHECK((rate > 0.15 && rate < 0.35)); // 25 %
    CHECK(h.zone->counters().dodges == static_cast<std::uint64_t>(misses));
    // Observers see the misses too.
    CHECK(filter<vn::CombatEvent>(*watcher, [](const vn::CombatEvent& e) { return e.kind == vn::kCombatKindMiss; })
              .size() > 0);
    // A player without Çeviklik never dodges.
    CHECK(vg::deriveStats(1, 1, {}, {}).dodgeChance > 0.0f); // (a warrior still has 6 Çeviklik: 2.4 %)
}

TEST_CASE("Stats zone: the heal skill scales with the caster's intelligence and spirit, item bonuses count",
          "[server][world][stats]")
{
    Harness h(monsters(""));
    // A priest with 27 points in İrade at level 10: multiplier 1 + (8 + 37) / 60 = 1.75.
    auto priest = h.enter(1, 130.0f, 100.0f, 4, 10, {5, 0}, std::nullopt, {0, 0, 0, 0, 27});
    auto tank = h.enter(2, 132.0f, 100.0f); // a level-1 warrior, 90 hp
    h.step();
    CHECK_THAT(h.combat(1).stats.healMultiplier, Catch::Matchers::WithinAbs(1.75, 1e-6));
    h.zone->setVitalsForTesting(2, 10, 34);
    h.cmd(1, K::Skill, 2, vg::kSkillPaleSun);
    h.stepFor(900ms);
    const auto heals = filter<vn::CombatEvent>(*tank, [](const vn::CombatEvent& e) { return e.kind == vn::kCombatKindHeal; });
    REQUIRE(heals.size() == 1);
    CHECK(heals[0].amount == 30); // round(90 x 0.19 x 1.75) = 29.9
    // Equipped items with attribute bonuses: counted in the stats, reported apart in CHAR_STATS.
    priest->clear();
    vg::EquipBonus mace{7, 1};
    mace.attributes = {0, 0, 0, 0, 2}; // Cüruf Topuzu: İrade +2
    h.zone->setEquipment(1, 14, 0, mace);
    h.step();
    const auto stats = priest->all<vn::CharStats>();
    REQUIRE(stats.size() == 1);
    CHECK(stats[0].attributes[4].base == 37);
    CHECK(stats[0].attributes[4].bonus == 2);
    checkCharStats(stats[0], vg::deriveStats(4, 10, {0, 0, 0, 0, 27}, mace), 10);
}
