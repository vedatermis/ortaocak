// World scenarios of the bot client (docs/SERVER_WORLD.md §9): legal walking, speed hack, wall hack,
// MOVE_REQ flood, area of interest between two bots, position persistence. All moves are planned on
// the same zone terrain the server uses (--terrain, default assets/terrain/zone1/zone1.vterrain).

#include "shared/game/Movement.h"
#include "shared/game/TerrainWalk.h"
#include "tools/botclient/Bot.h"
#include "tools/botclient/Nav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace vo::tools::botclient {

namespace {

constexpr float kSpeedSafety = 0.9f; // bots move at 90 % of the allowed speed
constexpr float kPosEps = 0.01f;
constexpr auto kStep = Ms(100); // 10 Hz while moving (§5.4)

const assets::TerrainData& terrainOf(const Context& c)
{
    if (!c.terrain)
        fail("no terrain loaded (use --terrain <zone1.vterrain>)");
    return *c.terrain;
}

struct Self
{
    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
    [[nodiscard]] Point p() const { return {x, z}; }
};

void loginAs(Bot& bot, const std::string& user, const std::string& password)
{
    const auto res = bot.login(user, password);
    if (res.result != vn::LoginResult::Ok)
        fail(std::format("login of '{}' failed with result {}", user, static_cast<int>(res.result)));
}

std::vector<vn::CharSummary> listChars(Bot& bot)
{
    bot.send(vn::CharListReq{});
    return bot.expect<vn::CharListRes>().characters;
}

std::uint32_t createCharacter(Bot& bot, int race)
{
    vn::CharCreateReq req;
    req.name = randomCharacterName();
    req.race = static_cast<std::uint8_t>(race);
    req.charClass = 1;
    req.face = 1;
    req.hair = 2;
    bot.send(req);
    const auto res = bot.expect<vn::CharCreateRes>();
    if (res.result != vn::CharCreateResult::Ok)
        fail(std::format("CHAR_CREATE_RES result {}", static_cast<int>(res.result)));
    return res.character.charId;
}

// ENTER_WORLD_REQ; world tracking is switched on first so no ENTITY_SPAWN is missed.
vn::EnterWorldRes enterWorld(Bot& bot, std::uint32_t charId, const assets::TerrainData& t, Self& self)
{
    bot.view() = {}; // a new world session starts with no entities
    bot.trackWorld(true);
    bot.send(vn::EnterWorldReq{charId});
    const auto enter = bot.expect<vn::EnterWorldRes>();
    if (enter.result != vn::EnterWorldResult::Ok || enter.entityId == 0)
        fail(std::format("ENTER_WORLD_RES result {}", static_cast<int>(enter.result)));
    if (enter.zoneId != 1 || enter.walkSpeed != game::kWalkSpeed || enter.runSpeed != game::kRunSpeed)
        fail(std::format("ENTER_WORLD_RES zone {} speeds {}/{} (expected zone 1, {}/{})", enter.zoneId, enter.walkSpeed,
                         enter.runSpeed, game::kWalkSpeed, game::kRunSpeed));
    if (!t.isWalkable(enter.x, enter.z))
        fail(std::format("entered at an unwalkable position ({:.2f}, {:.2f})", enter.x, enter.z));
    if (std::abs(enter.y - t.heightAt(enter.x, enter.z)) > kPosEps)
        fail(std::format("ENTER_WORLD_RES y {:.3f} is not on the terrain ({:.3f})", enter.y, t.heightAt(enter.x, enter.z)));
    const auto& v = bot.view();
    if (v.firstSpawnIndex != 0 && v.firstSpawnIndex < bot.lastIndex())
        fail("ENTITY_SPAWN arrived before ENTER_WORLD_RES");
    self = {enter.x, enter.y, enter.z, enter.yaw};
    return enter;
}

void sendMove(Bot& bot, const assets::TerrainData& t, Self& self, Point to, std::uint8_t flags,
              std::uint32_t* seqOut = nullptr)
{
    const float dx = to.x - self.x, dz = to.z - self.z;
    if (std::abs(dx) + std::abs(dz) > 1e-6f)
        self.yaw = game::yawFromDirection(dx, dz);
    self.x = to.x;
    self.z = to.z;
    self.y = t.heightAt(to.x, to.z);
    vn::MoveReq m;
    m.seq = bot.nextSeq();
    m.flags = flags;
    m.x = self.x;
    m.y = self.y;
    m.z = self.z;
    m.yaw = self.yaw;
    bot.send(m);
    if (seqOut)
        *seqOut = m.seq;
}

// Waits `d` while pumping `bot` and calling `idle` (other bots) every millisecond or so.
void waitPumping(Bot& bot, Ms d, const std::function<void()>& idle)
{
    const auto until = Clock::now() + d;
    while (Clock::now() < until)
    {
        bot.poll();
        if (idle)
            idle();
        std::this_thread::sleep_for(Ms(1));
    }
}

// Walks `path` (from Nav::bfs) sending MOVE_REQ every `step` at 90 % of the walk/run speed, never
// passing a path vertex within one step, then a stop packet. Returns the number of corrections
// received meanwhile (should be 0 for a legal walk).
std::size_t walkPath(Bot& bot, const assets::TerrainData& t, Self& self, const std::vector<Point>& path, bool run,
                     const std::function<void()>& idle = nullptr, Ms step = kStep)
{
    const std::size_t before = bot.view().corrections.size();
    const std::uint8_t moving = static_cast<std::uint8_t>(game::kMoveMoving | (run ? game::kMoveRunning : 0));
    const float speed = (run ? game::kRunSpeed : game::kWalkSpeed) * kSpeedSafety;
    sendMove(bot, t, self, self.p(), moving); // start
    auto last = Clock::now();
    std::size_t next = 1;
    while (next < path.size())
    {
        waitPumping(bot, step, idle);
        bot.keepAlive();
        const auto now = Clock::now();
        float budget = speed * std::chrono::duration<float>(now - last).count();
        last = now;
        Point pos = self.p();
        const Point target = path[next];
        const float d = distance(pos, target);
        if (d <= budget)
        {
            pos = target;
            ++next;
        }
        else
        {
            pos.x += (target.x - pos.x) * budget / d;
            pos.z += (target.z - pos.z) * budget / d;
        }
        sendMove(bot, t, self, pos, moving);
        if (bot.view().corrections.size() > before)
        {
            const auto& c = bot.view().corrections.back();
            fail(std::format("POSITION_CORRECT seq {} to ({:.2f}, {:.2f}) during a legal walk at ({:.2f}, {:.2f})",
                             c.seq, c.x, c.z, self.x, self.z));
        }
    }
    waitPumping(bot, Ms(50), idle);
    sendMove(bot, t, self, self.p(), static_cast<std::uint8_t>(run ? game::kMoveRunning : 0)); // stop
    waitPumping(bot, Ms(150), idle);
    return bot.view().corrections.size() - before;
}

std::vector<Point> pathAway(const assets::TerrainData& t, Point from, float minDist, int maxVisited = 60000)
{
    const Nav nav(t);
    auto path = nav.bfs(
        from, [&](int i, int j) { return distance(nav.centre(i, j), from) >= minDist; }, maxVisited);
    if (!path)
        fail(std::format("no walkable path of {:.0f} m from ({:.1f}, {:.1f})", minDist, from.x, from.z));
    return *path;
}

void expectNoViewErrors(Bot& bot, std::string_view who)
{
    if (!bot.view().errors.empty())
        fail(std::format("{}: {}", who, bot.view().errors.front()));
}

const vn::PositionCorrect& waitCorrection(Bot& bot, std::size_t before, Ms timeout, std::string_view what)
{
    if (!bot.pumpUntil([&] { return bot.view().corrections.size() > before; }, timeout))
        fail(std::format("no POSITION_CORRECT after {}", what));
    return bot.view().corrections.back();
}

void expectAt(const vn::PositionCorrect& c, Point p, std::string_view what)
{
    if (std::abs(c.x - p.x) > kPosEps || std::abs(c.z - p.z) > kPosEps)
        fail(std::format("{}: corrected to ({:.3f}, {:.3f}), expected the unchanged position ({:.3f}, {:.3f})", what,
                         c.x, c.z, p.x, p.z));
}

// A direction from `from` along which the next `length` metres are walkable in a straight line.
std::optional<Point> straightDirection(const assets::TerrainData& t, Point from, float length)
{
    for (int k = 0; k < 32; ++k)
    {
        const float a = static_cast<float>(k) * 6.2831853f / 32.0f;
        const Point d{std::sin(a), std::cos(a)};
        if (game::isSegmentWalkable(t, from.x, from.z, from.x + d.x * length, from.z + d.z * length))
            return d;
    }
    return std::nullopt;
}

std::string truncUser(const std::string& base, const std::string& suffix)
{
    return base.substr(0, std::min<std::size_t>(base.size(), 20 - suffix.size())) + suffix;
}

} // namespace

