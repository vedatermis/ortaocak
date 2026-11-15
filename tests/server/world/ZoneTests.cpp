// Zone (server/world/Zone): placement on enter, ENTER/SPAWN ordering, the AOI grid against a
// brute-force oracle under random walks (symmetric, never duplicated), ENTITY_MOVE relay limited to
// 10 Hz per entity with the final state always delivered, POSITION_CORRECT, leave, staggered saves.
// Everything runs on synthetic terrain with fake links: no sockets, no threads.

#include "server/world/AoiGrid.h"
#include "server/world/Zone.h"
#include "tests/server/world/FakeLink.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <set>

namespace vw = vo::server::world;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using vo::tests::FakeLink;

namespace {

vw::ZoneSettings testSettings()
{
    vw::ZoneSettings s;
    s.savePeriodTicks = 0;
    s.seed = 1234;
    return s;
}

std::unique_ptr<vw::Zone> makeZone(vw::ZoneSettings s = testSettings(), vo::assets::TerrainData t = vo::tests::makeTestTerrain())
{
    std::string error;
    auto z = vw::Zone::create(1, std::move(t), s, error);
    REQUIRE(z);
    return z;
}

struct Harness
{
    std::unique_ptr<vw::Zone> zone;
    std::map<vw::SessionKey, std::shared_ptr<FakeLink>> links;
    vo::core::MonoTime t0 = vo::core::monoNow();
    std::uint64_t tick = 0;
    std::map<vw::SessionKey, std::uint32_t> seq;

    explicit Harness(vw::ZoneSettings s = testSettings()) : zone(makeZone(s)) {}

    [[nodiscard]] vo::core::MonoTime now() const { return t0 + tick * 50ms; }

    std::shared_ptr<FakeLink> enter(vw::SessionKey s, float x, float z, std::uint8_t race = 1, std::uint16_t zoneId = 1)
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
        p.character.race = race;
        p.character.charClass = 1;
        p.character.level = 1;
        p.character.position = {zoneId, x, 0.0f, z, 0.0f};
        zone->enter(std::move(p), now(), static_cast<std::uint32_t>(tick * 50));
        return link;
    }

    std::optional<vw::MoveVerdict> move(vw::SessionKey s, float x, float z, std::uint8_t flags = vg::kMoveMoving | vg::kMoveRunning,
                                        vo::core::MonoClock::duration offset = 0ms)
    {
        vn::MoveReq r;
        r.seq = ++seq[s];
        r.flags = flags;
        r.x = x;
        r.z = z;
        r.y = zone->terrain().heightAt(x, z);
        return zone->move(s, r, now() + offset, static_cast<std::uint32_t>(tick * 50));
    }

    // One tick: update (corrections, relays, saves) + flush, then advance the logical clock.
    void step(const vw::SavePositionFn& save = nullptr)
    {
        zone->update(now(), tick, save);
        zone->flush();
        ++tick;
    }

    [[nodiscard]] const std::unordered_set<vw::EntityId>& known(vw::SessionKey s) const
    {
        return zone->findPlayer(s)->known;
    }
};

std::vector<vn::EntityMove> movesOf(const FakeLink& link, vw::EntityId id)
{
    std::vector<vn::EntityMove> out;
    for (const auto& m : link.all<vn::EntityMove>())
        if (m.entityId == id)
            out.push_back(m);
    return out;
}

} // namespace

// ---- AoiGrid ----

TEST_CASE("AoiGrid: cells, view block, view changes", "[server][world][aoi]")
{
    vw::AoiGrid g(512.0f, 32.0f, 2);
    CHECK(g.cellsPerSide() == 16);
    CHECK(g.cellOf(0.0f, 0.0f) == vw::AoiCell{0, 0});
    CHECK(g.cellOf(31.99f, 32.0f) == vw::AoiCell{0, 1});
    CHECK(g.cellOf(511.9f, 600.0f) == vw::AoiCell{15, 15}); // clamped
    CHECK(g.cellOf(-5.0f, NAN) == vw::AoiCell{0, 0});
    CHECK(g.inView({5, 5}, {7, 3}));
    CHECK_FALSE(g.inView({5, 5}, {8, 5}));
    CHECK(g.inView({7, 3}, {5, 5}) == g.inView({5, 5}, {7, 3}));

    for (int i = 0; i < 16; ++i)
        g.add(static_cast<vw::EntityId>(100 + i), {i, i});
    CHECK(g.population() == 16);
    std::set<vw::EntityId> seen;
    g.forEachInView({5, 5}, [&](vw::EntityId id) { seen.insert(id); });
    CHECK(seen == std::set<vw::EntityId>{103, 104, 105, 106, 107});
    std::set<vw::EntityId> left, entered;
    g.forEachViewChange({5, 5}, {6, 6}, [&](vw::EntityId id) { left.insert(id); },
                        [&](vw::EntityId id) { entered.insert(id); });
    CHECK(left == std::set<vw::EntityId>{103});
    CHECK(entered == std::set<vw::EntityId>{108});
    g.move(108, {8, 8}, {0, 0});
    g.remove(100, {0, 0});
    CHECK(g.at({0, 0}) == std::vector<vw::EntityId>{108});
    CHECK(g.population() == 15);
}

