// Zones and gates in the world (docs/PROTOCOL_ZONES.md, server/world/World + Zone): three synthetic
// zones with fake links and a fake clock. Isolation between zones, the gate trigger and its re-arm
// rules, refusals of the other race's homeland and their rate limit, the change sequence (old zone's
// batch, ZONE_CHANGE_BEGIN, the arrival stored before the player appears, the entry packets), the
// loading protection, a failed write, leave / duplicate login / stop during a change, effects and
// cooldowns carried, placement by race and home zone, and the startup checks of the layout.

#include "server/world/World.h"
#include "tests/server/world/FakeLink.h"
#include "tests/server/world/TestTerrain.h"
#include "tests/server/world/ZoneTestWorld.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace vw = vo::server::world;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using vo::tests::FakeLink;

namespace {

constexpr std::uint8_t kAydor = 1;
constexpr std::uint8_t kKargan = 2;

struct Saves
{
    struct Entry
    {
        va::CharacterId character;
        va::CharacterPosition position;
    };
    std::mutex mutex;
    std::vector<Entry> entries;
    bool complete = true; // call done at once; false: keep it in `pending`
    bool ok = true;       // what done reports
    std::vector<std::function<void(bool)>> pending;

    vw::SavePositionFn fn()
    {
        return [this](va::AccountId, va::CharacterId c, va::CharacterPosition p, va::CharacterProgress,
                      std::function<void(bool)> done) {
            bool result = true;
            {
                const std::scoped_lock lock(mutex);
                entries.push_back({c, p});
                result = ok;
                if (!complete)
                {
                    pending.push_back(std::move(done));
                    return;
                }
            }
            if (done)
                done(result);
        };
    }
    void finishAll(bool result)
    {
        std::vector<std::function<void(bool)>> list;
        {
            const std::scoped_lock lock(mutex);
            list.swap(pending);
        }
        for (auto& d : list)
            if (d)
                d(result);
    }
    std::vector<Entry> of(va::CharacterId c)
    {
        const std::scoped_lock lock(mutex);
        std::vector<Entry> out;
        for (const auto& e : entries)
            if (e.character == c)
                out.push_back(e);
        return out;
    }
};

struct Harness
{
    Saves saves;
    vw::World world;
    vo::core::MonoTime t0 = vo::core::monoNow();
    int tickNo = 0;
    std::map<vw::SessionKey, std::shared_ptr<FakeLink>> links;
    std::map<vw::SessionKey, std::uint32_t> seqs;
    std::map<vw::SessionKey, std::pair<float, float>> pos;

    explicit Harness(bool homesInZone1 = false) : world(settings(), saves.fn(), vo::core::monoNow())
    {
        vo::tests::addThreeZones(world, homesInZone1);
    }
    static vw::WorldSettings settings()
    {
        vw::WorldSettings s;
        s.zone.seed = 1301;
        s.saveInterval = std::chrono::seconds(0); // only the saves of this milestone's events
        return s;
    }
    [[nodiscard]] vo::core::MonoTime now() const { return t0 + tickNo * 50ms; }
    void tick(int n = 1)
    {
        for (int i = 0; i < n; ++i)
        {
            world.tick(now(), now());
            ++tickNo;
        }
    }

    std::shared_ptr<FakeLink> enter(vw::SessionKey session, va::CharacterId character, std::uint8_t race,
                                    std::uint16_t zone, float x, float z, std::uint8_t cls = 2, vg::SkillRanks ranks = {})
    {
        auto link = std::make_shared<FakeLink>();
        links[session] = link;
        vw::EnterCommand cmd;
        cmd.session = session;
        cmd.generation = 1;
        cmd.link = link;
        cmd.account = character + 1000;
        cmd.character.id = character;
        cmd.character.name = "Char" + std::to_string(character);
        cmd.character.race = race;
        cmd.character.charClass = cls;
        cmd.character.level = 5;
        cmd.character.position = {zone, x, 0.0f, z, 0.0f};
        cmd.character.skillRanks = ranks;
        world.post(std::move(cmd));
        tick();
        REQUIRE(link->entered());
        pos[session] = {link->entered()->x, link->entered()->z};
        return link;
    }
    [[nodiscard]] vw::EntityId entity(vw::SessionKey s) const { return links.at(s)->entered()->entityId; }