// ---- world: legal walking, leave, re-enter ----

std::string scenarioWorld(const Context& c)
{
    const auto& t = terrainOf(c);
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    const std::uint32_t charId = createCharacter(bot, c.opts.race);
    Self self;
    const auto enter = enterWorld(bot, charId, t, self);

    // A new character starts at its race spawn, spread by at most a couple of metres.
    const auto* spawn = t.findSpawn(c.opts.race == 2 ? "spawn_kargan" : "spawn_aydor");
    if (!spawn)
        fail("terrain has no race spawn point");
    const float fromSpawn = distance(self.p(), {spawn->position.x, spawn->position.z});
    if (fromSpawn > 2.5f)
        fail(std::format("new character entered {:.2f} m from its race spawn (expected <= 2.5 m)", fromSpawn));

    // Run 20 m away along a planned path, then walk 8 m: no correction allowed.
    const Point start = self.p();
    if (walkPath(bot, t, self, pathAway(t, start, 20.0f), true) != 0)
        fail("corrections while running a legal path");
    const float ran = distance(self.p(), start);
    if (walkPath(bot, t, self, pathAway(t, self.p(), 8.0f), false) != 0)
        fail("corrections while walking a legal path");
    bot.pumpFor(Ms(300));
    if (!bot.view().corrections.empty())
        fail("POSITION_CORRECT after a legal walk");

    bot.send(vn::Ping{42});
    if (bot.expect<vn::Pong>().clientTimeMs != 42)
        fail("bad PONG in world");

    bot.send(vn::LeaveWorldReq{});
    if (bot.expect<vn::LeaveWorldRes>().result != vn::kLeaveWorldOk)
        fail("LEAVE_WORLD_RES not ok");
    bot.trackWorld(false);
    if (listChars(bot).empty())
        fail("character list empty after leaving the world");

    // Re-entering puts the character where it left.
    Self again;
    (void)enterWorld(bot, charId, t, again);
    if (distance(again.p(), self.p()) > kPosEps)
        fail(std::format("re-entered at ({:.2f}, {:.2f}) instead of the last position ({:.2f}, {:.2f})", again.x,
                         again.z, self.x, self.z));
    bot.send(vn::LeaveWorldReq{});
    if (bot.expect<vn::LeaveWorldRes>().result != vn::kLeaveWorldOk)
        fail("second LEAVE_WORLD_RES not ok");
    expectNoViewErrors(bot, "world view");

    bot.send(vn::Logout{});
    bot.expectClosed(false, bot.timeout());
    return std::format("entity {} spawned {:.2f} m from the race spawn, ran {:.1f} m + walked, 0 corrections, "
                       "left, re-entered at the same spot, logged out",
                       enter.entityId, fromSpawn, ran);
}

