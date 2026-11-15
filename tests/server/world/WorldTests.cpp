// World (server/world/World): the command queue, enter/move/leave lifecycle with fake session links,
// position saves on leave and at stop(), take-over of a character still in the world (duplicate
// login), the recently-left cache, the simulation thread, the MOVE_REQ flood limiter and TickStats.

#include "server/world/MoveRateLimiter.h"
#include "server/world/TickStats.h"
#include "server/world/World.h"
#include "tests/server/world/FakeLink.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace vw = vo::server::world;
namespace va = vo::server::auth;
namespace vn = vo::net;
namespace vg = vo::game;
using namespace std::chrono_literals;
using vo::tests::FakeLink;

namespace {

struct Saves
{
    struct Entry
    {
        va::AccountId account;
        va::CharacterId character;
        va::CharacterPosition position;
    };
    std::mutex mutex;
    std::vector<Entry> entries;
    bool complete = true; // call `done` synchronously (like a fast DB); false: never complete
    std::vector<std::function<void(bool)>> pending;

    vw::SavePositionFn fn()
    {
        return [this](va::AccountId a, va::CharacterId c, va::CharacterPosition p, std::function<void(bool)> done) {
            {
                const std::scoped_lock lock(mutex);
                entries.push_back({a, c, p});
                if (!complete)
                {
                    pending.push_back(std::move(done));
                    return;
                }
            }
            if (done)
                done(true);
        };
    }
    std::size_t size()
    {
        const std::scoped_lock lock(mutex);
        return entries.size();
    }
};

struct WorldHarness
{
    Saves saves;
    vw::World world;
    vo::core::MonoTime t0 = vo::core::monoNow();
    int tickNo = 0;

    explicit WorldHarness(vw::WorldSettings s = {}) : world(withSeed(s), saves.fn(), vo::core::monoNow())
    {
        std::string error;
        REQUIRE(world.createZone(1, vo::tests::makeTestTerrain(), error));
    }
    static vw::WorldSettings withSeed(vw::WorldSettings s)
    {
        s.zone.seed = 99;
        return s;
    }
    [[nodiscard]] vo::core::MonoTime now() const { return t0 + tickNo * 50ms; }
    void tick()
    {
        world.tick(now(), now());
        ++tickNo;
    }

    std::shared_ptr<FakeLink> enter(vw::SessionKey session, va::CharacterId character, float x, float z,
                                    std::uint64_t generation = 1)
    {
        auto link = std::make_shared<FakeLink>();
        vw::EnterCommand cmd;
        cmd.session = session;
        cmd.generation = generation;
        cmd.link = link;
        cmd.account = character + 1000;
        cmd.character.id = character;
        cmd.character.name = "Char" + std::to_string(character);
        cmd.character.race = 1;
        cmd.character.charClass = 2;
        cmd.character.level = 1;
        cmd.character.position = {1, x, 0.0f, z, 0.0f};
        world.post(std::move(cmd));
        return link;
    }
    void move(vw::SessionKey session, std::uint32_t seq, float x, float z, vo::core::MonoClock::duration at,
              std::uint8_t flags = vg::kMoveMoving)
    {
        vn::MoveReq r;
        r.seq = seq;
        r.flags = flags;
        r.x = x;
        r.z = z;
        r.y = 0.0f;
        world.post(vw::MoveCommand{session, r, t0 + at});
    }
};

} // namespace

TEST_CASE("World: enter, see each other, move, leave with a position save", "[server][world]")
{
    WorldHarness h;
    auto a = h.enter(1, 11, 250.0f, 250.0f);
    h.tick();
    REQUIRE(a->entered());
    CHECK(a->entered()->x == 250.0f);
    CHECK(h.world.playerCount() == 1);

    auto b = h.enter(2, 12, 252.0f, 250.0f);
    h.tick();
    REQUIRE(b->entered());
    CHECK(b->entered()->entityId != a->entered()->entityId);
    CHECK(a->visible() == std::set<std::uint32_t>{b->entered()->entityId});
    CHECK(b->visible() == std::set<std::uint32_t>{a->entered()->entityId});

    h.move(2, 1, 252.5f, 250.0f, 150ms);
    h.tick();
    h.tick();
    const auto moves = a->all<vn::EntityMove>();
    REQUIRE_FALSE(moves.empty());
    CHECK(moves.back().x == 252.5f);
    CHECK(moves.back().serverTimeMs == h.world.serverTimeMs(h.t0 + 150ms));

    std::optional<bool> done;
    h.world.post(vw::LeaveCommand{2, [&](bool ok) { done = ok; }});
    h.tick();
    REQUIRE(done.has_value());
    CHECK(*done);
    REQUIRE(h.saves.size() == 1);
    CHECK(h.saves.entries[0].character == 12);
    CHECK(h.saves.entries[0].account == 1012);
    CHECK(h.saves.entries[0].position.x == 252.5f);
    CHECK(h.saves.entries[0].position.zoneId == 1);
    CHECK(a->visible().empty());
    CHECK(h.world.playerCount() == 1);
    CHECK(a->errors().empty());
    CHECK(b->errors().empty());
}