    void moveTo(vw::SessionKey s, float x, float z, std::uint8_t flags = vg::kMoveMoving | vg::kMoveRunning)
    {
        vn::MoveReq r;
        r.seq = ++seqs[s];
        r.flags = flags;
        r.x = x;
        r.z = z;
        r.y = 0.0f;
        world.post(vw::MoveCommand{s, r, now()});
        pos[s] = {x, z};
    }
    // Walks in steps of at most 2 m, 8 ticks (0.4 s) apart, ending with a stop; stops early if a
    // ZONE_CHANGE_BEGIN arrives.
    void walk(vw::SessionKey s, float x, float z)
    {
        const std::size_t begins = links.at(s)->begins().size();
        for (int guard = 0; guard < 400; ++guard)
        {
            const auto [cx, cz] = pos[s];
            const float dx = x - cx, dz = z - cz;
            const float d = std::hypot(dx, dz);
            if (d < 0.01f)
                break;
            const float step = std::min(d, 2.0f);
            moveTo(s, cx + dx / d * step, cz + dz / d * step);
            for (int t = 0; t < 8; ++t)
            {
                tick();
                if (links.at(s)->begins().size() != begins)
                    return; // right after the tick of ZONE_CHANGE_BEGIN
            }
        }
        moveTo(s, x, z, 0);
        tick();
    }
    void arrived(vw::SessionKey s)
    {
        const auto e = links.at(s)->entered();
        REQUIRE(e);
        pos[s] = {e->x, e->z};
    }
    void ready(vw::SessionKey s)
    {
        world.post(vw::ZoneReadyCommand{s});
        tick();
    }
    template <class T>
    std::size_t count(vw::SessionKey s)
    {
        return links.at(s)->count<T>();
    }
};

float dist(float ax, float az, float bx, float bz)
{
    return std::hypot(ax - bx, az - bz);
}

} // namespace