// ---- speedhack: too-fast moves are corrected, the server position stays put ----

std::string scenarioSpeedhack(const Context& c)
{
    const auto& t = terrainOf(c);
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    Self self;
    (void)enterWorld(bot, createCharacter(bot, c.opts.race), t, self);
    const Point start = self.p();
    const auto dir = straightDirection(t, start, 60.0f);
    if (!dir)
        fail("no straight walkable direction near the spawn");

    // Full movement allowance first (worst case for the server), then 4x run speed at 10 Hz.
    bot.pumpFor(Ms(1300));
    Self hack = self;
    std::uint32_t lastSeq = 0;
    constexpr int kHackSteps = 10;
    const float hackStep = game::kRunSpeed * 4.0f * 0.1f;
    for (int i = 1; i <= kHackSteps; ++i)
    {
        sendMove(bot, t, hack, {start.x + dir->x * hackStep * static_cast<float>(i), start.z + dir->z * hackStep * static_cast<float>(i)},
                 game::kMoveMoving | game::kMoveRunning, &lastSeq);
        bot.pumpFor(kStep);
    }
    bot.pumpFor(Ms(300));
    const auto& corr = bot.view().corrections;
    if (corr.empty())
        fail("no POSITION_CORRECT for moves at 4x run speed");
    // Once a move is rejected every later one is even further away: the corrected position must stay
    // the same and be much closer than where the hacker claims to be.
    const Point held{corr.front().x, corr.front().z};
    for (const auto& k : corr)
        expectAt(k, held, "repeated speed-hack correction");
    if (corr.back().seq != lastSeq)
        fail(std::format("last POSITION_CORRECT carries seq {}, expected the last processed seq {}", corr.back().seq,
                         lastSeq));
    const float claimed = hackStep * kHackSteps;
    const float allowed = distance(held, start);
    if (allowed > claimed * 0.7f)
        fail(std::format("server accepted {:.1f} of {:.1f} m claimed at 4x speed", allowed, claimed));

    // Teleport: 50 m in one packet after a full allowance.
    bot.pumpFor(Ms(1300));
    std::size_t before = corr.size();
    sendMove(bot, t, hack, {held.x + dir->x * 40.0f, held.z + dir->z * 40.0f}, game::kMoveMoving | game::kMoveRunning);
    expectAt(waitCorrection(bot, before, Ms(1000), "a 40 m teleport"), held, "teleport");

    // Banked time: standing still for 5 s must not allow a 15 m jump (allowance capped at ~1 s).
    for (int i = 0; i < 5; ++i)
    {
        bot.pumpFor(Ms(1000));
        bot.keepAlive();
    }
    before = bot.view().corrections.size();
    sendMove(bot, t, hack, {held.x + dir->x * 15.0f, held.z + dir->z * 15.0f}, game::kMoveMoving | game::kMoveRunning);
    expectAt(waitCorrection(bot, before, Ms(1000), "a 15 m jump after standing still 5 s"), held, "banked time");

    // A legal step from the corrected position is accepted again.
    before = bot.view().corrections.size();
    Self legal{held.x, t.heightAt(held.x, held.z), held.z, 0.0f};
    sendMove(bot, t, legal, {held.x + dir->x * 1.0f, held.z + dir->z * 1.0f}, game::kMoveMoving);
    bot.pumpFor(Ms(500));
    if (bot.view().corrections.size() != before)
        fail("a legal 1 m step after the corrections was rejected");
    expectNoViewErrors(bot, "speedhack view");
    bot.send(vn::Logout{});
    bot.expectClosed(true, bot.timeout());
    return std::format("{} corrections; server held the player {:.1f} m from the start while it claimed {:.1f} m; "
                       "teleport and banked 15 m jump rejected; legal step accepted",
                       corr.size(), allowed, claimed);
}