TEST_CASE("World: leave of a session without an entity completes at once without a save", "[server][world]")
{
    WorldHarness h;
    std::optional<bool> done;
    h.world.post(vw::LeaveCommand{42, [&](bool ok) { done = ok; }});
    h.tick();
    REQUIRE(done.has_value());
    CHECK(*done);
    CHECK(h.saves.size() == 0);
}

TEST_CASE("World: re-entering a character still in the world takes over its live position", "[server][world]")
{
    WorldHarness h;
    auto oldLink = h.enter(1, 7, 250.0f, 250.0f);
    auto watcher = h.enter(3, 8, 255.0f, 250.0f);
    h.tick();
    h.move(1, 1, 251.0f, 250.5f, 1s);
    h.tick();
    // Duplicate login: the new session enters the same character with the (stale) stored position
    // before the old session's leave arrives.
    auto newLink = h.enter(2, 7, 250.0f, 250.0f, 1);
    h.tick();
    REQUIRE(newLink->entered());
    CHECK(newLink->entered()->x == 251.0f);
    CHECK(newLink->entered()->z == 250.5f);
    CHECK(h.world.playerCount() == 2);
    // The watcher saw the old entity go and the new one come.
    const auto oldId = oldLink->entered()->entityId;
    const auto newId = newLink->entered()->entityId;
    CHECK(oldId != newId);
    CHECK(watcher->visible() == std::set<std::uint32_t>{newId});
    CHECK(watcher->errors().empty());
    // The old session's leave arrives later: nothing to do, no save of the stale entity.
    const std::size_t batchesOld = oldLink->batches();
    std::optional<bool> done;
    h.world.post(vw::LeaveCommand{1, [&](bool ok) { done = ok; }});
    h.tick();
    REQUIRE(done.has_value());
    CHECK(*done);
    CHECK(h.saves.size() == 0);
    CHECK(oldLink->batches() == batchesOld);
    CHECK(h.world.playerCount() == 2);
}

TEST_CASE("World: a character that just left re-enters at its last position even if the save is slow",
          "[server][world]")
{
    WorldHarness h;
    h.saves.complete = false; // the DB has not stored anything yet
    h.enter(1, 7, 250.0f, 250.0f);
    h.tick();
    h.move(1, 1, 251.5f, 250.0f, 1s);
    h.tick();
    std::optional<bool> done;
    h.world.post(vw::LeaveCommand{1, [&](bool ok) { done = ok; }});
    h.tick();
    CHECK_FALSE(done.has_value()); // waits for the save
    auto again = h.enter(2, 7, 250.0f, 250.0f);
    h.tick();
    REQUIRE(again->entered());
    CHECK(again->entered()->x == 251.5f);
    // Completing the save completes the leave.
    std::vector<std::function<void(bool)>> pending;
    {
        const std::scoped_lock lock(h.saves.mutex);
        pending.swap(h.saves.pending);
    }
    REQUIRE(pending.size() == 1);
    pending[0](true);
    REQUIRE(done.has_value());
    CHECK(*done);
}

TEST_CASE("World: stop() saves everyone still in the world and refuses later commands", "[server][world]")
{
    WorldHarness h;
    h.enter(1, 1, 250.0f, 250.0f);
    h.enter(2, 2, 260.0f, 250.0f);
    h.tick();
    h.enter(3, 3, 270.0f, 250.0f); // still queued when stop() runs: processed, then saved
    h.world.stop();
    CHECK(h.saves.size() == 3);
    CHECK(h.world.playerCount() == 0);
    std::optional<bool> done;
    h.world.post(vw::LeaveCommand{1, [&](bool ok) { done = ok; }});
    REQUIRE(done.has_value());
    CHECK_FALSE(*done);
    auto late = h.enter(4, 4, 250.0f, 250.0f);
    h.world.stop(); // idempotent
    CHECK_FALSE(late->entered());
    CHECK(h.saves.size() == 3);
}

