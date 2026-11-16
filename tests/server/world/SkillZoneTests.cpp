// Skills and timed effects in the zone (server/world/ZoneSkills.cpp, docs/PROTOCOL_SKILLS.md §1, §3.6,
// §3.7) on synthetic terrain with a fake clock and fake links: ranks (NotLearned, rank-scaled
// numbers), area targeting (around self / around the target, max targets, nearest first, NoTarget,
// safe zone), damage over time and its expiry, heal over time, buffs changing the stats, max hp up,
// absorb shields, stun, slow, weaken, taunt, the leap (landing point, NoPath), effects shown on
// entering the area of interest, death and leash return clearing effects, cooldown reports and their
// memory across a leave.

#include "server/world/Zone.h"
#include "shared/game/Combat.h"
#include "shared/game/Monsters.h"
#include "shared/game/Skills.h"
#include "tests/server/world/FakeLink.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <string>

namespace vw = vo::server::world;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using vo::tests::FakeLink;
using K = vw::CombatCommand::Kind;
using R = vn::ActionFailReason;
using EK = vg::EffectKind;

namespace {

// Monster 1: hits back (attack configurable, 1 s interval); 2: a dummy (lots of hp, attack 0, never
// aggressive); a safe zone around the Aydor spawn (100, 100).
std::shared_ptr<const vg::MonsterData> monsters(const std::string& spawns, std::uint32_t attack = 30,
                                                std::uint32_t hp = 400)
{
    const std::string toml = std::format(R"(
[[monster]]
id = 1
name = "Kül Çömezi"
model = "ash_thrall"
level = 2
hp = {}
attack = {}
defense = 4
attack_range = 2.0
attack_interval_ms = 1000
move_speed = 4.0
wander_speed = 1.0
aggro_radius = 0.0
leash_radius = 30.0
xp = 25
respawn_ms = 10000

[[monster]]
id = 2
name = "Talim Kuklası"
model = "ash_thrall"
level = 1
hp = 50000
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

[[safe_zone]]
zone = 1
x = 100.0
z = 100.0
radius = 20.0
{}
)",
                                         hp, attack, spawns);
    auto data = std::make_shared<vg::MonsterData>();
    std::string error;
    INFO(error);
    REQUIRE(vg::parseMonsterData(toml, "test.toml", *data, error));
    return data;
}

std::string spawnAt(int monster, float x, float z)
{
    return std::format("\n[[spawn]]\nzone = 1\nmonster = {}\nx = {}\nz = {}\nradius = 0.0\n", monster, x, z);
}

vg::SkillRanks ranks(std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> list)
{
    vg::SkillRanks r{};
    for (const auto& [id, rank] : list)
        r[id] = rank;
    return r;
}

struct Harness
{
    std::unique_ptr<vw::Zone> zone;
    std::map<vw::SessionKey, std::shared_ptr<FakeLink>> links;
    vo::core::MonoTime t0 = vo::core::monoNow();
    std::uint64_t tick = 0;
    vw::EntityId nextMonsterId = 1000;

    explicit Harness(std::shared_ptr<const vg::MonsterData> data, vo::assets::TerrainData terrain = vo::tests::makeTestTerrain())
    {
        vw::ZoneSettings s;
        s.savePeriodTicks = 0;
        s.seed = 99;
        std::string error;
        zone = vw::Zone::create(1, std::move(terrain), s, error);
        REQUIRE(zone);
        zone->beginTick(now(), ms());
        REQUIRE(zone->installMonsters(std::move(data), [this] { return nextMonsterId++; }, now(), error));
    }

    [[nodiscard]] vo::core::MonoTime now() const { return t0 + tick * 50ms; }
    [[nodiscard]] std::uint32_t ms() const { return static_cast<std::uint32_t>(tick * 50); }