// ---- wallhack: into a blocked cell, and through a thin wall ----

std::string scenarioWallhack(const Context& c)
{
    const auto& t = terrainOf(c);
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    Self self;
    (void)enterWorld(bot, createCharacter(bot, c.opts.race), t, self);

    // Nearest walkable cell with a 1-3 cell thick blocked wall in one of the four directions and
    // walkable ground behind it.
    const Nav nav(t);
    int wallDi = 0, wallDj = 0, thickness = 0;
    static constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    const auto path = nav.bfs(
        self.p(),
        [&](int i, int j) {
            for (const auto& d : kDirs)
            {
                int k = 1;
                while (k <= 3 && !nav.walkable(i + d[0] * k, j + d[1] * k) && i + d[0] * k >= 0 &&
                       j + d[1] * k >= 0 && i + d[0] * k < nav.resolution() && j + d[1] * k < nav.resolution())
                    ++k;
                if (k >= 2 && k <= 4 && nav.walkable(i + d[0] * k, j + d[1] * k))
                {
                    wallDi = d[0];
                    wallDj = d[1];
                    thickness = k - 1;
                    return true;
                }
            }
            return false;
        },
        200000);
    if (!path)
        fail("no thin wall (1-3 blocked cells with ground behind) reachable from the spawn");
    if (walkPath(bot, t, self, *path, true) != 0)
        fail("corrections on the way to the wall");
    const Point front = self.p();
    const int fi = nav.cellIndex(front.x), fj = nav.cellIndex(front.z);

    // Into the wall (1 m, well within the speed allowance).
    bot.pumpFor(Ms(1300));
    std::size_t before = bot.view().corrections.size();
    Self probe = self;
    sendMove(bot, t, probe, nav.centre(fi + wallDi, fj + wallDj), game::kMoveMoving);
    expectAt(waitCorrection(bot, before, Ms(1000), "moving into a blocked cell"), front, "into the wall");

    // Through the wall: both end points walkable, the straight path crosses blocked cells.
    bot.pumpFor(Ms(1300));
    before = bot.view().corrections.size();
    probe = self;
    const Point behind = nav.centre(fi + wallDi * (thickness + 1), fj + wallDj * (thickness + 1));
    sendMove(bot, t, probe, behind, game::kMoveMoving | game::kMoveRunning);
    expectAt(waitCorrection(bot, before, Ms(1000), "tunnelling through the wall"), front, "through the wall");

    // Out of the zone.
    bot.pumpFor(Ms(300));
    before = bot.view().corrections.size();
    probe = self;
    sendMove(bot, t, probe, {-0.5f, front.z}, game::kMoveMoving);
    expectAt(waitCorrection(bot, before, Ms(1000), "moving outside the zone"), front, "outside the zone");

    expectNoViewErrors(bot, "wallhack view");
    bot.send(vn::Logout{});
    bot.expectClosed(true, bot.timeout());
    return std::format("walked {} path points to ({:.1f}, {:.1f}); a {}-cell wall: move into it, tunnel through "
                       "it ({:.1f} m) and leaving the zone were all corrected",
                       path->size(), front.x, front.z, thickness, distance(front, behind));
}