TEST_CASE("World: the simulation thread runs the fixed-rate loop", "[server][world][thread]")
{
    vw::WorldSettings s;
    s.tickRate = 50;
    s.statsInterval = std::chrono::seconds(0);
    WorldHarness h(s);
    CHECK(h.world.tickInterval() == 20ms);
    h.world.start();
    CHECK(h.world.running());
    auto link = h.enter(1, 1, 250.0f, 250.0f);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!link->entered() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(link->entered());
    // Moves posted from this thread are validated on the world thread; a teleport is corrected.
    vn::MoveReq r;
    r.seq = 1;
    r.flags = vg::kMoveMoving;
    r.x = 400.0f;
    r.z = 250.0f;
    h.world.post(vw::MoveCommand{1, r, vo::core::monoNow()});
    const auto deadline2 = std::chrono::steady_clock::now() + 2s;
    while (link->count<vn::PositionCorrect>() == 0 && std::chrono::steady_clock::now() < deadline2)
        std::this_thread::sleep_for(1ms);
    CHECK(link->count<vn::PositionCorrect>() == 1);
    std::this_thread::sleep_for(400ms);
    h.world.stop();
    CHECK_FALSE(h.world.running());
    const auto st = h.world.totalStats();
    CHECK(st.ticks >= 15);
    CHECK(st.ticks <= 80);
    CHECK(st.avgMs < 20.0);
    CHECK(h.saves.size() == 1); // saved at stop
}

TEST_CASE("World: new characters spawn at the start zone's race spawns", "[server][world]")
{
    WorldHarness h;
    const auto s = h.world.newCharacterSpawns();
    CHECK(s.aydor.zoneId == 1);
    CHECK(s.aydor.x == 100.0f);
    CHECK(s.aydor.z == 100.0f);
    CHECK(s.kargan.x == 400.0f);
    CHECK(s.forRace(2).z == 400.0f);
    CHECK(s.forRace(1).x == 100.0f);
}

TEST_CASE("MoveRateLimiter: more than 30 per second is refused", "[server][world][flood]")
{
    vw::MoveRateLimiter lim;
    const auto t0 = vo::core::monoNow();
    for (int i = 0; i < 30; ++i)
        REQUIRE(lim.allow(t0 + std::chrono::milliseconds(i)));
    CHECK_FALSE(lim.allow(t0 + 999ms)); // 31st within one second
    vw::MoveRateLimiter steady;
    for (int i = 0; i < 300; ++i) // exactly 30 Hz for 10 s is fine
        REQUIRE(steady.allow(t0 + std::chrono::microseconds(33'334) * i));
    vw::MoveRateLimiter sliding;
    for (int i = 0; i < 30; ++i)
        REQUIRE(sliding.allow(t0));
    CHECK_FALSE(sliding.allow(t0 + 500ms));
    CHECK(sliding.allow(t0 + 1000ms)); // the window has passed for the oldest
    sliding.reset();
    for (int i = 0; i < 30; ++i)
        REQUIRE(sliding.allow(t0 + 2000ms));
}

TEST_CASE("TickStats: average, percentiles, maximum, slow ticks", "[server][world]")
{
    vw::TickStats s(50ms);
    CHECK(s.summary().ticks == 0);
    for (int i = 0; i < 98; ++i)
        s.record(1ms);
    s.record(30ms);
    s.record(80ms);
    const auto sum = s.summary();
    CHECK(sum.ticks == 100);
    CHECK(sum.slowTicks == 1);
    CHECK_THAT(sum.avgMs, Catch::Matchers::WithinAbs((98.0 + 30.0 + 80.0) / 100.0, 1e-6));
    CHECK_THAT(sum.p50Ms, Catch::Matchers::WithinAbs(1.0, 0.051));
    CHECK_THAT(sum.p99Ms, Catch::Matchers::WithinAbs(30.0, 0.051));
    CHECK_THAT(sum.maxMs, Catch::Matchers::WithinAbs(80.0, 1e-6));
    s.reset();
    CHECK(s.summary().ticks == 0);
}