    std::shared_ptr<FakeLink> enter(vw::SessionKey s, float x, float z, std::uint8_t cls, vg::SkillRanks r = {},
                                    std::uint16_t level = 10, std::vector<vw::Zone::RememberedCooldown> cds = {})
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
        p.character.position = {1, x, 0.0f, z, 0.0f};
        p.character.skillRanks = r;
        p.equip = {10, 4};
        p.cooldowns = std::move(cds);
        zone->beginTick(now(), ms());
        zone->enter(std::move(p), now(), ms());
        return link;
    }

    void cmd(vw::SessionKey s, K kind, std::uint32_t target = 0, std::uint16_t skill = 0)
    {
        zone->beginTick(now(), ms());
        zone->combat(vw::CombatCommand{s, kind, target, skill, 0});
    }
    void skill(vw::SessionKey s, std::uint16_t id, std::uint32_t target = 0) { cmd(s, K::Skill, target, id); }

    void step(int n = 1)
    {
        for (int i = 0; i < n; ++i)
        {
            zone->beginTick(now(), ms());
            zone->update(now(), tick, [](va::AccountId, va::CharacterId, va::CharacterPosition, va::CharacterProgress,
                                         std::function<void(bool)> done) {
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
    void checkLinks() const
    {
        for (const auto& [s, link] : links)
            if (zone->findPlayer(s))
            {
                INFO("session " << s);
                CHECK(link->errors().empty());
                for (const auto& e : link->errors())
                    UNSCOPED_INFO(e);
            }
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

std::vector<vn::CombatEvent> damageBy(const FakeLink& link, vw::EntityId source, std::uint16_t skill)
{
    return filter<vn::CombatEvent>(link, [&](const vn::CombatEvent& e) {
        return e.sourceId == source && e.skillId == skill && e.kind == vn::kCombatKindDamage;
    });
}

bool hasFail(const FakeLink& link, std::uint16_t skill, R reason)
{
    const auto f = link.all<vn::ActionFail>();
    return std::any_of(f.begin(), f.end(), [&](const vn::ActionFail& a) { return a.skillId == skill && a.reason == reason; });
}

bool effectOn(const std::vector<vw::Zone::ActiveEffect>* list, EK kind)
{
    return list && std::any_of(list->begin(), list->end(), [&](const auto& e) { return e.kind == kind; });
}

} // namespace

TEST_CASE("Skill zone: rank 0 is NotLearned, the free skill always works, ranks scale the numbers",
          "[server][world][skills]")
{
    Harness h(monsters(spawnAt(2, 140.0f, 100.0f)));
    const auto dummy = h.monsterIds()[0];
    auto a = h.enter(1, 138.0f, 100.0f, 1); // a warrior with no stored ranks
    h.step();
    h.skill(1, 5, 0); // Yarma, not learned
    h.skill(1, 2, dummy); // a rogue skill: UnknownSkill (as v3)
    h.skill(1, 1, dummy); // Ağır Darbe: the free skill at rank 1
    h.step();
    CHECK(hasFail(*a, 5, R::NotLearned));
    CHECK(hasFail(*a, 2, R::UnknownSkill));
    auto r1 = damageBy(*a, 1, 1);
    REQUIRE(r1.size() == 1);
    // SKILL_USED and the cooldown report come with it.
    const auto used = a->all<vn::SkillUsed>();
    REQUIRE(used.size() == 1);
    CHECK(used[0] == vn::SkillUsed{1, 1, dummy, 1});
    const auto cds = a->all<vn::SkillCooldowns>();
    REQUIRE(cds.size() == 1);
    CHECK(cds[0].entries == std::vector<vn::SkillCooldownEntry>{{1, 6000, 6000}});

    // Rank 3 hits harder (x2.8 instead of x2.2): over many uses the average ratio is ~1.27.
    Harness h3(monsters(spawnAt(2, 140.0f, 100.0f)));
    const auto d3 = h3.monsterIds()[0];
    auto b = h3.enter(1, 138.0f, 100.0f, 1, ranks({{1, 3}}));
    Harness h1(monsters(spawnAt(2, 140.0f, 100.0f)));
    const auto d1 = h1.monsterIds()[0];
    auto c = h1.enter(1, 138.0f, 100.0f, 1);
    for (int i = 0; i < 30; ++i)
    {
        h3.zone->setVitalsForTesting(1, 100000, 100000); // full mana for every use
        h1.zone->setVitalsForTesting(1, 100000, 100000);
        h3.skill(1, 1, d3);
        h1.skill(1, 1, d1);
        h3.stepFor(6050ms);
        h1.stepFor(6050ms);
    }
    double s3 = 0, s1 = 0;
    for (const auto& e : damageBy(*b, 1, 1))
        s3 += e.amount;
    for (const auto& e : damageBy(*c, 1, 1))
        s1 += e.amount;
    REQUIRE(damageBy(*b, 1, 1).size() == 30);
    CHECK(s3 / s1 > 1.15);
    CHECK(s3 / s1 < 1.40);
    CHECK(b->all<vn::SkillUsed>().back().rank == 3);
    h.checkLinks();
}

TEST_CASE("Skill zone: around-self area - nearest first, max targets, NoTarget, safe zone", "[server][world][skills]")
{
    // Seven dummies around (200, 200): six within 3 m, one at 6 m. Yarma hits at most five.
    std::string spawns;
    const float ring[6][2] = {{1.0f, 0.0f}, {0.0f, 1.5f}, {-2.0f, 0.0f}, {0.0f, -2.5f}, {2.4f, 1.0f}, {-1.0f, -2.6f}};
    for (const auto& p : ring)
        spawns += spawnAt(2, 200.0f + p[0], 200.0f + p[1]);
    spawns += spawnAt(2, 206.0f, 200.0f);
    Harness h(monsters(spawns));
    auto a = h.enter(1, 200.0f, 200.0f, 1, ranks({{1, 3}, {5, 1}}));
    h.step();
    h.skill(1, 5);
    h.step();
    const auto hits = damageBy(*a, 1, 5);
    REQUIRE(hits.size() == 5);
    std::set<std::uint32_t> hitIds;
    for (const auto& e : hits)
        hitIds.insert(e.targetId);
    // The farthest of the ring (2.79 m) and the one at 6 m are spared.
    const auto ids = h.monsterIds();
    CHECK_FALSE(hitIds.contains(ids[5]));
    CHECK_FALSE(hitIds.contains(ids[6]));
    CHECK(a->all<vn::SkillUsed>().at(0).targetId == 1); // around self: the caster

    // Nobody near: NoTarget, no cost, no cooldown.
    Harness e(monsters(spawnAt(2, 250.0f, 250.0f)));
    auto b = e.enter(1, 200.0f, 200.0f, 1, ranks({{1, 3}, {5, 1}}));
    e.step();
    const auto mp = e.combat(1).mp;
    e.skill(1, 5);
    e.step();
    CHECK(hasFail(*b, 5, R::NoTarget));
    CHECK(e.combat(1).mp == mp);
    CHECK(b->count<vn::SkillCooldowns>() == 0);
    // In the safe zone: InSafeZone.
    Harness s(monsters(spawnAt(2, 125.0f, 100.0f)));
    auto c = s.enter(1, 105.0f, 100.0f, 1, ranks({{1, 3}, {5, 1}}));
    s.step();
    s.skill(1, 5);
    s.step();
    CHECK(hasFail(*c, 5, R::InSafeZone));
    h.checkLinks();
}

TEST_CASE("Skill zone: around-target area slows everyone near the target, observers see the effects",
          "[server][world][skills][effects]")
{
    Harness h(monsters(spawnAt(2, 210.0f, 200.0f) + spawnAt(2, 212.0f, 201.0f) + spawnAt(2, 220.0f, 200.0f)));
    const auto ids = h.monsterIds();
    auto a = h.enter(1, 202.0f, 200.0f, 2, ranks({{14, 3}, {15, 2}})); // a rogue: Sis Bombası rank 2
    auto o = h.enter(2, 204.0f, 205.0f, 1);                             // an observer
    h.step();
    h.skill(1, 15, ids[0]);
    h.step();
    // The primary and the one 2.2 m from it; not the one 10 m away.
    CHECK(effectOn(h.zone->effectsOf(ids[0]), EK::Slow));
    CHECK(effectOn(h.zone->effectsOf(ids[1]), EK::Slow));
    CHECK_FALSE(effectOn(h.zone->effectsOf(ids[2]), EK::Slow));
    const auto applies = o->all<vn::EffectApply>();
    REQUIRE(applies.size() == 2);
    CHECK(applies[0].effect.skillId == 15);
    CHECK(applies[0].effect.kind == static_cast<std::uint8_t>(EK::Slow));
    CHECK(applies[0].effect.amount == 40); // rank 2: 40 %
    CHECK(applies[0].effect.durationMs == 6000);
    CHECK(applies[0].effect.sourceId == 1);
    CHECK(o->effects(ids[0]).size() == 1);
    // Expiry after 6 s: EFFECT_REMOVE(Expired) to the observers.
    h.stepFor(6100ms);
    CHECK_FALSE(effectOn(h.zone->effectsOf(ids[0]), EK::Slow));
    const auto removes = o->all<vn::EffectRemove>();
    REQUIRE(removes.size() == 2);
    CHECK(removes[0].reason == vn::EffectRemoveReason::Expired);
    CHECK(o->effects(ids[0]).empty());
    h.checkLinks();
}

TEST_CASE("Skill zone: damage over time ticks every second, then expires; a DoT kill gives experience",
          "[server][world][skills][effects]")
{
    Harness h(monsters(spawnAt(2, 140.0f, 100.0f)));
    const auto dummy = h.monsterIds()[0];
    auto a = h.enter(1, 130.0f, 100.0f, 3, ranks({{3, 3}, {17, 1}})); // a mage: Yakıcı İz rank 1
    h.step();
    const auto attack = h.combat(1).stats.attack;
    h.skill(1, 17, dummy);
    h.step();
    CHECK(damageBy(*a, 1, 17).empty()); // no direct hit
    h.stepFor(8100ms);
    const auto ticks = damageBy(*a, 1, 17);
    REQUIRE(ticks.size() == 8);
    for (const auto& t : ticks)
    {
        CHECK((t.flags & vn::kCombatFlagPeriodic) != 0);
        CHECK(t.amount == vg::periodicDamage(attack, vg::findSkill(17)->effects.at(0).value[0]));
    }
    CHECK(a->all<vn::EffectRemove>().size() == 1);
    CHECK(h.zone->effectsOf(dummy)->empty());

    // A weak monster dies of the DoT: killing blow flag, death, experience for the caster.
    Harness k(monsters(spawnAt(1, 140.0f, 100.0f), 0, 30));
    const auto weak = k.monsterIds()[0];
    auto b = k.enter(1, 130.0f, 100.0f, 3, ranks({{3, 1}, {17, 3}}), 1);
    k.step();
    k.skill(1, 17, weak);
    k.stepFor(8100ms);
    const auto last = damageBy(*b, 1, 17);
    REQUIRE_FALSE(last.empty());
    CHECK((last.back().flags & vn::kCombatFlagKillingBlow) != 0);
    CHECK(b->count<vn::EntityDeath>() == 1);
    CHECK(b->count<vn::XpGain>() == 1);
    CHECK(k.monster(weak).effects.empty());
    k.checkLinks();
}

TEST_CASE("Skill zone: buffs change the stats and expire; max hp up raises hp, expiry clamps it",
          "[server][world][skills][effects]")
{
    Harness h(monsters(""));
    auto a = h.enter(1, 200.0f, 200.0f, 1, ranks({{1, 3}, {5, 3}, {6, 2}}));
    h.step();
    const auto base = h.combat(1).stats;
    h.skill(1, 6); // Kan Öfkesi rank 2: +20 % attack for 12 s
    h.step();
    CHECK(h.combat(1).stats.attack == static_cast<std::uint16_t>(std::lround(base.attack * 1.2)));
    CHECK(a->all<vn::SelfStats>().back().attack == h.combat(1).stats.attack);
    CHECK(a->all<vn::CharStats>().back().attack == h.combat(1).stats.attack);
    h.stepFor(12100ms);
    CHECK(h.combat(1).stats.attack == base.attack);
    CHECK(a->all<vn::CharStats>().back().attack == base.attack);

    // A priest's Kutlu Siper on another player: +30 % defense, +15 % max hp, hp raised by the gain.
    auto p = h.enter(2, 202.0f, 200.0f, 4, ranks({{4, 3}, {23, 3}, {24, 2}}));
    h.step();
    const auto before = h.combat(1);
    h.skill(2, 24, 1);
    h.step();
    const auto& after = h.combat(1);
    CHECK(after.stats.maxHp == static_cast<std::uint32_t>(std::lround(before.stats.maxHp * 1.15)));
    CHECK(after.hp == before.hp + (after.stats.maxHp - before.stats.maxHp));
    CHECK(after.stats.defense == static_cast<std::uint16_t>(std::lround(before.stats.defense * 1.3)));
    const auto own = filter<vn::EntityStats>(*a, [](const vn::EntityStats& s) { return s.entityId == 1; });
    REQUIRE_FALSE(own.empty());
    CHECK(own.back().maxHp == after.stats.maxHp); // observers' bars
    h.stepFor(20100ms);
    CHECK(h.combat(1).stats.maxHp == before.stats.maxHp);
    CHECK(h.combat(1).hp <= before.stats.maxHp);
    h.checkLinks();
}

TEST_CASE("Skill zone: heal over time on an ally, ring heal around self", "[server][world][skills][effects]")
{
    Harness h(monsters(""));
    auto p = h.enter(1, 200.0f, 200.0f, 4, ranks({{4, 3}, {23, 2}, {24, 1}, {25, 1}}));
    auto w = h.enter(2, 203.0f, 200.0f, 1);
    auto far = h.enter(3, 220.0f, 200.0f, 1);
    h.step();
    h.zone->setVitalsForTesting(2, 50, 10);
    h.zone->setVitalsForTesting(3, 50, 10);
    h.skill(1, 23, 2); // Yenileyen Işık rank 2 on the warrior: 5 % per second for 10 s
    h.step();
    h.stepFor(10100ms);
    const auto heals = filter<vn::CombatEvent>(*w, [](const vn::CombatEvent& e) {
        return e.skillId == 23 && e.kind == vn::kCombatKindHeal && (e.flags & vn::kCombatFlagPeriodic) != 0;
    });
    CHECK(heals.size() == 10);
    CHECK(h.combat(2).hp > 50);
    // Halka Şifası: the caster and the warrior 3 m away, not the one 20 m away.
    h.zone->setVitalsForTesting(1, 60, 200);
    h.zone->setVitalsForTesting(2, 60, 10);
    h.skill(1, 25);
    h.step();
    CHECK(h.combat(1).hp > 60);
    CHECK(h.combat(2).hp > 60);
    CHECK(filter<vn::CombatEvent>(*far, [](const vn::CombatEvent& e) { return e.skillId == 25 && e.targetId == 3; }).empty());
    h.checkLinks();
}

TEST_CASE("Skill zone: an absorb shield takes damage first, then is depleted", "[server][world][skills][effects]")
{
    Harness h(monsters(spawnAt(1, 201.5f, 200.0f), 60, 50000));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 200.0f, 200.0f, 3, ranks({{3, 1}, {20, 3}, {21, 1}}));
    h.step();
    const auto attack = h.combat(1).stats.attack;
    h.skill(1, 21); // Buz Kabuğu rank 1: 150 % of the magic power
    h.step();
    const auto shields = a->all<vn::EffectApply>();
    REQUIRE(shields.size() == 1);
    CHECK(shields[0].effect.amount == vg::shieldAmount(attack, 150.0f));
    h.cmd(1, K::Attack, m); // pull it
    h.stepFor(1500ms);
    const auto hits = filter<vn::CombatEvent>(*a, [&](const vn::CombatEvent& e) {
        return e.sourceId == m && e.kind == vn::kCombatKindDamage;
    });
    REQUIRE_FALSE(hits.empty());
    CHECK((hits[0].flags & vn::kCombatFlagAbsorbed) != 0);
    h.stepFor(6000ms);
    const auto removes = a->all<vn::EffectRemove>();
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].reason == vn::EffectRemoveReason::Depleted);
    CHECK_FALSE(effectOn(h.zone->effectsOf(1), EK::Shield));
    h.checkLinks();
}

TEST_CASE("Skill zone: stun stops a monster's attacks, weaken lowers its damage", "[server][world][skills][effects]")
{
    Harness h(monsters(spawnAt(1, 201.5f, 200.0f), 40, 50000));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 200.0f, 200.0f, 1, ranks({{1, 3}, {5, 2}, {6, 0}, {7, 1}}));
    h.step();
    h.cmd(1, K::Attack, m);
    h.stepFor(3000ms);
    h.skill(1, 7, m); // Sarsıntı rank 1: stun 1.5 s
    h.step();
    REQUIRE(effectOn(h.zone->effectsOf(m), EK::Stun));
    const auto during = [&] {
        return filter<vn::CombatEvent>(*a, [&](const vn::CombatEvent& e) { return e.sourceId == m; }).size();
    };
    const auto at = during();
    h.stepFor(1300ms);
    CHECK(during() == at); // no attacks while stunned
    h.stepFor(1500ms);
    CHECK(during() > at); // attacks again
    CHECK_FALSE(effectOn(h.zone->effectsOf(m), EK::Stun));

    // Weaken (Bağlayan Söz, 30 %): the average hit drops by about that much.
    Harness w(monsters(spawnAt(1, 201.5f, 200.0f), 30, 50000));
    const auto wm = w.monsterIds()[0];
    auto pr = w.enter(1, 200.0f, 200.0f, 4, ranks({{4, 1}, {26, 3}, {27, 3}}));
    w.step();
    w.cmd(1, K::Attack, wm);
    double plain = 0, weak = 0;
    int nPlain = 0, nWeak = 0;
    const auto hits = [&] {
        return filter<vn::CombatEvent>(*pr, [&](const vn::CombatEvent& e) {
            return e.sourceId == wm && e.kind == vn::kCombatKindDamage && (e.flags & vn::kCombatFlagCritical) == 0;
        });
    };
    const auto sum = [&](std::size_t from, std::size_t to, double& into, int& n) {
        const auto all = hits();
        for (std::size_t i = from; i < to && i < all.size(); ++i)
        {
            into += all[i].amount;
            ++n;
        }
    };
    w.stepFor(2000ms); // it is in melee now
    for (int round = 0; round < 6; ++round)
    {
        w.zone->setVitalsForTesting(1, 100000, 1000);
        // 8 s of weaken (retried while a swing's global cooldown blocks it).
        const auto usedBefore = pr->count<vn::SkillUsed>();
        for (int tries = 0; tries < 20 && pr->count<vn::SkillUsed>() == usedBefore; ++tries)
        {
            w.skill(1, 27, wm);
            w.step();
        }
        REQUIRE(pr->count<vn::SkillUsed>() == usedBefore + 1);
        const std::size_t a0 = hits().size();
        for (int k = 0; k < 15; ++k)
        {
            w.zone->setVitalsForTesting(1, 100000, 100000);
            w.stepFor(500ms);
        }
        const std::size_t a1 = hits().size();
        sum(a0, a1, weak, nWeak);
        w.stepFor(800ms); // past the end of the weaken
        const std::size_t b0 = hits().size();
        for (int k = 0; k < 13; ++k)
        {
            w.zone->setVitalsForTesting(1, 100000, 100000);
            w.stepFor(500ms); // the 15 s cooldown runs out
        }
        w.stepFor(250ms);
        sum(b0, hits().size(), plain, nPlain);
    }    REQUIRE(nWeak > 10);
    REQUIRE(nPlain > 10);
    const double ratio = (weak / nWeak) / (plain / nPlain);
    INFO("weakened " << weak / nWeak << " plain " << plain / nPlain);
    // (0.7 A - D / 2) / (A - D / 2) with the priest's defense (PROTOCOL_SKILLS.md §1.3).
    const double d = w.combat(1).stats.defense * 0.5;
    const double expected = (0.7 * 30.0 - d) / (30.0 - d);
    CHECK(std::abs(ratio - expected) < 0.1);
    h.checkLinks();
    w.checkLinks();
}