// ---- Placement and ordering ----

TEST_CASE("Zone: placement on enter", "[server][world][zone]")
{
    Harness h;
    const auto& t = h.zone->terrain();
    SECTION("valid saved position is kept, y snapped")
    {
        auto link = h.enter(1, 250.0f, 260.0f);
        h.step();
        const auto e = link->entered();
        REQUIRE(e);
        CHECK(e->x == 250.0f);
        CHECK(e->z == 260.0f);
        CHECK(e->y == t.heightAt(250.0f, 260.0f));
        CHECK(e->zoneId == 1);
        CHECK(e->walkSpeed == vg::kWalkSpeed);
        CHECK(e->runSpeed == vg::kRunSpeed);
        CHECK(e->entityId == 1);
    }
    SECTION("unknown zone, out of bounds, blocked or non-finite saved positions use the race spawn")
    {
        auto blocked = vo::tests::makeTestTerrain();
        vo::tests::blockCell(blocked, 300.5f, 300.5f);
        Harness b;
        b.zone = makeZone(testSettings(), blocked);
        const auto check = [&](vw::SessionKey s, float x, float z, std::uint8_t race, std::uint16_t zoneId,
                               float sx, float sz) {
            auto link = b.enter(s, x, z, race, zoneId);
            b.step();
            const auto e = link->entered();
            REQUIRE(e);
            const float d = std::hypot(e->x - sx, e->z - sz);
            CHECK(d <= 2.0f + 1e-4f); // spread around the spawn
            CHECK(b.zone->terrain().isWalkable(e->x, e->z));
        };
        check(1, 250.0f, 250.0f, 1, 7, 100.0f, 100.0f);
        check(2, -3.0f, 20.0f, 1, 1, 100.0f, 100.0f);
        check(3, 300.5f, 300.5f, 2, 1, 400.0f, 400.0f);
        check(4, NAN, 20.0f, 2, 1, 400.0f, 400.0f);
    }
    SECTION("players at the spawn point are spread, not stacked")
    {
        std::set<std::pair<float, float>> spots;
        for (vw::SessionKey s = 1; s <= 20; ++s)
        {
            auto link = h.enter(s, 100.0f, 100.0f); // never moved since creation = the spawn point
            h.step();
            const auto e = link->entered();
            REQUIRE(e);
            const float d = std::hypot(e->x - 100.0f, e->z - 100.0f);
            CHECK(d >= 0.5f - 1e-4f);
            CHECK(d <= 2.0f + 1e-4f);
            spots.insert({e->x, e->z});
        }
        CHECK(spots.size() == 20);
    }
    SECTION("raceSpawn")
    {
        CHECK(h.zone->raceSpawn(1).x == 100.0f);
        CHECK(h.zone->raceSpawn(2).z == 400.0f);
        CHECK(h.zone->raceSpawn(9).x == 100.0f);
    }
}

TEST_CASE("Zone: create refuses terrain without usable race spawns", "[server][world][zone]")
{
    std::string error;
    auto t = vo::tests::makeTestTerrain();
    t.spawns.pop_back();
    CHECK_FALSE(vw::Zone::create(1, t, testSettings(), error));
    CHECK(error.find("spawn_kargan") != std::string::npos);
    auto u = vo::tests::makeTestTerrain();
    vo::tests::blockCell(u, 100.0f, 100.0f);
    CHECK_FALSE(vw::Zone::create(1, u, testSettings(), error));
    CHECK(error.find("not walkable") != std::string::npos);
    CHECK_FALSE(vw::Zone::load(1, "does/not/exist.vterrain", testSettings(), error));
}