TEST_CASE("Zones: an Aydor walks through gate_aydor into Tanyeri (sequence, storage first, isolation)",
          "[server][world][zones]")
{
    Harness h;
    auto a = h.enter(1, 11, kAydor, 1, 114.0f, 100.0f); // 4 m east of gate_aydor
    auto b = h.enter(2, 12, kAydor, 1, 120.0f, 100.0f); // an observer in Eşik
    auto c = h.enter(3, 13, kAydor, 2, 116.0f, 104.0f); // somebody in Tanyeri near arrive_esik
    const vw::EntityId oldA = h.entity(1);
    CHECK(a->visible().contains(h.entity(2)));
    CHECK_FALSE(a->visible().contains(h.entity(3))); // another zone: never replicated
    CHECK_FALSE(c->visible().contains(oldA));
    h.tick(45); // > 2 s: armed

    h.saves.complete = false; // the arrival's write stays in flight until finishAll()
    h.walk(1, 111.0f, 100.0f);
    REQUIRE(a->begins().size() == 1);
    CHECK(a->begins()[0].fromZone == 1);
    CHECK(a->begins()[0].toZone == 2);
    CHECK(h.world.inTransit(1));
    CHECK(h.world.zoneIdOf(1) == 0);
    CHECK(h.world.playerCount() == 3);
    // The observer in Eşik saw it go.
    const auto despawns = b->all<vn::EntityDespawn>();
    REQUIRE_FALSE(despawns.empty());
    CHECK(despawns.back().entityId == oldA);
    CHECK(b->errors().empty());
    // §3.5: the arrival (zone 2 near arrive_esik) is being written; the player is not in a zone yet.
    const auto writes = h.saves.of(11);
    REQUIRE_FALSE(writes.empty());
    CHECK(writes.back().position.zoneId == 2);
    CHECK(dist(writes.back().position.x, writes.back().position.z, 118.0f, 100.0f) <= 2.05f);
    h.tick(10);
    CHECK(a->arrivals() == 0);
    // Moves the client sent before it knew are dropped by the world (no zone): nothing comes back.
    h.moveTo(1, 112.0f, 100.0f);
    h.tick();
    CHECK(a->count<vn::PositionCorrect>() == 0);

    h.saves.complete = true;
    a->clear();
    h.saves.finishAll(true);
    h.tick();
    REQUIRE(a->arrivals() == 1);
    const auto e = *a->entered();
    CHECK(e.zoneChange);
    CHECK(e.zoneChangeResult == vn::ZoneChangeResult::Ok);
    CHECK(e.fromZone == 1);
    CHECK(e.zoneId == 2);
    CHECK(e.entityId != oldA);
    CHECK(dist(e.x, e.z, writes.back().position.x, writes.back().position.z) < 0.01f); // placed where it was stored
    CHECK(dist(e.x, e.z, 110.0f, 100.0f) > vo::tests::kGateRadius);                   // outside gate_esik
    CHECK(h.world.zoneIdOf(1) == 2);
    CHECK_FALSE(h.world.inTransit(1));
    // The entry packets: ZONE_CHANGE first, CHAR_STATS before any spawn, the Tanyeri player spawned.
    const auto packets = a->packets();
    REQUIRE(packets.size() >= 3);
    CHECK(packets[0].opcode == static_cast<std::uint16_t>(vn::Opcode::ZoneChange));
    CHECK(packets[1].opcode == static_cast<std::uint16_t>(vn::Opcode::CharStats));
    CHECK(a->visible() == std::set<std::uint32_t>{h.entity(3)});
    CHECK(c->visible().contains(e.entityId));
    CHECK(a->errors().empty());
    CHECK(c->errors().empty());

    // Loading: moves are ignored (no correction), gates do not fire, nothing of Eşik arrives.
    a->clear();
    h.moveTo(1, 150.0f, 150.0f);
    h.tick(5);
    CHECK(a->count<vn::PositionCorrect>() == 0);
    h.links[1] = a;
    h.pos[1] = {e.x, e.z};
    h.ready(1);
    h.walk(2, 125.0f, 100.0f); // B walks around in Eşik
    for (const auto& m : a->all<vn::EntityMove>())
        CHECK(m.entityId != h.entity(2));
    CHECK(h.world.zoneChanges() == 1);
}

TEST_CASE("Zones: re-arm after arriving and after logging in inside a gate", "[server][world][zones]")
{
    Harness h;
    auto a = h.enter(1, 21, kAydor, 1, 114.0f, 100.0f);
    h.tick(45);
    h.walk(1, 111.0f, 100.0f);
    REQUIRE(a->begins().size() == 1);
    h.tick(3);
    REQUIRE(a->arrivals() == 1);
    h.arrived(1);
    h.ready(1);
    // Straight back into gate_esik: inside before the 2 s re-arm is over -> nothing yet ...
    h.walk(1, 111.5f, 100.0f);
    const int insideAt = h.tickNo;
    CHECK(a->begins().size() == 1);
    // ... and it fires once the 2 s are over, standing inside (no extra key press or step).
    h.tick(40);
    REQUIRE(a->begins().size() == 2);
    CHECK(a->begins()[1].fromZone == 2);
    CHECK(a->begins()[1].toZone == 1);
    CHECK(h.tickNo - insideAt <= 40);
    h.tick(3);
    REQUIRE(a->arrivals() == 2);
    CHECK(a->entered()->zoneId == 1);
    CHECK(dist(a->entered()->x, a->entered()->z, 118.0f, 100.0f) <= 2.05f); // arrive_aydor
    CHECK(dist(a->entered()->x, a->entered()->z, 110.0f, 100.0f) > vo::tests::kGateRadius);
    // Standing at the arrival point fires nothing (it lies outside gate_aydor).
    h.arrived(1);
    h.ready(1);
    h.tick(100);
    CHECK(a->begins().size() == 2);

    // A character that logs in standing inside a gate must step out first.
    auto b = h.enter(2, 22, kAydor, 1, 110.5f, 100.0f);
    CHECK(h.world.zoneIdOf(2) == 1);
    h.tick(80);
    CHECK(b->begins().empty());
    h.walk(2, 114.0f, 100.0f);
    CHECK(b->begins().empty());
    h.walk(2, 111.0f, 100.0f);
    CHECK(b->begins().size() == 1);
}