TEST_CASE("Skill zone: taunt makes monsters attack the warrior", "[server][world][skills][effects]")
{
    Harness h(monsters(spawnAt(1, 202.0f, 200.0f) + spawnAt(1, 199.0f, 203.0f), 5, 50000));
    const auto ids = h.monsterIds();
    auto mage = h.enter(1, 200.0f, 200.0f, 3, ranks({{3, 3}}));
    auto war = h.enter(2, 201.0f, 201.0f, 1, ranks({{8, 3}, {9, 1}}));
    h.step();
    // The mage hits both: they chase the mage.
    h.skill(1, 3, ids[0]);
    h.stepFor(3800ms); // Köz Oku: 3 s cooldown from the end of its 0.6 s cast
    h.skill(1, 3, ids[1]);
    h.stepFor(700ms);
    REQUIRE(h.monster(ids[0]).target == 1);
    REQUIRE(h.monster(ids[1]).target == 1);
    h.skill(2, 9); // Meydan Okuma: both within 6 m
    h.step(2);
    CHECK(h.monster(ids[0]).target == 2);
    CHECK(h.monster(ids[1]).target == 2);
    CHECK(effectOn(h.zone->effectsOf(ids[0]), EK::Taunt));
    // After the taunt the warrior still has the highest threat (top + 10 %).
    h.stepFor(4100ms);
    CHECK_FALSE(effectOn(h.zone->effectsOf(ids[0]), EK::Taunt));
    CHECK(h.monster(ids[0]).target == 2);
    h.checkLinks();
}