// ---- floodmove: more than 30 MOVE_REQ per second ----

std::string scenarioFloodMove(const Context& c)
{
    const auto& t = terrainOf(c);
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    Self self;
    (void)enterWorld(bot, createCharacter(bot, c.opts.race), t, self);
    for (int i = 0; i < 40; ++i)
        sendMove(bot, t, self, self.p(), game::kMoveMoving);
    const Received m = bot.next("waiting for ERROR(Flood)");
    vn::Error err;
    if (m.opcode != static_cast<std::uint16_t>(vn::Opcode::Error) || !vn::decodeMessage(m.payload, err))
        fail(std::format("got {} instead of ERROR", vn::opcodeName(static_cast<vn::Opcode>(m.opcode))));
    if (err.errorCode != vn::ErrorCode::Flood)
        fail(std::format("ERROR code {} instead of Flood", vn::errorCodeName(err.errorCode)));
    bot.expectClosed(true, bot.timeout());
    return std::format("40 MOVE_REQ in a burst -> ERROR(Flood '{}') and close", err.debugMessage);
}

// ---- leaverace: MOVE_REQ right behind LEAVE_WORLD_REQ / LOGOUT must not get the bot kicked ----

std::string scenarioLeaveRace(const Context& c)
{
    const auto& t = terrainOf(c);
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    const std::uint32_t charId = createCharacter(bot, c.opts.race);
    Self self;
    (void)enterWorld(bot, charId, t, self);
    const auto dir = straightDirection(t, self.p(), 6.0f);
    if (!dir)
        fail("no straight walkable direction near the spawn");
    const std::uint8_t walking = game::kMoveMoving;
    auto stepAlong = [&](float metres) {
        sendMove(bot, t, self, {self.x + dir->x * metres, self.z + dir->z * metres}, walking);
    };
    // Walk a little (well inside the movement allowance), then leave while "still walking": the
    // moves the client had already sent arrive after LEAVE_WORLD_REQ.
    bot.pumpFor(Ms(400));
    for (int i = 0; i < 3; ++i)
    {
        stepAlong(0.15f);
        bot.pumpFor(kStep);
    }
    bot.send(vn::LeaveWorldReq{});
    for (int i = 0; i < 3; ++i)
        stepAlong(0.15f);
    if (bot.expect<vn::LeaveWorldRes>().result != vn::kLeaveWorldOk)
        fail("LEAVE_WORLD_RES not ok");
    stepAlong(0.15f); // one more inside the 2 s grace window, now in character select
    if (listChars(bot).empty())
        fail("character list empty after leaving the world");
    expectNoViewErrors(bot, "leaverace");
    bot.trackWorld(false);

    // The same with LOGOUT: the server saves and closes without any ERROR.
    (void)enterWorld(bot, charId, t, self);
    bot.pumpFor(Ms(300));
    stepAlong(0.15f);
    bot.send(vn::Logout{});
    stepAlong(0.15f);
    stepAlong(0.15f);
    bot.expectClosed(false, bot.timeout());
    return "3 MOVE_REQ after LEAVE_WORLD_REQ + 1 in character select dropped, 2 after LOGOUT: no ERROR";
}

// ---- dupworld: duplicate login while in the world, seen by a third player ----