TEST_CASE("Zones: the other race's homeland refuses, once per stay and at most every 3 s",
          "[server][world][zones]")
{
    Harness h;
    auto k = h.enter(1, 31, kKargan, 1, 114.0f, 100.0f);
    h.tick(45);
    h.walk(1, 111.0f, 100.0f); // into gate_aydor
    const auto first = k->all<vn::ZoneGateRefused>();
    REQUIRE(first.size() == 1);
    CHECK(first[0].toZone == 2);
    CHECK(first[0].reason == vn::GateRefusal::RaceForbidden);
    CHECK(k->begins().empty());
    // Standing in the gate does not spam.
    h.tick(200);
    CHECK(k->count<vn::ZoneGateRefused>() == 1);
    CHECK(h.world.zoneIdOf(1) == 1);
    // Out and straight back in: the next one waits for the 3 s since the last.
    h.walk(1, 114.0f, 100.0f);
    const int lastRefusalTick = h.tickNo;
    (void)lastRefusalTick;
    h.walk(1, 111.0f, 100.0f);
    CHECK(k->count<vn::ZoneGateRefused>() == 2); // > 3 s passed while standing inside above
    h.walk(1, 113.0f, 100.0f);                   // out (2 m from the edge) ...
    h.walk(1, 111.0f, 100.0f);                   // ... and in again within 3 s
    CHECK(k->count<vn::ZoneGateRefused>() == 2);
    h.tick(60); // still inside when the 3 s are over: the refusal of this stay follows
    CHECK(k->count<vn::ZoneGateRefused>() == 3);
    h.tick(100);
    CHECK(k->count<vn::ZoneGateRefused>() == 3);
    CHECK(k->begins().empty());

    // Its own gate works: Eşik -> Demirağız.
    auto k2 = h.enter(2, 32, kKargan, 1, 386.0f, 400.0f);
    h.tick(45);
    h.walk(2, 389.0f, 400.0f);
    REQUIRE(k2->begins().size() == 1);
    CHECK(k2->begins()[0].toZone == 3);
    h.tick(3);
    REQUIRE(k2->arrivals() == 1);
    CHECK(k2->entered()->zoneId == 3);
    CHECK(dist(k2->entered()->x, k2->entered()->z, 382.0f, 400.0f) <= 2.05f);
}

TEST_CASE("Zones: dead players never fire a gate", "[server][world][zones]")
{
    Harness h;
    auto a = h.enter(1, 41, kAydor, 1, 114.0f, 100.0f);
    h.tick(45);
    vw::Zone* z1 = h.world.zone(1);
    REQUIRE(z1);
    z1->killPlayerForTesting(1);
    z1->placeEntity(1, 111.0f, 100.0f); // a corpse inside the gate (e.g. killed while standing there)
    h.tick(100);
    CHECK(a->begins().empty());
    CHECK(a->count<vn::ZoneGateRefused>() == 0);
    // The respawn puts it at its race spawn, outside the gate.
    h.world.post(vw::CombatCommand{1, vw::CombatCommand::Kind::Respawn, 0, 0, 0});
    h.tick(2);
    REQUIRE(a->count<vn::RespawnRes>() == 1);
    CHECK(h.world.zoneIdOf(1) == 1);
    const auto at = z1->positionOf(1);
    REQUIRE(at);
    CHECK(dist(at->x, at->z, 100.0f, 100.0f) <= 2.05f);
    h.tick(45);
    CHECK(a->begins().empty());
    // Alive it fires.
    h.pos[1] = {at->x, at->z};
    h.walk(1, 111.0f, 100.0f);
    CHECK(a->begins().size() == 1);
}