TEST_CASE("Zone: ENTER first, then ENTITY_SPAWN of everyone in range; observers get the newcomer", "[server][world][zone]")
{
    Harness h;
    auto a = h.enter(1, 250.0f, 250.0f);
    h.step();
    auto b = h.enter(2, 270.0f, 255.0f);
    auto far = h.enter(3, 480.0f, 30.0f);
    h.step();
    const auto pb = b->packets();
    REQUIRE(pb.size() == 2);
    CHECK(pb[0].opcode == static_cast<std::uint16_t>(vn::Opcode::EnterWorldRes));
    CHECK(pb[1].opcode == static_cast<std::uint16_t>(vn::Opcode::EntitySpawn));
    CHECK(pb[0].batch == pb[1].batch); // one batch, in order
    const auto spawnsForB = b->all<vn::EntitySpawn>();
    REQUIRE(spawnsForB.size() == 1);
    CHECK(spawnsForB[0].entityId == 1);
    CHECK(spawnsForB[0].name == "Player1");
    CHECK(spawnsForB[0].x == 250.0f);
    CHECK(spawnsForB[0].entityType == vn::kEntityTypePlayer);
    const auto spawnsForA = a->all<vn::EntitySpawn>();
    REQUIRE(spawnsForA.size() == 1);
    CHECK(spawnsForA[0].entityId == 2);
    CHECK(far->count<vn::EntitySpawn>() == 0);
    CHECK(a->visible() == std::set<std::uint32_t>{2});
}

// ---- AOI property test ----

TEST_CASE("Zone: AOI membership matches a brute-force oracle under random walks", "[server][world][zone][aoi]")
{
    Harness h;
    std::mt19937 rng(20261115);
    std::uniform_real_distribution<float> anywhere(0.0f, 511.9f);
    std::uniform_real_distribution<float> step(-6.0f, 6.0f);
    std::uniform_int_distribution<int> percent(0, 99);
    std::map<vw::SessionKey, std::pair<float, float>> pos;
    vw::SessionKey next = 1;
    for (; next <= 40; ++next)
    {
        // Clustered around the middle so there are many neighbours and many boundary crossings.
        const float x = 200.0f + step(rng) * 15.0f, z = 220.0f + step(rng) * 15.0f;
        h.enter(next, x, z);
        pos[next] = {x, z};
    }
    h.step();

    const auto cellDist = [](std::pair<float, float> a, std::pair<float, float> b) {
        const auto c = [](float v) { return std::clamp(static_cast<int>(v / 32.0f), 0, 15); };
        return std::max(std::abs(c(a.first) - c(b.first)), std::abs(c(a.second) - c(b.second)));
    };
    const auto verify = [&] {
        for (const auto& [sa, pa] : pos)
        {
            const auto& ka = h.known(sa);
            for (const auto& [sb, pb] : pos)
            {
                if (sa == sb)
                {
                    REQUIRE_FALSE(ka.contains(static_cast<vw::EntityId>(sb)));
                    continue;
                }
                const bool oracle = cellDist(pa, pb) <= 2;
                const bool aSeesB = ka.contains(static_cast<vw::EntityId>(sb));
                const bool bSeesA = h.known(sb).contains(static_cast<vw::EntityId>(sa));
                INFO("A " << sa << " (" << pa.first << ", " << pa.second << ") B " << sb << " (" << pb.first << ", "
                          << pb.second << ")");
                REQUIRE(aSeesB == oracle);
                REQUIRE(bSeesA == aSeesB);
            }
            // What the client was told matches the server's view exactly.
            std::set<std::uint32_t> expected;
            for (const auto id : ka)
                expected.insert(id);
            REQUIRE(h.links[sa]->visible() == expected);
            REQUIRE(h.links[sa]->errors().empty());
        }
    };
    verify();

    for (int iteration = 0; iteration < 3000; ++iteration)
    {
        const int roll = percent(rng);
        if (roll < 3 && pos.size() > 5)
        {
            // Leave.
            auto it = pos.begin();
            std::advance(it, std::uniform_int_distribution<std::size_t>(0, pos.size() - 1)(rng));
            REQUIRE(h.zone->leave(it->first));
            pos.erase(it);
        }
        else if (roll < 6)
        {
            // Enter.
            const float x = anywhere(rng), z = anywhere(rng);
            h.enter(next, x, z);
            pos[next++] = {x, z};
        }
        else
        {
            auto it = pos.begin();
            std::advance(it, std::uniform_int_distribution<std::size_t>(0, pos.size() - 1)(rng));
            auto [x, z] = it->second;
            if (roll < 10)
            {
                x = anywhere(rng); // teleport (e.g. a correction far away, or admin move)
                z = anywhere(rng);
            }
            else
            {
                x = std::clamp(x + step(rng), 0.0f, 511.9f);
                z = std::clamp(z + step(rng), 0.0f, 511.9f);
            }
            h.zone->placeEntity(it->first, x, z);
            it->second = {x, z};
        }
        if (iteration % 7 == 0)
            h.step();
        if (iteration % 50 == 0)
        {
            h.step();
            verify();
        }
    }
    h.step();
    verify();
}