std::string scenarioDupWorld(const Context& c)
{
    const auto& t = terrainOf(c);
    Bot a(c.opts.timeout);
    Bot watcher(c.opts.timeout);
    a.connectOrFail(c.settings);
    watcher.connectOrFail(c.settings);
    const std::string user = truncUser(c.user, "d");
    loginAs(a, user, c.opts.password);
    loginAs(watcher, truncUser(c.user, "w"), c.opts.password);
    const std::uint32_t charA = createCharacter(a, c.opts.race);
    const std::uint32_t charW = createCharacter(watcher, c.opts.race);
    Self sa, sw;
    const auto enterA = enterWorld(a, charA, t, sa);
    (void)enterWorld(watcher, charW, t, sw);
    const auto pumpWatcher = [&] { watcher.poll(); };
    if (!watcher.pumpUntil([&] { return watcher.view().entities.contains(enterA.entityId); }, a.timeout()))
        fail("the watcher never saw the first session's character spawn");

    // A walks somewhere: its live position is what the second login must continue from.
    if (walkPath(a, t, sa, pathAway(t, sa.p(), 6.0f), false, pumpWatcher) != 0)
        fail("corrections while walking a legal path");
    a.pumpFor(Ms(200));

    // Same account from a second connection while A is in the world.
    Bot b(c.opts.timeout);
    b.connectOrFail(c.settings);
    loginAs(b, user, c.opts.password);
    const Received kicked = a.next("waiting for ERROR(KickedDuplicateLogin)");
    vn::Error err;
    if (kicked.opcode != static_cast<std::uint16_t>(vn::Opcode::Error) || !vn::decodeMessage(kicked.payload, err) ||
        err.errorCode != vn::ErrorCode::KickedDuplicateLogin)
        fail(std::format("first session got {} instead of ERROR(KickedDuplicateLogin)",
                         vn::opcodeName(static_cast<vn::Opcode>(kicked.opcode))));
    a.expectClosed(true, a.timeout());
    if (!watcher.pumpUntil([&] { return !watcher.view().entities.contains(enterA.entityId); }, a.timeout()))
        fail("the watcher still sees the kicked session's entity");

    // The new session enters exactly where the old one stood, as one new entity.
    const auto chars = listChars(b);
    if (chars.size() != 1 || chars[0].charId != charA)
        fail("the second session does not list the character");
    Self sb;
    const auto enterB = enterWorld(b, charA, t, sb);
    if (std::abs(enterB.x - sa.x) > kPosEps || std::abs(enterB.z - sa.z) > kPosEps)
        fail(std::format("second session entered at ({:.2f}, {:.2f}), the first stood at ({:.2f}, {:.2f})", enterB.x,
                         enterB.z, sa.x, sa.z));
    if (!watcher.pumpUntil([&] { return watcher.view().entities.contains(enterB.entityId); }, b.timeout()))
        fail("the watcher never saw the second session's character spawn");
    std::size_t sameName = 0;
    for (const auto& [id, r] : watcher.view().entities)
        sameName += r.spawn.name == watcher.view().entities.at(enterB.entityId).spawn.name ? 1 : 0;
    if (sameName != 1)
        fail(std::format("the watcher sees the character {} times", sameName));
    expectNoViewErrors(watcher, "watcher");
    expectNoViewErrors(b, "second session");

    b.send(vn::LeaveWorldReq{});
    if (b.expect<vn::LeaveWorldRes>().result != vn::kLeaveWorldOk)
        fail("LEAVE_WORLD_RES not ok");
    b.send(vn::Logout{});
    watcher.send(vn::Logout{});
    return std::format("kicked in world at ({:.1f}, {:.1f}); re-entered there as entity {} (was {}); watcher saw "
                       "despawn + one spawn",
                       sa.x, sa.z, enterB.entityId, enterA.entityId);
}

// ---- aoi: two bots see each other spawn, move (<= 10 Hz, final stop), leave, despawn ----