TEST_CASE("Skill zone: the leap lands next to the target, NoPath when blocked", "[server][world][skills]")
{
    Harness h(monsters(spawnAt(2, 210.0f, 200.0f)));
    const auto dummy = h.monsterIds()[0];
    auto a = h.enter(1, 200.0f, 200.0f, 2, ranks({{2, 3}, {11, 3}, {12, 1}}));
    auto o = h.enter(2, 195.0f, 205.0f, 1);
    h.step();
    h.skill(1, 12, dummy); // Gölge Adımı rank 1: 10 m
    h.step();
    const auto leaps = a->all<vn::SkillLeap>();
    REQUIRE(leaps.size() == 1);
    CHECK(std::abs(leaps[0].x - 208.5f) < 0.05f); // 1.5 m before the target
    CHECK(std::abs(leaps[0].z - 200.0f) < 0.05f);
    CHECK(o->count<vn::SkillLeap>() == 1);
    CHECK(a->count<vn::PositionCorrect>() == 1);
    CHECK(std::abs(h.zone->findPlayer(1)->move.x - 208.5f) < 0.05f);
    CHECK(damageBy(*a, 1, 12).size() == 1);
    // SKILL_LEAP comes before SKILL_USED, which comes before the hit.
    const auto packets = a->packets();
    const auto pos = [&](vn::Opcode op) {
        return std::find_if(packets.begin(), packets.end(), [&](const auto& p) { return p.opcode == static_cast<std::uint16_t>(op); }) -
               packets.begin();
    };
    CHECK(pos(vn::Opcode::SkillLeap) < pos(vn::Opcode::SkillUsed));
    CHECK(pos(vn::Opcode::SkillUsed) < pos(vn::Opcode::CombatEvent));

    // A wall between: no landing point, NoPath, nothing paid.
    auto terrain = vo::tests::makeTestTerrain();
    for (float z = 190.0f; z <= 210.0f; z += 0.5f)
        vo::tests::blockCell(terrain, 205.0f, z);
    Harness b(monsters(spawnAt(2, 210.0f, 200.0f)), std::move(terrain));
    const auto d2 = b.monsterIds()[0];
    auto r = b.enter(1, 200.0f, 200.0f, 2, ranks({{2, 3}, {11, 3}, {12, 1}}));
    b.step();
    const auto mp = b.combat(1).mp;
    b.skill(1, 12, d2);
    b.step();
    CHECK(hasFail(*r, 12, R::NoPath));
    CHECK(b.combat(1).mp == mp);
    CHECK(r->count<vn::SkillLeap>() == 0);
    // Out of range (12 m with rank 1's 10 m): OutOfRange.
    Harness c(monsters(spawnAt(2, 212.5f, 200.0f)));
    const auto d3 = c.monsterIds()[0];
    auto q = c.enter(1, 200.0f, 200.0f, 2, ranks({{2, 3}, {11, 3}, {12, 1}}));
    c.step();
    c.skill(1, 12, d3);
    c.step();
    CHECK(hasFail(*q, 12, R::OutOfRange));
    h.checkLinks();
}