// ---- Relay ----

TEST_CASE("Zone: ENTITY_MOVE at most every 100 ms per entity, latest state, final stop delivered",
          "[server][world][zone][relay]")
{
    Harness h;
    auto a = h.enter(1, 250.0f, 250.0f);
    auto b = h.enter(2, 255.0f, 250.0f);
    h.step();
    a->clear();

    // B moves every tick (20 Hz) for 2 s: A must get 10 ENTITY_MOVE per second, each with B's latest state.
    float x = 255.0f;
    for (int i = 0; i < 40; ++i)
    {
        x += 0.25f;
        REQUIRE(h.move(2, x, 250.0f) == vw::MoveVerdict::Accepted);
        h.step();
    }
    auto moves = movesOf(*a, 2);
    CHECK(moves.size() == 20);
    for (std::size_t i = 1; i < moves.size(); ++i)
        CHECK(moves[i].x > moves[i - 1].x);
    // Every relay carries the state of its tick (not an older one).
    CHECK(moves.back().x == x - 0.25f);

    // Stop in a tick right after a relay: not sent at once (rate limit), but on the next allowed tick.
    a->clear();
    h.step(); // relays the last moving state (x)
    REQUIRE(movesOf(*a, 2).size() == 1);
    REQUIRE(h.move(2, x, 250.0f, vg::kMoveRunning) == vw::MoveVerdict::Accepted); // stop packet
    h.step();
    CHECK(movesOf(*a, 2).size() == 1); // rate limited this tick
    h.step();
    moves = movesOf(*a, 2);
    REQUIRE(moves.size() == 2);
    CHECK((moves.back().flags & vg::kMoveMoving) == 0);
    CHECK(moves.back().x == x);
    CHECK(moves.back().y == h.zone->terrain().heightAt(x, 250.0f));

    // Repeated identical stop packets are not relayed again.
    for (int i = 0; i < 6; ++i)
    {
        REQUIRE(h.move(2, x, 250.0f, vg::kMoveRunning) == vw::MoveVerdict::Accepted);
        h.step();
    }
    CHECK(movesOf(*a, 2).size() == 2);

    // A start after a long pause goes out on the very next tick.
    REQUIRE(h.move(2, x + 0.3f, 250.0f) == vw::MoveVerdict::Accepted);
    h.step();
    CHECK(movesOf(*a, 2).size() == 3);
    CHECK(b->errors().empty());
    CHECK(a->errors().empty());
}

TEST_CASE("Zone: start, move and stop inside one relay interval still ends with the stop state", "[server][world][zone][relay]")
{
    Harness h;
    auto a = h.enter(1, 250.0f, 250.0f);
    h.enter(2, 255.0f, 250.0f);
    h.step();
    REQUIRE(h.move(2, 255.3f, 250.0f) == vw::MoveVerdict::Accepted);
    h.step(); // relayed
    REQUIRE(h.move(2, 255.6f, 250.0f, vg::kMoveMoving | vg::kMoveRunning, 10ms) == vw::MoveVerdict::Accepted);
    REQUIRE(h.move(2, 255.7f, 250.0f, vg::kMoveRunning, 20ms) == vw::MoveVerdict::Accepted); // stop
    h.step();
    h.step();
    h.step();
    const auto moves = movesOf(*a, 2);
    REQUIRE(moves.size() == 2);
    CHECK(moves[1].x == 255.7f);
    CHECK((moves[1].flags & vg::kMoveMoving) == 0);
}