TEST_CASE("Zones: a failed write puts the player back where it stood", "[server][world][zones]")
{
    Harness h;
    auto a = h.enter(1, 51, kAydor, 1, 114.0f, 100.0f);
    h.tick(45);
    h.saves.ok = false;
    h.walk(1, 111.0f, 100.0f);
    REQUIRE(a->begins().size() == 1);
    h.tick(2);
    REQUIRE(a->arrivals() == 1);
    const auto e = *a->entered();
    CHECK(e.zoneChangeResult == vn::ZoneChangeResult::Failed);
    CHECK(e.zoneId == 1);
    CHECK(e.fromZone == 1);
    CHECK(dist(e.x, e.z, 110.0f, 100.0f) <= vo::tests::kGateRadius); // inside the gate, where it stood
    CHECK(h.world.zoneIdOf(1) == 1);
    h.saves.ok = true;
    h.arrived(1);
    h.ready(1);
    // It stands inside the gate, disarmed until it steps out.
    h.tick(100);
    CHECK(a->begins().size() == 1);
    h.walk(1, 114.0f, 100.0f);
    h.walk(1, 111.0f, 100.0f);
    REQUIRE(a->begins().size() == 2);
    h.tick(2);
    CHECK(a->entered()->zoneId == 2);
    CHECK(a->entered()->zoneChangeResult == vn::ZoneChangeResult::Ok);
}

TEST_CASE("Zones: leaving, a duplicate login and stop() while the arrival is being written",
          "[server][world][zones]")
{
    SECTION("leave waits for the write; the next entry is at the arrival")
    {
        Harness h;
        auto a = h.enter(1, 61, kAydor, 1, 114.0f, 100.0f);
        h.tick(45);
        h.saves.complete = false;
        h.walk(1, 111.0f, 100.0f);
        REQUIRE(h.world.inTransit(1));
        bool doneCalled = false, doneOk = false;
        h.world.post(vw::LeaveCommand{1, [&](bool ok) {
                                          doneCalled = true;
                                          doneOk = ok;
                                      },
                                      {}});
        h.tick(3);
        CHECK_FALSE(doneCalled);
        h.saves.complete = true;
        h.saves.finishAll(true);
        h.tick();
        CHECK(doneCalled);
        CHECK(doneOk);
        CHECK(a->arrivals() == 0);
        CHECK(h.world.playerCount() == 0);
        // Recently left: the next entry (stored position not landed in this fake) uses the arrival.
        auto again = h.enter(2, 61, kAydor, 1, 114.0f, 100.0f);
        CHECK(again->entered()->zoneId == 2);
        CHECK(dist(again->entered()->x, again->entered()->z, 118.0f, 100.0f) <= 2.05f);
    }
    SECTION("leave and a failed write: stored where it stood")
    {
        Harness h;
        auto a = h.enter(1, 62, kAydor, 1, 114.0f, 100.0f);
        h.tick(45);
        h.saves.complete = false;
        h.walk(1, 111.0f, 100.0f);
        bool doneCalled = false;
        h.world.post(vw::LeaveCommand{1, [&](bool) { doneCalled = true; }, {}});
        h.tick();
        h.saves.complete = true;
        h.saves.finishAll(false);
        h.tick();
        CHECK(doneCalled);
        const auto writes = h.saves.of(62);
        REQUIRE(writes.size() >= 2);
        CHECK(writes.back().position.zoneId == 1);
        CHECK(dist(writes.back().position.x, writes.back().position.z, 110.0f, 100.0f) <= vo::tests::kGateRadius);
    }
    SECTION("a duplicate login takes over the arrival")
    {
        Harness h;
        auto a = h.enter(1, 63, kAydor, 1, 114.0f, 100.0f);
        h.tick(45);
        h.saves.complete = false;
        h.walk(1, 111.0f, 100.0f);
        REQUIRE(h.world.inTransit(1));
        auto b = h.enter(2, 63, kAydor, 1, 114.0f, 100.0f); // same character, new session
        CHECK(b->entered()->zoneId == 2);
        CHECK_FALSE(b->entered()->zoneChange);
        CHECK_FALSE(h.world.inTransit(1));
        h.saves.complete = true;
        h.saves.finishAll(true); // the old change's completion finds nothing
        h.tick();
        CHECK(a->arrivals() == 0);
        bool doneCalled = false;
        h.world.post(vw::LeaveCommand{1, [&](bool) { doneCalled = true; }, {}});
        h.tick();
        CHECK(doneCalled);
        CHECK(h.world.zoneIdOf(2) == 2);
    }
    SECTION("stop() saves the arrival again and ends the leave")
    {
        Harness h;
        (void)h.enter(1, 64, kAydor, 1, 114.0f, 100.0f);
        h.tick(45);
        h.saves.complete = false;
        h.walk(1, 111.0f, 100.0f);
        REQUIRE(h.world.inTransit(1));
        const auto before = h.saves.of(64).size();
        h.saves.complete = true;
        h.world.stop();
        const auto writes = h.saves.of(64);
        REQUIRE(writes.size() == before + 1);
        CHECK(writes.back().position.zoneId == 2);
    }
}