TEST_CASE("Skill zone: entering the area of interest shows the effects; death and leash return clear them",
          "[server][world][skills][effects]")
{
    Harness h(monsters(spawnAt(1, 210.0f, 200.0f), 0, 5000));
    const auto m = h.monsterIds()[0];
    auto a = h.enter(1, 200.0f, 200.0f, 2, ranks({{2, 1}, {14, 3}}));
    h.step();
    h.zone->placeEntity(1, 208.5f, 200.0f);
    h.skill(1, 14, m); // Zehirli Kesik: hit + dot
    h.step();
    REQUIRE(effectOn(h.zone->effectsOf(m), EK::Dot));
    // A newcomer gets ENTITY_SPAWN of the monster followed by its EFFECT_LIST.
    auto b = h.enter(2, 205.0f, 205.0f, 1);
    h.step();
    const auto packets = b->packets();
    const auto spawnAt_ = std::find_if(packets.begin(), packets.end(), [&](const auto& p) {
        vn::EntitySpawn s;
        return p.opcode == static_cast<std::uint16_t>(vn::Opcode::EntitySpawn) && vn::decodeMessage(p.payload, s) &&
               s.entityId == m;
    });
    REQUIRE(spawnAt_ != packets.end());
    REQUIRE(std::next(spawnAt_) != packets.end());
    CHECK(std::next(spawnAt_)->opcode == static_cast<std::uint16_t>(vn::Opcode::EffectList));
    CHECK(b->effects(m).size() == 1);
    // The monster walks home when its target leaves the leash: Cleared.
    h.zone->placeEntity(1, 300.0f, 300.0f);
    h.zone->placeEntity(2, 300.0f, 302.0f);
    h.stepFor(500ms);
    CHECK(h.monster(m).effects.empty());
    // A player with a buff dies: its effects are gone (no EFFECT_REMOVE).
    Harness d(monsters(spawnAt(1, 201.5f, 200.0f), 500, 50000));
    const auto killer = d.monsterIds()[0];
    auto c = d.enter(1, 200.0f, 200.0f, 2, ranks({{2, 1}, {14, 3}, {15, 3}, {16, 1}}));
    d.step();
    d.skill(1, 16); // Kaçamak
    d.step();
    REQUIRE(effectOn(d.zone->effectsOf(1), EK::DodgeUp));
    d.cmd(1, K::Attack, killer);
    d.stepFor(5000ms);
    REQUIRE(d.combat(1).dead);
    CHECK(d.combat(1).effects.empty());
    CHECK(c->count<vn::EffectRemove>() == 0);
    CHECK(c->effects(1).empty()); // cleared by ENTITY_DEATH
    h.checkLinks();
    d.checkLinks();
}