// ---- Corrections ----

TEST_CASE("Zone: rejected moves -> POSITION_CORRECT with the last seq; observers see the stop", "[server][world][zone]")
{
    Harness h;
    auto a = h.enter(1, 250.0f, 250.0f);
    auto b = h.enter(2, 255.0f, 250.0f);
    h.step();
    REQUIRE(h.move(2, 255.5f, 250.0f) == vw::MoveVerdict::Accepted); // seq 1
    h.step();
    h.step();
    REQUIRE((movesOf(*a, 2).back().flags & vg::kMoveMoving) != 0);
    REQUIRE(h.move(2, 300.0f, 250.0f) == vw::MoveVerdict::TooFar); // seq 2
    REQUIRE(h.move(2, 301.0f, 250.0f) == vw::MoveVerdict::TooFar); // seq 3, same tick
    h.step();
    const auto corr = b->all<vn::PositionCorrect>();
    REQUIRE(corr.size() == 1); // one per tick
    CHECK(corr[0].seq == 3);
    CHECK(corr[0].x == 255.5f);
    CHECK(corr[0].z == 250.0f);
    CHECK(corr[0].y == h.zone->terrain().heightAt(255.5f, 250.0f));
    h.step();
    const auto last = movesOf(*a, 2).back();
    CHECK(last.x == 255.5f);
    CHECK((last.flags & vg::kMoveMoving) == 0); // stands at the corrected position
    CHECK(a->count<vn::PositionCorrect>() == 0);
    // Unknown session: nothing happens.
    CHECK_FALSE(h.move(99, 1.0f, 1.0f).has_value());
}

// ---- Leave ----

TEST_CASE("Zone: leave despawns to observers, returns the final position", "[server][world][zone]")
{
    Harness h;
    auto a = h.enter(1, 250.0f, 250.0f);
    auto b = h.enter(2, 255.0f, 250.0f);
    h.step();
    REQUIRE(h.move(2, 255.5f, 250.5f) == vw::MoveVerdict::Accepted);
    const auto left = h.zone->leave(2);
    REQUIRE(left);
    CHECK(left->character == 2);
    CHECK(left->account == 2);
    CHECK(left->position.x == 255.5f);
    CHECK(left->position.z == 250.5f);
    CHECK(left->position.zoneId == 1);
    const auto batchesBefore = b->batches();
    h.step();
    CHECK(b->batches() == batchesBefore); // its pending packets were dropped
    const auto despawns = a->all<vn::EntityDespawn>();
    REQUIRE(despawns.size() == 1);
    CHECK(despawns[0].entityId == 2);
    CHECK(a->visible().empty());
    CHECK(h.known(1).empty());
    CHECK_FALSE(h.zone->leave(2));
    CHECK(h.zone->playerCount() == 1);
}

// ---- Persistence cadence ----

TEST_CASE("Zone: periodic saves are staggered over the period and skip unchanged players", "[server][world][zone]")
{
    auto s = testSettings();
    s.savePeriodTicks = 40;
    Harness h(s);
    for (vw::SessionKey i = 1; i <= 60; ++i)
        h.enter(i, 150.0f + static_cast<float>(i), 250.0f);
    std::map<std::uint64_t, int> perTick;
    std::map<va::CharacterId, int> perChar;
    const auto save = [&](va::AccountId, va::CharacterId c, va::CharacterPosition p, std::function<void(bool)> done) {
        ++perTick[h.tick];
        ++perChar[c];
        CHECK(p.zoneId == 1);
        if (done)
            done(true);
    };
    for (int i = 0; i < 40; ++i)
        h.step(save);
    CHECK(perChar.size() == 60); // everyone once per period
    for (const auto& [c, n] : perChar)
        CHECK(n == 1);
    CHECK(perTick.size() >= 20); // spread out, not all in one tick
    for (const auto& [t, n] : perTick)
        CHECK(n <= 8);

    // Nobody moved: the next period saves nothing; one mover is saved exactly once.
    perChar.clear();
    REQUIRE(h.move(5, 155.5f, 250.0f) == vw::MoveVerdict::Accepted);
    for (int i = 0; i < 40; ++i)
        h.step(save);
    CHECK(perChar.size() == 1);
    CHECK(perChar[5] == 1);
}