TEST_CASE("Zones: effects and cooldowns come along, the player's own effects re-sourced",
          "[server][world][zones]")
{
    Harness h;
    vg::SkillRanks ranks{};
    ranks[8] = 1; // Kalkan Duvarı (warrior, self: defense up 12 s, cooldown 25 s)
    auto w = h.enter(1, 71, kAydor, 1, 114.0f, 100.0f, 1, ranks);
    h.tick(45);
    h.world.post(vw::CombatCommand{1, vw::CombatCommand::Kind::Skill, 0, 8, 0});
    h.tick();
    REQUIRE(w->count<vn::EffectApply>() == 1);
    REQUIRE(w->count<vn::SkillCooldowns>() >= 1);
    h.walk(1, 111.0f, 100.0f);
    REQUIRE(w->begins().size() == 1);
    w->clear();
    h.tick(2);
    REQUIRE(w->arrivals() == 1);
    const vw::EntityId self = w->entered()->entityId;
    // After CHAR_STATS: SKILL_COOLDOWNS (Kalkan Duvarı still running), EFFECT_LIST about itself.
    const auto cds = w->all<vn::SkillCooldowns>();
    REQUIRE(cds.size() == 1);
    REQUIRE(cds[0].entries.size() == 1);
    CHECK(cds[0].entries[0].skillId == 8);
    CHECK(cds[0].entries[0].remainingMs < 25000);
    CHECK(cds[0].entries[0].remainingMs > 15000);
    const auto lists = w->all<vn::EffectList>();
    REQUIRE(lists.size() == 1);
    CHECK(lists[0].entityId == self);
    REQUIRE(lists[0].effects.size() == 1);
    CHECK(lists[0].effects[0].skillId == 8);
    CHECK(lists[0].effects[0].sourceId == self);
    CHECK(lists[0].effects[0].remainingMs < 12000);
    CHECK(w->errors().empty());
    const auto packets = w->packets();
    REQUIRE(packets.size() >= 4);
    CHECK(packets[1].opcode == static_cast<std::uint16_t>(vn::Opcode::CharStats));
    CHECK(packets[2].opcode == static_cast<std::uint16_t>(vn::Opcode::SkillCooldowns));
    CHECK(packets[3].opcode == static_cast<std::uint16_t>(vn::Opcode::EffectList));
    // The buff still counts in the new zone (its CHAR_STATS has the raised defense) and expires there.
    h.tick(12000 / 50);
    CHECK(w->count<vn::EffectRemove>() == 1);
}