TEST_CASE("Skill zone: cooldowns are reported and remembered across a leave", "[server][world][skills]")
{
    Harness h(monsters(""));
    auto a = h.enter(1, 200.0f, 200.0f, 1, ranks({{1, 3}, {8, 1}}));
    h.step();
    h.skill(1, 8); // Kalkan Duvarı: 25 s
    h.step();
    h.skill(1, 8);
    h.step();
    const auto f = a->all<vn::ActionFail>();
    REQUIRE(f.size() == 1);
    CHECK(f[0].reason == R::OnCooldown);
    h.stepFor(5000ms);
    const auto left = h.zone->leave(1);
    REQUIRE(left);
    REQUIRE(left->cooldowns.size() == 1);
    CHECK(left->cooldowns[0].skill == 8);
    CHECK(left->cooldowns[0].totalMs == 25000);
    // Back 2 s later: SKILL_COOLDOWNS on entry with ~18 s left, still on cooldown.
    h.stepFor(2000ms);
    auto b = h.enter(1, 200.0f, 200.0f, 1, ranks({{1, 3}, {8, 1}}), 10, left->cooldowns);
    h.step();
    const auto cds = b->all<vn::SkillCooldowns>();
    REQUIRE(cds.size() == 1);
    REQUIRE(cds[0].entries.size() == 1);
    CHECK(cds[0].entries[0].skillId == 8);
    CHECK(cds[0].entries[0].totalMs == 25000);
    CHECK(cds[0].entries[0].remainingMs <= 18000);
    CHECK(cds[0].entries[0].remainingMs >= 17800);
    // CHAR_STATS first, then SKILL_COOLDOWNS (PROTOCOL_SKILLS.md §4).
    const auto packets = b->packets();
    CHECK(packets[1].opcode == static_cast<std::uint16_t>(vn::Opcode::CharStats));
    CHECK(packets[2].opcode == static_cast<std::uint16_t>(vn::Opcode::SkillCooldowns));
    h.skill(1, 8);
    h.step();
    CHECK(b->all<vn::ActionFail>().at(0).reason == R::OnCooldown);
    h.checkLinks();
}

TEST_CASE("Skill zone: new ranks from the session apply from the next command", "[server][world][skills]")
{
    Harness h(monsters(""));
    auto a = h.enter(1, 200.0f, 200.0f, 1);
    h.step();
    h.skill(1, 8);
    h.step();
    CHECK(a->all<vn::ActionFail>().at(0).reason == R::NotLearned);
    h.zone->setSkillRanks(1, ranks({{8, 1}}));
    h.skill(1, 8);
    h.step();
    CHECK(effectOn(h.zone->effectsOf(1), EK::DefenseUp));
    CHECK(h.combat(1).skillRanks[1] == 1); // the free skill stays
}