std::string scenarioAoi(const Context& c)
{
    const auto& t = terrainOf(c);
    Bot a(c.opts.timeout);
    Bot b(c.opts.timeout);
    a.connectOrFail(c.settings);
    b.connectOrFail(c.settings);
    loginAs(a, truncUser(c.user, "a"), c.opts.password);
    loginAs(b, truncUser(c.user, "b"), c.opts.password);
    const std::uint32_t charA = createCharacter(a, 1);
    const std::uint32_t charB = createCharacter(b, 1);
    Self sa, sb;
    const auto enterA = enterWorld(a, charA, t, sa);
    a.pumpFor(Ms(200));
    if (!a.view().entities.empty())
        fail("A sees entities before B entered (another test running on this server?)");
    auto enterB = enterWorld(b, charB, t, sb);
    const auto pumpBoth = [&] {
        a.poll();
        b.poll();
    };
    const auto idleA = [&] {
        a.poll();
        a.keepAlive();
    };

    // 1. Mutual spawn.
    if (!a.pumpUntil([&] { return a.view().entities.contains(enterB.entityId); }, Ms(2000)))
        fail("A did not get ENTITY_SPAWN for B");
    if (!b.pumpUntil([&] { return b.view().entities.contains(enterA.entityId); }, Ms(2000)))
        fail("B did not get ENTITY_SPAWN for A after its ENTER_WORLD_RES");
    {
        const auto& s = a.view().entities.at(enterB.entityId).spawn;
        if (s.entityType != vn::kEntityTypePlayer || s.race != 1 || s.level != 1 ||
            distance({s.x, s.z}, sb.p()) > kPosEps)
            fail("ENTITY_SPAWN of B has wrong fields");
    }

    // 2. B moves at 20 Hz for 2 s; A must get ENTITY_MOVE for B at most 10 times per second.
    const auto movesBefore = a.view().moves[enterB.entityId].size();
    const auto path = pathAway(t, sb.p(), 6.0f);
    const auto t0 = Clock::now();
    {
        const float speed = game::kWalkSpeed * kSpeedSafety;
        sendMove(b, t, sb, sb.p(), game::kMoveMoving);
        auto last = Clock::now();
        std::size_t next = 1;
        while (Clock::now() - t0 < Ms(2000))
        {
            waitPumping(b, Ms(50), idleA);
            const auto now = Clock::now();
            float budget = speed * std::chrono::duration<float>(now - last).count();
            last = now;
            Point pos = sb.p();
            if (next < path.size())
            {
                const float d = distance(pos, path[next]);
                if (d <= budget)
                    pos = path[next++];
                else
                {
                    pos.x += (path[next].x - pos.x) * budget / d;
                    pos.z += (path[next].z - pos.z) * budget / d;
                }
            }
            sendMove(b, t, sb, pos, game::kMoveMoving);
        }
    }
    const auto window = std::chrono::duration<double>(Clock::now() - t0).count();
    // 3. Stop: the final state must reach A promptly.
    sendMove(b, t, sb, sb.p(), 0);
    const Point stopAt = sb.p();
    const auto stoppedAt = Clock::now();
    if (!a.pumpUntil(
            [&] {
                b.poll();
                const auto it = a.view().entities.find(enterB.entityId);
                if (it == a.view().entities.end() || !it->second.last)
                    return false;
                const auto& last = *it->second.last;
                return (last.flags & game::kMoveMoving) == 0 && distance({last.x, last.z}, stopAt) < kPosEps;
            },
            Ms(1000)))
        fail("A did not receive B's final stopped state");
    const double stopLatencyMs = std::chrono::duration<double, std::milli>(Clock::now() - stoppedAt).count();
    const auto& hist = a.view().moves[enterB.entityId];
    const std::size_t relayed = hist.size() - movesBefore;
    const auto maxAllowed = static_cast<std::size_t>(std::ceil((window + 0.4) * game::kEntityMoveRelayHz)) + 1;
    if (relayed > maxAllowed)
        fail(std::format("A got {} ENTITY_MOVE for B in {:.2f} s (more than 10 Hz)", relayed, window));
    if (relayed < 8)
        fail(std::format("A got only {} ENTITY_MOVE for B in {:.2f} s", relayed, window));
    for (std::size_t i = movesBefore + 1; i < hist.size(); ++i)
        if (hist[i].move.serverTimeMs < hist[i - 1].move.serverTimeMs)
            fail("ENTITY_MOVE server_time_ms went backwards");
    if (!b.view().corrections.empty())
        fail("B got POSITION_CORRECT for legal 20 Hz moves");

    // 4. B leaves the world: A sees the despawn; B re-enters where it stopped and is spawned again.
    b.send(vn::LeaveWorldReq{});
    if (b.expect<vn::LeaveWorldRes>().result != vn::kLeaveWorldOk)
        fail("B: LEAVE_WORLD_RES not ok");
    if (!a.pumpUntil([&] { return !a.view().entities.contains(enterB.entityId); }, Ms(2000)))
        fail("A did not get ENTITY_DESPAWN when B left the world");
    b.trackWorld(false);
    b.view() = {};
    Self sb2;
    enterB = enterWorld(b, charB, t, sb2);
    if (distance(sb2.p(), stopAt) > kPosEps)
        fail("B re-entered somewhere else than where it stopped");
    sb = sb2;
    if (!a.pumpUntil([&] { return a.view().entities.contains(enterB.entityId); }, Ms(2000)))
        fail("A did not get ENTITY_SPAWN when B re-entered");

    // 5. B runs out of A's 5x5 cells: both sides get the despawn. Then back in: both get the spawn.
    const Nav nav(t);
    const Point pa = sa.p();
    auto out = nav.bfs(
        sb.p(), [&](int i, int j) { return aoiDistance(nav.centre(i, j), pa) >= 3; }, 250000);
    if (!out)
        fail("no walkable path out of A's area of interest");
    if (walkPath(b, t, sb, *out, true, idleA) != 0)
        fail("B got corrections while running out of range");
    if (!a.pumpUntil([&] { return !a.view().entities.contains(enterB.entityId); }, Ms(2000)))
        fail("A still sees B after B left its area of interest");
    if (!b.pumpUntil([&] { return !b.view().entities.contains(enterA.entityId); }, Ms(2000)))
        fail("B still sees A after leaving A's area of interest (asymmetric)");
    auto back = nav.bfs(
        sb.p(), [&](int i, int j) { return aoiDistance(nav.centre(i, j), pa) <= 2; }, 250000);
    if (!back)
        fail("no walkable path back into A's area of interest");
    if (walkPath(b, t, sb, *back, true, idleA) != 0)
        fail("B got corrections while running back");
    if (!a.pumpUntil([&] { return a.view().entities.contains(enterB.entityId); }, Ms(2000)))
        fail("A did not see B again after it came back into range");
    if (!b.pumpUntil([&] { return b.view().entities.contains(enterA.entityId); }, Ms(2000)))
        fail("B did not see A again after coming back into range");
    pumpBoth();
    expectNoViewErrors(a, "A's view");
    expectNoViewErrors(b, "B's view");

    a.send(vn::Logout{});
    b.send(vn::Logout{});
    a.expectClosed(true, a.timeout());
    b.expectClosed(true, b.timeout());
    return std::format("mutual spawn; {} ENTITY_MOVE in {:.2f} s for 40 MOVE_REQ (<= 10 Hz); stop seen after "
                       "{:.0f} ms; despawn on leave, spawn on re-enter; despawn/spawn both ways when leaving and "
                       "re-entering the 5x5 cells",
                       relayed, window, stopLatencyMs);
}