TEST_CASE("Zones: placement by race and home zone", "[server][world][zones]")
{
    Harness h;
    // An Aydor saved in Tanyeri stays there; a Kargan saved there goes to Demirağız's spawn.
    auto a = h.enter(1, 81, kAydor, 2, 150.0f, 150.0f);
    CHECK(a->entered()->zoneId == 2);
    CHECK(a->entered()->x == 150.0f);
    auto k = h.enter(2, 82, kKargan, 2, 150.0f, 150.0f);
    CHECK(k->entered()->zoneId == 3);
    CHECK(dist(k->entered()->x, k->entered()->z, 400.0f, 400.0f) <= 2.05f);
    // An unknown zone, or an unusable position: the home zone's race spawn.
    auto u = h.enter(3, 83, kAydor, 9, 150.0f, 150.0f);
    CHECK(u->entered()->zoneId == 2);
    CHECK(dist(u->entered()->x, u->entered()->z, 100.0f, 100.0f) <= 2.05f);
    auto off = h.enter(4, 84, kKargan, 1, -5.0f, 600.0f);
    CHECK(off->entered()->zoneId == 3);
    // Existing characters in Eşik keep their place.
    auto old = h.enter(5, 85, kKargan, 1, 200.0f, 220.0f);
    CHECK(old->entered()->zoneId == 1);
    CHECK(old->entered()->x == 200.0f);
    // New characters: the race spawns of the home zones.
    const auto spawns = h.world.newCharacterSpawns();
    CHECK(spawns.aydor.zoneId == 2);
    CHECK(spawns.aydor.x == 100.0f);
    CHECK(spawns.kargan.zoneId == 3);
    CHECK(spawns.kargan.x == 400.0f);
    CHECK(h.world.homeZone(kAydor)->id() == 2);
    CHECK(h.world.homeZone(kKargan)->id() == 3);
    // The override (world.start_zone_override = 1): both start in Eşik.
    Harness o(true);
    const auto s1 = o.world.newCharacterSpawns();
    CHECK(s1.aydor.zoneId == 1);
    CHECK(s1.kargan.zoneId == 1);
    CHECK(s1.kargan.x == 400.0f);
    // A zone's race spawn falls back to the other race's for a race that may not be there.
    CHECK(h.world.zone(2)->raceSpawn(kKargan).x == 100.0f);
    CHECK_FALSE(h.world.zone(2)->allowsRace(kKargan));
    CHECK(h.world.zone(1)->allowsRace(kKargan));
}

TEST_CASE("Zones: respawn in the zone the player died in", "[server][world][zones]")
{
    // RESPAWN_REQ uses the race spawn of the player's zone: Tanyeri's spawn_aydor for an Aydor that
    // died in its homeland, Ortaocak's race spawns in Eşik (PROTOCOL_ZONES.md §4).
    Harness h;
    CHECK(h.world.zone(2)->raceSpawn(kAydor).zoneId == 2);
    CHECK(h.world.zone(2)->raceSpawn(kAydor).x == 100.0f);
    CHECK(h.world.zone(1)->raceSpawn(kAydor).zoneId == 1);
    CHECK(h.world.zone(1)->raceSpawn(kKargan).x == 400.0f);
    CHECK(h.world.zone(3)->raceSpawn(kKargan).zoneId == 3);
}

TEST_CASE("Zones: the startup checks of the layout", "[server][world][zones]")
{
    const auto fresh = [] {
        vw::WorldSettings s;
        return std::make_unique<vw::World>(s, nullptr);
    };
    std::string error;
    SECTION("a zone without the spawn of a race it allows")
    {
        auto w = fresh();
        auto t = vo::tests::zoneTerrain(2);
        std::erase_if(t.spawns, [](const auto& s) { return s.name == "spawn_aydor"; });
        CHECK_FALSE(w->createZone(2, t, error, vo::tests::zoneLayout(2)));
        CHECK_THAT(error, ContainsSubstring("spawn_aydor"));
    }
    SECTION("a zone that allows only Kargan needs no spawn_aydor")
    {
        auto w = fresh();
        auto t = vo::tests::zoneTerrain(3);
        std::erase_if(t.spawns, [](const auto& s) { return s.name == "spawn_aydor"; });
        CHECK(w->createZone(3, t, error, vo::tests::zoneLayout(3)));
    }
    SECTION("a missing or unwalkable gate marker")
    {
        auto w = fresh();
        auto t = vo::tests::zoneTerrain(1);
        std::erase_if(t.spawns, [](const auto& s) { return s.name == "gate_kargan"; });
        CHECK_FALSE(w->createZone(1, t, error, vo::tests::zoneLayout(1)));
        CHECK_THAT(error, ContainsSubstring("gate_kargan"));
        auto t2 = vo::tests::zoneTerrain(1);
        vo::tests::blockCell(t2, 110.0f, 100.0f);
        CHECK_FALSE(w->createZone(1, t2, error, vo::tests::zoneLayout(1)));
        CHECK_THAT(error, ContainsSubstring("not walkable"));
    }
    SECTION("a gate to a zone that is not loaded")
    {
        auto w = fresh();
        REQUIRE(w->createZone(1, vo::tests::zoneTerrain(1), error, vo::tests::zoneLayout(1)));
        REQUIRE(w->createZone(2, vo::tests::zoneTerrain(2), error, vo::tests::zoneLayout(2)));
        CHECK_FALSE(w->finalizeZones(2, 0, error));
        CHECK_THAT(error, ContainsSubstring("zone 3"));
    }
    SECTION("an arrival marker missing, unwalkable or too close to a gate")
    {
        for (int variant = 0; variant < 3; ++variant)
        {
            auto w = fresh();
            auto t2 = vo::tests::zoneTerrain(2);
            if (variant == 0)
                std::erase_if(t2.spawns, [](const auto& s) { return s.name == "arrive_esik"; });
            else if (variant == 1)
                vo::tests::blockCell(t2, 118.0f, 100.0f);
            else
                for (auto& s : t2.spawns)
                    if (s.name == "arrive_esik")
                        s.position.x = 113.4f; // 3.4 m from gate_esik: not more than 2.5 + 1
            REQUIRE(w->createZone(1, vo::tests::zoneTerrain(1), error, vo::tests::zoneLayout(1)));
            REQUIRE(w->createZone(2, t2, error, vo::tests::zoneLayout(2)));
            REQUIRE(w->createZone(3, vo::tests::zoneTerrain(3), error, vo::tests::zoneLayout(3)));
            INFO(variant);
            CHECK_FALSE(w->finalizeZones(2, 3, error));
            CHECK_THAT(error, ContainsSubstring("arrive_esik"));
        }
    }
    SECTION("a home zone that does not allow the race")
    {
        auto w = fresh();
        for (std::uint16_t id = 1; id <= 3; ++id)
            REQUIRE(w->createZone(id, vo::tests::zoneTerrain(id), error, vo::tests::zoneLayout(id)));
        CHECK_FALSE(w->finalizeZones(3, 3, error));
        CHECK_THAT(error, ContainsSubstring("does not allow race 1"));
        CHECK(w->finalizeZones(2, 3, error));
        CHECK(w->finalizeZones(0, 0, error)); // 0 = the start zone (1), which allows both
    }
}

TEST_CASE("Zones: many players commuting keep the AOI invariants per zone", "[server][world][zones]")
{
    Harness h;
    std::vector<std::shared_ptr<FakeLink>> links;
    for (int i = 0; i < 12; ++i)
        links.push_back(h.enter(100 + i, 900 + i, kAydor, 1, 114.0f + static_cast<float>(i % 4), 96.0f + static_cast<float>(i)));
    h.tick(45);
    // Everybody walks into gate_aydor, arrives in Tanyeri, walks back out through gate_esik.
    for (int i = 0; i < 12; ++i)
        h.walk(100 + i, 111.0f, 100.0f);
    h.tick(3);
    for (int i = 0; i < 12; ++i)
    {
        INFO(i);
        REQUIRE(links[i]->arrivals() == 1);
        CHECK(links[i]->entered()->zoneId == 2);
        h.arrived(100 + i);
        h.ready(100 + i);
        CHECK(links[i]->errors().empty());
    }
    // In Tanyeri they all see each other (the arrival point is one AOI area), nobody from Eşik.
    for (int i = 0; i < 12; ++i)
        CHECK(links[i]->visible().size() == 11);
    h.tick(45);
    for (int i = 0; i < 6; ++i)
        h.walk(100 + i, 111.0f, 100.0f);
    h.tick(3);
    // Those who went back see each other in Eşik; those who stayed see only those who stayed.
    for (int i = 0; i < 12; ++i)
    {
        INFO(i);
        CHECK(links[i]->entered()->zoneId == (i < 6 ? 1 : 2));
        CHECK(links[i]->visible().size() == 5);
        CHECK(links[i]->errors().empty());
    }
}