// ---- persist: walk, logout (or server shutdown), login again -> same position ----

namespace {

struct PersistState
{
    std::uint32_t charId = 0;
    float x = 0.0f;
    float z = 0.0f;
};

void writeState(const std::string& file, const PersistState& s)
{
    if (file.empty())
        return;
    std::ofstream out(file, std::ios::trunc);
    out << s.charId << ' ' << std::format("{:.6f} {:.6f}", s.x, s.z) << '\n';
    if (!out)
        fail("cannot write --state-file " + file);
}

PersistState readState(const std::string& file)
{
    std::ifstream in(file);
    PersistState s;
    if (!(in >> s.charId >> s.x >> s.z))
        fail("cannot read --state-file " + file);
    return s;
}

std::uint32_t firstOrNewCharacter(Bot& bot, int race)
{
    const auto chars = listChars(bot);
    return chars.empty() ? createCharacter(bot, race) : chars.front().charId;
}

Point enterAndCheck(const Context& c, const PersistState& expected)
{
    const auto& t = terrainOf(c);
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    const auto chars = listChars(bot);
    if (std::none_of(chars.begin(), chars.end(), [&](const vn::CharSummary& s) { return s.charId == expected.charId; }))
        fail(std::format("character {} is not in the list after logging in again", expected.charId));
    Self self;
    (void)enterWorld(bot, expected.charId, t, self);
    if (std::abs(self.x - expected.x) > kPosEps || std::abs(self.z - expected.z) > kPosEps)
        fail(std::format("entered at ({:.3f}, {:.3f}) instead of the saved ({:.3f}, {:.3f})", self.x, self.z,
                         expected.x, expected.z));
    bot.send(vn::Logout{});
    bot.expectClosed(true, bot.timeout());
    return self.p();
}

} // namespace

std::string scenarioPersist(const Context& c)
{
    const std::string phase = c.opts.phase.empty() ? "walk" : c.opts.phase;
    if (phase == "check")
    {
        const PersistState s = readState(c.opts.stateFile);
        const Point p = enterAndCheck(c, s);
        return std::format("character {} entered at the saved position ({:.2f}, {:.2f})", s.charId, p.x, p.z);
    }
    if (phase != "walk" && phase != "stay")
        fail("unknown --phase '" + phase + "' (walk | stay | check)");

    const auto& t = terrainOf(c);
    PersistState s;
    {
        Bot bot(c.opts.timeout);
        bot.connectOrFail(c.settings);
        loginAs(bot, c.user, c.opts.password);
        s.charId = firstOrNewCharacter(bot, c.opts.race);
        Self self;
        (void)enterWorld(bot, s.charId, t, self);
        if (walkPath(bot, t, self, pathAway(t, self.p(), 15.0f), true) != 0)
            fail("corrections while walking");
        s.x = self.x;
        s.z = self.z;
        writeState(c.opts.stateFile, s);
        if (phase == "stay")
        {
            // Stay in the world until the server shuts down: it must save the position on the way out.
            std::printf("in world at (%.2f, %.2f), waiting for the server to shut down\n", s.x, s.z);
            std::fflush(stdout);
            bot.trackWorld(true);
            if (!bot.pumpUntil(
                    [&] {
                        bot.keepAlive();
                        return bot.closed();
                    },
                    Ms(120000)))
                fail("server did not shut down");
            return std::format("character {} stayed in world at ({:.2f}, {:.2f}) until the server stopped", s.charId,
                               s.x, s.z);
        }
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
    }
    const Point p = enterAndCheck(c, s);
    return std::format("walked to ({:.2f}, {:.2f}), logged out, logged in again and entered at ({:.2f}, {:.2f})", s.x,
                       s.z, p.x, p.z);
}

} // namespace vo::tools::botclient
