// World scenarios of the bot client (docs/SERVER_WORLD.md §9): legal walking, speed hack, wall hack,
// MOVE_REQ flood, area of interest between two bots, position persistence. All moves are planned on
// the same zone terrain the server uses (--terrain, default assets/terrain/zone1/zone1.vterrain).

#include "shared/game/Combat.h"
#include "shared/game/Movement.h"
#include "shared/game/PathFind.h"
#include "shared/game/TerrainWalk.h"
#include "tools/botclient/Bot.h"
#include "tools/botclient/Nav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
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
    // PROTOCOL_ITEMS.md §2: INVENTORY right after ENTER_WORLD_RES, before the first ENTITY_SPAWN.
    if (!bot.pumpUntil([&] { return v.inventory.has_value() || bot.closed(); }, bot.timeout()) || !v.inventory)
        fail("no INVENTORY after ENTER_WORLD_RES");
    if (v.inventoryIndex != bot.lastIndex() + 1)
        fail("INVENTORY did not come right after ENTER_WORLD_RES");
    if (v.firstSpawnIndex != 0 && v.firstSpawnIndex < v.inventoryIndex)
        fail("ENTITY_SPAWN arrived before INVENTORY");
    if (v.inventory->bagSize != game::kBagSlots)
        fail(std::format("INVENTORY bag_size {}", v.inventory->bagSize));
    // PROTOCOL_LOOT.md §4.3: then COINS (gained 0), still before the first ENTITY_SPAWN.
    if (!bot.pumpUntil([&] { return !v.coins.empty() || bot.closed(); }, bot.timeout()) || v.coins.empty())
        fail("no COINS after INVENTORY");
    if (v.firstCoinsIndex != v.inventoryIndex + 1 || v.coins.front().gained != 0)
        fail("COINS did not come right after INVENTORY with gained 0");
    if (v.firstSpawnIndex != 0 && v.firstSpawnIndex < v.firstCoinsIndex)
        fail("ENTITY_SPAWN arrived before COINS");
    self = {enter.x, enter.y, enter.z, enter.yaw};
    return enter;
}

// ITEM_MOVE_REQ and its ITEM_MOVE_RES; on Ok exactly one ITEM_UPDATE must have come first, on a
// failure none. Returns the result; `update` receives the ITEM_UPDATE of an Ok move.
vn::ItemMoveResult moveItem(Bot& bot, game::ItemPlace from, game::ItemPlace to, vn::ItemUpdate* update = nullptr)
{
    const std::size_t updatesBefore = bot.view().itemUpdates.size();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::ItemMoveReq{seq, from.container, from.slot, to.container, to.slot});
    const auto res = bot.expect<vn::ItemMoveRes>();
    if (res.seq != seq)
        fail(std::format("ITEM_MOVE_RES seq {} (expected {})", res.seq, seq));
    const std::size_t updates = bot.view().itemUpdates.size() - updatesBefore;
    const bool ok = res.result == vn::ItemMoveResult::Ok;
    if (updates != (ok ? 1u : 0u))
        fail(std::format("{} ITEM_UPDATE(s) for a move with result {}", updates, vn::itemMoveResultName(res.result)));
    if (ok && bot.view().lastItemUpdateIndex > bot.lastIndex())
        fail("ITEM_UPDATE arrived after ITEM_MOVE_RES");
    if (ok && update)
        *update = bot.view().itemUpdates.back();
    return res.result;
}

// Waits for the next ENTITY_EQUIP about `entity` (after `seen` of them were already received).
vn::EntityEquip waitEquip(Bot& bot, std::uint32_t entity, std::size_t& seen)
{
    std::optional<vn::EntityEquip> found;
    if (!bot.pumpUntil(
            [&] {
                auto& eq = bot.view().equips;
                while (seen < eq.size())
                    if (eq[seen++].entityId == entity)
                    {
                        found = eq[seen - 1];
                        return true;
                    }
                return false;
            },
            bot.timeout()))
        fail(std::format("no ENTITY_EQUIP for entity {}", entity));
    return *found;
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
    if (a.view().count(vn::kEntityTypePlayer) != 0) // monsters around the village are fine (v3)
        fail("A sees players before B entered (another test running on this server?)");
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

// ---- items: starter kit, equip / unequip, ENTITY_EQUIP, persistence (docs/PROTOCOL_ITEMS.md) ----

namespace {

using game::EquipSlot;
using game::ItemPlace;

std::uint16_t inventoryTemplateAt(const vn::Inventory& inv, ItemPlace p)
{
    for (const auto& e : inv.items)
        if (e.container == p.container && e.slot == p.slot)
            return e.templateId;
    return 0;
}

void expectEquip(const vn::EntityEquip& e, std::uint16_t main, std::uint16_t off, std::string_view what)
{
    if (e.mainHandTemplate != main || e.offHandTemplate != off)
        fail(std::format("{}: ENTITY_EQUIP ({}, {}), expected ({}, {})", what, e.mainHandTemplate, e.offHandTemplate,
                         main, off));
}

} // namespace

std::string scenarioItems(const Context& c)
{
    const auto& t = terrainOf(c);
    constexpr std::uint16_t kSword = 1, kShield = 2, kAxe = 6, kGiven = 7;
    const ItemPlace mainHand = ItemPlace::equipment(EquipSlot::MainHand);
    const ItemPlace offHand = ItemPlace::equipment(EquipSlot::OffHand);
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);

    if (c.opts.phase == "check")
    {
        // After `vo_admin give-item <name> 7`: the kit is still there once, plus the new item.
        std::ifstream in(c.opts.stateFile);
        std::string name;
        if (!(in >> name))
            fail("cannot read the character name from --state-file");
        const auto chars = listChars(bot);
        const auto it = std::find_if(chars.begin(), chars.end(), [&](const vn::CharSummary& s) { return s.name == name; });
        if (it == chars.end())
            fail("character '" + name + "' not listed");
        if (it->mainHandTemplate != kSword || it->offHandTemplate != kShield)
            fail(std::format("list shows ({}, {}), expected sword and shield", it->mainHandTemplate, it->offHandTemplate));
        Self self;
        (void)enterWorld(bot, it->charId, t, self);
        const auto& inv = *bot.view().inventory;
        if (inv.items.size() != 4)
            fail(std::format("INVENTORY has {} items after give-item (expected 4)", inv.items.size()));
        const bool given = std::any_of(inv.items.begin(), inv.items.end(),
                                       [&](const vn::ItemEntry& e) { return e.container == 1 && e.templateId == kGiven; });
        if (!given)
            fail("the given item (template 7) is not in the bag");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("character '{}' has its kit once plus the given item", name);
    }

    // A warrior: listed with the kit before entering.
    const std::uint32_t charId = createCharacter(bot, c.opts.race);
    const auto listed = listChars(bot);
    const auto me = std::find_if(listed.begin(), listed.end(), [&](const vn::CharSummary& s) { return s.charId == charId; });
    if (me == listed.end() || me->mainHandTemplate != kSword || me->offHandTemplate != kShield)
        fail("the character list does not show the starter kit before the first entry");
    const std::string name = me->name;

    Self self;
    const auto enter = enterWorld(bot, charId, t, self);
    const vn::Inventory kit = *bot.view().inventory;
    if (kit.items.size() != 3 || inventoryTemplateAt(kit, mainHand) != kSword ||
        inventoryTemplateAt(kit, offHand) != kShield || inventoryTemplateAt(kit, ItemPlace::bag(0)) != kAxe)
        fail("INVENTORY is not the warrior's starter kit");

    std::size_t equipsSeen = 0;
    vn::ItemUpdate update;
    // Two-handed axe: the shield moves to the bag (4 slots change).
    if (const auto r = moveItem(bot, ItemPlace::bag(0), mainHand, &update); r != vn::ItemMoveResult::Ok)
        fail(std::format("equipping the axe: {}", vn::itemMoveResultName(r)));
    if (update.items.size() != 4)
        fail(std::format("ITEM_UPDATE with {} entries for the axe (expected 4)", update.items.size()));
    expectEquip(waitEquip(bot, enter.entityId, equipsSeen), kAxe, 0, "axe");
    // Refused moves change nothing and send no ITEM_UPDATE.
    if (const auto r = moveItem(bot, ItemPlace::bag(1), offHand); r != vn::ItemMoveResult::SlotBlocked)
        fail(std::format("shield next to the axe: {} (expected SlotBlocked)", vn::itemMoveResultName(r)));
    if (const auto r = moveItem(bot, ItemPlace::bag(1), mainHand); r != vn::ItemMoveResult::WrongSlot)
        fail(std::format("shield into MainHand: {} (expected WrongSlot)", vn::itemMoveResultName(r)));
    if (const auto r = moveItem(bot, ItemPlace::bag(9), ItemPlace::bag(10)); r != vn::ItemMoveResult::NoItem)
        fail(std::format("empty slot: {} (expected NoItem)", vn::itemMoveResultName(r)));
    // Back to sword and shield; the axe ends in bag 5.
    if (moveItem(bot, mainHand, ItemPlace::bag(5)) != vn::ItemMoveResult::Ok)
        fail("unequipping the axe failed");
    expectEquip(waitEquip(bot, enter.entityId, equipsSeen), 0, 0, "unequipped");
    if (moveItem(bot, ItemPlace::bag(0), mainHand) != vn::ItemMoveResult::Ok ||
        moveItem(bot, ItemPlace::bag(1), offHand) != vn::ItemMoveResult::Ok)
        fail("re-equipping sword and shield failed");
    expectEquip(waitEquip(bot, enter.entityId, equipsSeen), kSword, 0, "sword");
    expectEquip(waitEquip(bot, enter.entityId, equipsSeen), kSword, kShield, "sword and shield");

    // Leave and enter again: the stored state, not a second kit.
    bot.send(vn::LeaveWorldReq{});
    if (bot.expect<vn::LeaveWorldRes>().result != vn::kLeaveWorldOk)
        fail("LEAVE_WORLD_RES not ok");
    bot.trackWorld(false);
    (void)enterWorld(bot, charId, t, self);
    const auto& again = *bot.view().inventory;
    if (again.items.size() != 3 || inventoryTemplateAt(again, mainHand) != kSword ||
        inventoryTemplateAt(again, offHand) != kShield || inventoryTemplateAt(again, ItemPlace::bag(5)) != kAxe)
        fail("INVENTORY after re-entering does not match the moves");
    for (const auto& e : again.items)
        if (std::none_of(kit.items.begin(), kit.items.end(), [&](const vn::ItemEntry& k) { return k.itemId == e.itemId; }))
            fail("re-entering created new items");
    expectNoViewErrors(bot, "items view");
    if (!c.opts.stateFile.empty())
    {
        std::ofstream out(c.opts.stateFile, std::ios::trunc);
        out << name << "\n";
        if (!out)
            fail("cannot write --state-file");
    }
    bot.send(vn::Logout{});
    bot.expectClosed(true, bot.timeout());
    return std::format("character '{}': kit listed and granted once, 7 moves checked, ENTITY_EQUIP x4, stored", name);
}

// ---- combat: fight a Kül Çömezi, die to the Kor Bekçisi, respawn (docs/PROTOCOL_COMBAT.md) ----

namespace {

bool isDeadOf(const Bot& bot, std::uint32_t entity)
{
    const auto& d = const_cast<Bot&>(bot).view().deaths;
    return std::any_of(d.begin(), d.end(), [&](const vn::EntityDeath& e) { return e.entityId == entity; });
}

// Where `entity` is now (its last ENTITY_MOVE, else its spawn); nullopt if not spawned.
std::optional<Point> positionOf(Bot& bot, std::uint32_t entity)
{
    const auto it = bot.view().entities.find(entity);
    if (it == bot.view().entities.end())
        return std::nullopt;
    if (it->second.last)
        return Point{it->second.last->x, it->second.last->z};
    return Point{it->second.spawn.x, it->second.spawn.z};
}

// Walks (runs) towards a moving goal until within `stopAt` metres, legal moves only (straight
// segments checked against the walk mask, grid paths when blocked), at 90 % of the run speed. False
// if the bot died, `goal` became unknown or `timeout` passed. `onStep` runs every step.
bool approach(Bot& bot, const assets::TerrainData& t, Self& self, std::uint32_t me,
              const std::function<std::optional<Point>()>& goal, float stopAt, Ms timeout,
              const std::function<void()>& onStep = nullptr)
{
    const auto deadline = Clock::now() + timeout;
    std::vector<Point> path;
    std::size_t next = 0;
    Point planned{-1.0f, -1.0f};
    auto lastPlan = Clock::now() - Ms(10000);
    auto last = Clock::now();
    bool moving = false;
    const auto stop = [&] {
        if (moving)
            sendMove(bot, t, self, self.p(), game::kMoveRunning);
        moving = false;
    };
    while (Clock::now() < deadline)
    {
        waitPumping(bot, kStep, nullptr);
        bot.keepAlive();
        if (onStep)
            onStep();
        if (isDeadOf(bot, me))
            return false;
        const auto g = goal();
        if (!g)
        {
            stop();
            return false;
        }
        if (distance(self.p(), *g) <= stopAt)
        {
            stop();
            return true;
        }
        const auto now = Clock::now();
        if (next >= path.size() || distance(planned, *g) > 2.0f || now - lastPlan > Ms(1500))
        {
            lastPlan = now;
            planned = *g;
            path.clear();
            next = 0;
            if (game::isSegmentWalkable(t, self.x, self.z, g->x, g->z))
                path.push_back(*g);
            else if (auto found = game::findWalkPathNear(t, {self.x, self.z}, {g->x, g->z}, 3.0f, 0.5f, 0.3f))
                for (const auto& p : found->points)
                    path.push_back({p.x, p.z});
        }
        if (path.empty())
            continue;
        float budget = game::kRunSpeed * kSpeedSafety * std::chrono::duration<float>(now - last).count();
        if (!moving)
            budget = 0.0f; // the start packet goes out from where we stand
        last = now;
        // Never past a path vertex within one step: the server checks the straight line.
        Point pos = self.p();
        if (budget > 1e-4f && next < path.size())
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
        sendMove(bot, t, self, pos, game::kMoveMoving | game::kMoveRunning);
        moving = true;
    }
    stop();
    return false;
}

std::uint32_t sumDamage(const std::vector<vn::CombatEvent>& events, std::uint32_t source, std::uint32_t target)
{
    std::uint32_t sum = 0;
    for (const auto& e : events)
        if (e.sourceId == source && e.targetId == target && e.kind == vn::kCombatKindDamage)
            sum += e.amount;
    return sum;
}

// Waits out the respawn delay, respawns at the race spawn and forgets the own death (so the next
// fight can run). Fails if the server refuses. A level-1 warrior can lose to an unlucky pull (a
// second thrall joining): the scenarios check the protocol, not the fight, so they respawn and try
// another one.
void respawn(Bot& bot, Self& self, std::uint32_t me)
{
    bot.pumpFor(std::chrono::duration_cast<Ms>(game::kRespawnMinDelay + Ms(400)));
    bot.dropPongs();
    bot.send(vn::RespawnReq{});
    const auto res = bot.expect<vn::RespawnRes>();
    if (res.result != vn::RespawnResult::Ok)
        fail(std::format("RESPAWN_REQ result {}", static_cast<int>(res.result)));
    self = {res.x, res.y, res.z, res.yaw};
    std::erase_if(bot.view().deaths, [&](const vn::EntityDeath& d) { return d.entityId == me; });
    bot.pumpFor(Ms(300));
}

} // namespace

std::string scenarioCombat(const Context& c)
{
    const auto& t = terrainOf(c);
    if (!c.monsters || !c.items)
        fail("gamedata/monsters.toml and gamedata/items.toml are needed");
    const game::MonsterData& data = *c.monsters;
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    if (c.opts.phase == "check")
    {
        // After `vo_admin set-level <name> 99` (clamped to the cap): entered at level 10, xp 0, full hp/mp.
        std::ifstream in(c.opts.stateFile);
        std::string name;
        if (!(in >> name))
            fail("cannot read the character name from --state-file");
        const auto chars = listChars(bot);
        const auto it = std::find_if(chars.begin(), chars.end(), [&](const vn::CharSummary& s) { return s.name == name; });
        if (it == chars.end())
            fail("character '" + name + "' not listed");
        if (it->level != game::kLevelCap)
            fail(std::format("character list shows level {} (expected {})", it->level, game::kLevelCap));
        Self self;
        const auto e = enterWorld(bot, it->charId, t, self);
        game::Inventory inv;
        (void)applyItemEntries(inv, bot.view().inventory->items);
        const game::Stats s = game::deriveStats(it->charClass, game::kLevelCap, game::equipmentBonus(*c.items, inv));
        if (e.level != game::kLevelCap || e.xp != 0 || e.xpToNext != 0 || e.hp != s.maxHp || e.maxHp != s.maxHp ||
            e.mp != s.maxMp || e.attack != s.attack)
            fail(std::format("entered at level {} xp {}/{} hp {}/{} mp {} atk {} (expected level {}, hp {}, mp {}, atk {})",
                             e.level, e.xp, e.xpToNext, e.hp, e.maxHp, e.mp, e.attack, game::kLevelCap, s.maxHp,
                             s.maxMp, s.attack));
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("'{}' entered at level {} with {} hp, {} mp, attack {}", name, e.level, e.hp, e.mp, e.attack);
    }
    const std::uint32_t charId = createCharacter(bot, c.opts.race); // a warrior
    if (!c.opts.stateFile.empty())
    {
        const auto listed = listChars(bot);
        const auto me = std::find_if(listed.begin(), listed.end(), [&](const vn::CharSummary& s) { return s.charId == charId; });
        std::ofstream out(c.opts.stateFile, std::ios::trunc);
        out << (me != listed.end() ? me->name : std::string("?")) << "\n";
        if (!out)
            fail("cannot write --state-file");
    }
    Self self;
    const auto enter = enterWorld(bot, charId, t, self);
    const std::uint32_t me = enter.entityId;

    // ENTER_WORLD_RES carries the derived stats of a level-1 warrior with its starter kit.
    game::Inventory inv;
    (void)applyItemEntries(inv, bot.view().inventory->items);
    const game::Stats expected = game::deriveStats(1, 1, game::equipmentBonus(*c.items, inv));
    if (enter.level != 1 || enter.xp != 0 || enter.xpToNext != game::xpToNext(1) || enter.hp != expected.maxHp ||
        enter.maxHp != expected.maxHp || enter.mp != expected.maxMp || enter.maxMp != expected.maxMp ||
        enter.attack != expected.attack || enter.defense != expected.defense || enter.isDead)
        fail(std::format("ENTER_WORLD_RES combat fields level {} xp {}/{} hp {}/{} mp {}/{} atk {} def {} dead {} "
                         "(expected hp {} mp {} atk {} def {})",
                         enter.level, enter.xp, enter.xpToNext, enter.hp, enter.maxHp, enter.mp, enter.maxMp,
                         enter.attack, enter.defense, enter.isDead, expected.maxHp, expected.maxMp, expected.attack,
                         expected.defense));
    if (!bot.pumpUntil([&] { return bot.view().count(vn::kEntityTypeMonster) > 0; }, Ms(3000)))
        fail("no monster spawned around the village");

    // 1. The nearest level-1 Kül Çömezi: walk there, auto-attack, Ağır Darbe when ready.
    const game::MonsterTemplate* thrall = nullptr;
    for (const auto& m : data.templates())
        if (m.model == "ash_thrall" && m.level == 1)
            thrall = &m;
    if (!thrall)
        fail("no level-1 ash_thrall template");
    std::uint32_t target = 0;
    bool usedSkill = false;
    const game::SkillDef& heavy = *game::findSkill(game::kSkillHeavyBlow);
    std::set<std::uint32_t> abandoned; // fought, but the warrior died first
    int deathsToThralls = 0;
    for (int attempt = 0; attempt < 6 && !target; ++attempt)
    {
        // The nearest one, preferring one with no other monster within 10 m (pull one at a time).
        std::uint32_t pick = 0;
        float best = 1e9f;
        for (const auto& [id, r] : bot.view().entities)
        {
            if (r.spawn.entityType != vn::kEntityTypeMonster || r.spawn.templateId != thrall->id ||
                (r.spawn.state & vn::kEntityStateDead) != 0 || isDeadOf(bot, id) || abandoned.contains(id))
                continue;
            const Point at = *positionOf(bot, id);
            bool crowded = false;
            for (const auto& [other, o] : bot.view().entities)
                if (other != id && o.spawn.entityType == vn::kEntityTypeMonster && !isDeadOf(bot, other) &&
                    distance(at, *positionOf(bot, other)) < 10.0f)
                    crowded = true;
            const float d = distance(self.p(), at) + (crowded ? 1000.0f : 0.0f);
            if (d < best)
            {
                best = d;
                pick = id;
            }
        }
        if (!pick)
        {
            bot.pumpFor(Ms(1000));
            continue;
        }
        const auto goal = [&]() { return positionOf(bot, pick); };
        if (!approach(bot, t, self, me, goal, 1.8f, Ms(40000)))
            continue;
        bot.send(vn::AttackReq{pick});
        auto lastSkill = Clock::now() - Ms(10000);
        const auto fightEnd = Clock::now() + Ms(40000);
        while (Clock::now() < fightEnd && !isDeadOf(bot, pick) && !isDeadOf(bot, me))
        {
            waitPumping(bot, kStep, nullptr);
            bot.keepAlive();
            const auto pos = positionOf(bot, pick);
            if (!pos)
                break;
            if (distance(self.p(), *pos) > 2.4f)
            {
                (void)approach(bot, t, self, me, goal, 1.8f, Ms(10000));
                continue;
            }
            const std::uint32_t mp =
                bot.view().selfStats.empty() ? enter.mp : bot.view().selfStats.back().mp;
            // A refused try (the 500 ms global cooldown after a swing: ACTION_FAIL OnCooldown) is
            // repeated soon; after a hit the skill's own cooldown applies.
            const bool hitWithSkill =
                std::any_of(bot.view().combatEvents.begin(), bot.view().combatEvents.end(),
                            [&](const vn::CombatEvent& e) { return e.sourceId == me && e.skillId == heavy.id; });
            const Ms wait = hitWithSkill ? Ms(heavy.cooldownMs + 300) : Ms(700);
            if (mp >= heavy.mpCost && Clock::now() - lastSkill > wait)
            {
                bot.send(vn::SkillReq{heavy.id, pick});
                lastSkill = Clock::now();
            }
        }
        if (isDeadOf(bot, me))
        {
            // An unlucky pull: respawn and fight another one (this one's damage sum is spoilt).
            ++deathsToThralls;
            abandoned.insert(pick);
            respawn(bot, self, me);
            continue;
        }
        if (!isDeadOf(bot, pick))
            continue;
        target = pick;
        usedSkill = std::any_of(bot.view().combatEvents.begin(), bot.view().combatEvents.end(),
                                [&](const vn::CombatEvent& e) { return e.sourceId == me && e.skillId == heavy.id; });
    }
    if (!target)
        fail(std::format("could not kill a Kül Çömezi within 6 attempts ({} death(s))", deathsToThralls));
    bot.pumpFor(Ms(300));
    const auto& events = bot.view().combatEvents;
    // Consistency: my hits add up to its hp, each target_hp follows from the previous one, the last
    // one is the killing blow at 0.
    const std::uint32_t dealt = sumDamage(events, me, target);
    if (dealt != thrall->hp)
        fail(std::format("damage to the thrall adds up to {} (its hp is {})", dealt, thrall->hp));
    std::uint32_t hp = thrall->hp;
    std::size_t hits = 0;
    const vn::CombatEvent* lastHit = nullptr;
    for (const auto& e : events)
    {
        if (e.targetId != target)
            continue;
        if (e.sourceId != me || e.kind != vn::kCombatKindDamage)
            fail("someone else hit the thrall");
        if (e.targetHp != hp - e.amount)
            fail(std::format("COMBAT_EVENT target_hp {} after {} damage from {}", e.targetHp, e.amount, hp));
        hp = e.targetHp;
        ++hits;
        lastHit = &e;
    }
    if (!lastHit || lastHit->targetHp != 0 || (lastHit->flags & vn::kCombatFlagKillingBlow) == 0)
        fail("the last hit on the thrall is not a killing blow at 0 hp");
    const auto death = std::find_if(bot.view().deaths.begin(), bot.view().deaths.end(),
                                    [&](const vn::EntityDeath& d) { return d.entityId == target; });
    if (death->killerId != me)
        fail(std::format("ENTITY_DEATH names killer {} instead of {}", death->killerId, me));
    if (!bot.pumpUntil([&] { return !bot.view().xpGains.empty(); }, Ms(2000)))
        fail("no XP_GAIN after the kill");
    const auto& xp = bot.view().xpGains.front();
    if (xp.amount != thrall->xp || xp.xp != thrall->xp || xp.xpToNext != game::xpToNext(1) || xp.sourceEntityId != target)
        fail(std::format("XP_GAIN {} (xp {}/{}, source {}), expected {} from {}", xp.amount, xp.xp, xp.xpToNext,
                         xp.sourceEntityId, thrall->xp, target));
    if (!usedSkill)
        fail("Ağır Darbe was never used in the fight");
    // Hits taken: SELF_STATS hp follows the monsters' COMBAT_EVENTs.
    std::uint32_t lastHpSeen = enter.hp;
    for (const auto& e : events)
        if (e.targetId == me)
            lastHpSeen = e.targetHp;
    const std::size_t taken = std::count_if(events.begin(), events.end(), [&](const vn::CombatEvent& e) { return e.targetId == me; });

    // 2. Die on purpose to the elite, then respawn.
    const game::MonsterTemplate* elite = nullptr;
    const game::MonsterSpawn* eliteSpawn = nullptr;
    for (const auto& s : data.spawns())
        if (const auto* m = data.find(s.monster); m && m->elite && s.zone == 1)
        {
            elite = m;
            eliteSpawn = &s;
        }
    if (!elite)
        fail("no elite monster in zone 1");
    const auto eliteEntity = [&]() -> std::uint32_t {
        for (const auto& [id, r] : bot.view().entities)
            if (r.spawn.templateId == elite->id && (r.spawn.state & vn::kEntityStateDead) == 0)
                return id;
        return 0;
    };
    const auto towardsElite = [&]() -> std::optional<Point> {
        if (const std::uint32_t id = eliteEntity())
            return positionOf(bot, id);
        return Point{eliteSpawn->x, eliteSpawn->z};
    };
    std::uint32_t attacked = 0;
    const auto deathBy = Clock::now() + Ms(90000);
    while (!isDeadOf(bot, me) && Clock::now() < deathBy)
    {
        (void)approach(bot, t, self, me, towardsElite, 1.8f, Ms(20000), [&] {
            if (const std::uint32_t id = eliteEntity(); id && id != attacked)
            {
                bot.send(vn::AttackReq{id}); // out of range at first: the server keeps it armed
                attacked = id;
            }
        });
        waitPumping(bot, Ms(300), nullptr);
    }
    if (!isDeadOf(bot, me))
        fail("the warrior did not die to the Kor Bekçisi within 90 s");
    const auto diedAt = Clock::now();
    const auto myDeath = *std::find_if(bot.view().deaths.begin(), bot.view().deaths.end(),
                                       [&](const vn::EntityDeath& d) { return d.entityId == me; });
    const auto killing = std::find_if(bot.view().combatEvents.rbegin(), bot.view().combatEvents.rend(),
                                      [&](const vn::CombatEvent& e) { return e.targetId == me; });
    if (killing == bot.view().combatEvents.rend() || killing->targetHp != 0 ||
        (killing->flags & vn::kCombatFlagKillingBlow) == 0 || killing->sourceId != myDeath.killerId)
        fail("the hit that killed the warrior is not a killing blow at 0 hp by the killer");
    // Dead: a move is corrected, an early RESPAWN_REQ is TooEarly.
    const std::size_t correctionsBefore = bot.view().corrections.size();
    sendMove(bot, t, self, {self.x + 0.5f, self.z}, game::kMoveMoving);
    (void)waitCorrection(bot, correctionsBefore, Ms(2000), "moving while dead");
    self.x = bot.view().corrections.back().x;
    self.z = bot.view().corrections.back().z;
    bot.dropPongs();
    bot.send(vn::RespawnReq{});
    const auto early = bot.expect<vn::RespawnRes>();
    if (early.result != vn::RespawnResult::TooEarly)
        fail(std::format("RESPAWN_REQ right after death: result {} (expected TooEarly)", static_cast<int>(early.result)));
    bot.pumpUntil([] { return false; }, std::chrono::duration_cast<Ms>(game::kRespawnMinDelay + Ms(300) -
                                                                       (Clock::now() - diedAt)));
    bot.dropPongs();
    bot.send(vn::RespawnReq{});
    const auto res = bot.expect<vn::RespawnRes>();
    if (res.result != vn::RespawnResult::Ok)
        fail(std::format("RESPAWN_REQ after 5 s: result {}", static_cast<int>(res.result)));
    const auto* spawn = t.findSpawn(c.opts.race == 2 ? "spawn_kargan" : "spawn_aydor");
    const float fromSpawn = distance({res.x, res.z}, {spawn->position.x, spawn->position.z});
    const std::uint32_t half = static_cast<std::uint32_t>(std::lround(expected.maxHp * game::kRespawnHpFraction));
    if (fromSpawn > 2.5f || res.hp != half)
        fail(std::format("respawned {:.1f} m from the race spawn with {} hp (expected <= 2.5 m, {} hp)", fromSpawn,
                         res.hp, half));
    self = {res.x, res.y, res.z, res.yaw};
    if (!bot.pumpUntil(
            [&] {
                const auto& s = bot.view().states;
                return !s.empty() && s.back().entityId == me && (s.back().state & vn::kEntityStateDead) == 0;
            },
            Ms(2000)))
        fail("no ENTITY_STATE clearing the dead bit after the respawn");
    // Alive again: a legal step is accepted.
    const std::size_t before = bot.view().corrections.size();
    sendMove(bot, t, self, self.p(), 0);
    bot.pumpFor(Ms(400));
    if (bot.view().corrections.size() != before)
        fail("POSITION_CORRECT after the respawn");
    expectNoViewErrors(bot, "combat view");
    bot.send(vn::Logout{});
    bot.expectClosed(true, bot.timeout());
    std::string killer = "?";
    if (const auto it = bot.view().entities.find(myDeath.killerId); it != bot.view().entities.end())
        killer = it->second.spawn.name;
    return std::format("killed Kül Çömezi {} in {} hits ({} dmg, Ağır Darbe used; {} death(s) first), took {} hits (hp {}), +{} xp; "
                       "died to {} ({}); early respawn refused, respawned {:.1f} m from the spawn with {} hp",
                       target, hits, dealt, deathsToThralls, taken, lastHpSeen, xp.amount, killer, myDeath.killerId, fromSpawn,
                       res.hp);
}

// ---- loot: kill, open, take everything, consistency of the answers (docs/PROTOCOL_LOOT.md) ----

namespace {

// Units per template in a local inventory (bag and equipment).
std::map<std::uint16_t, std::uint32_t> unitTotals(const game::Inventory& inv)
{
    std::map<std::uint16_t, std::uint32_t> out;
    for (const auto& i : inv.items())
        out[i.templateId] += i.quantity;
    return out;
}

// Kills the nearest live level-1 Kül Çömezi with auto-attacks (no other monster within 10 m if
// possible). Returns its entity id and where it died, or 0 if none could be killed.
std::uint32_t killNearestThrall(Bot& bot, const assets::TerrainData& t, Self& self, std::uint32_t me,
                                const game::MonsterData& data, const std::set<std::uint32_t>& skip, Point& diedAt)
{
    const game::MonsterTemplate* thrall = nullptr;
    for (const auto& m : data.templates())
        if (m.model == "ash_thrall" && m.level == 1)
            thrall = &m;
    if (!thrall)
        fail("no level-1 ash_thrall template");
    std::set<std::uint32_t> abandoned; // fought, but the warrior died first
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        std::uint32_t pick = 0;
        float best = 1e9f;
        for (const auto& [id, r] : bot.view().entities)
        {
            if (r.spawn.entityType != vn::kEntityTypeMonster || r.spawn.templateId != thrall->id ||
                (r.spawn.state & vn::kEntityStateDead) != 0 || isDeadOf(bot, id) || skip.contains(id) ||
                abandoned.contains(id))
                continue;
            const Point at = *positionOf(bot, id);
            bool crowded = false;
            for (const auto& [other, o] : bot.view().entities)
                if (other != id && o.spawn.entityType == vn::kEntityTypeMonster && !isDeadOf(bot, other) &&
                    distance(at, *positionOf(bot, other)) < 10.0f)
                    crowded = true;
            const float d = distance(self.p(), at) + (crowded ? 1000.0f : 0.0f);
            if (d < best)
            {
                best = d;
                pick = id;
            }
        }
        if (!pick)
        {
            bot.pumpFor(Ms(1000));
            continue;
        }
        const auto goal = [&]() { return positionOf(bot, pick); };
        if (!approach(bot, t, self, me, goal, 1.8f, Ms(40000)))
            continue;
        bot.send(vn::AttackReq{pick});
        const game::SkillDef& heavy = *game::findSkill(game::kSkillHeavyBlow);
        auto lastSkill = Clock::now() - Ms(10000);
        const auto fightEnd = Clock::now() + Ms(40000);
        while (Clock::now() < fightEnd && !isDeadOf(bot, pick) && !isDeadOf(bot, me))
        {
            waitPumping(bot, kStep, nullptr);
            bot.keepAlive();
            const auto pos = positionOf(bot, pick);
            if (!pos)
                break;
            if (distance(self.p(), *pos) > 2.4f)
            {
                (void)approach(bot, t, self, me, goal, 1.8f, Ms(10000));
                continue;
            }
            // Ağır Darbe whenever it may be ready (a refused try costs nothing): shorter fights.
            const std::uint32_t mp = bot.view().selfStats.empty() ? heavy.mpCost : bot.view().selfStats.back().mp;
            if (mp >= heavy.mpCost && Clock::now() - lastSkill > Ms(heavy.cooldownMs + 300))
            {
                bot.send(vn::SkillReq{heavy.id, pick});
                lastSkill = Clock::now();
            }
        }
        if (isDeadOf(bot, me))
        {
            abandoned.insert(pick); // an unlucky pull: respawn and try another one
            respawn(bot, self, me);
            continue;
        }
        if (isDeadOf(bot, pick))
        {
            diedAt = positionOf(bot, pick).value_or(self.p());
            return pick;
        }
    }
    return 0;
}

} // namespace

std::string scenarioLoot(const Context& c)
{
    const auto& t = terrainOf(c);
    if (!c.monsters || !c.items)
        fail("gamedata/monsters.toml and gamedata/items.toml are needed");
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    if (c.opts.phase == "check")
    {
        // A later entry (with -Database: a new server process): the same balance and stacks.
        std::ifstream in(c.opts.stateFile);
        std::string name;
        std::uint32_t coins = 0;
        if (!(in >> name >> coins))
            fail("cannot read the character name and balance from --state-file");
        std::map<std::uint16_t, std::uint32_t> expected;
        std::uint32_t tmpl = 0, units = 0;
        while (in >> tmpl >> units)
            expected[static_cast<std::uint16_t>(tmpl)] = units;
        const auto chars = listChars(bot);
        const auto it = std::find_if(chars.begin(), chars.end(), [&](const vn::CharSummary& s) { return s.name == name; });
        if (it == chars.end())
            fail("character '" + name + "' not listed");
        Self self;
        (void)enterWorld(bot, it->charId, t, self);
        if (bot.view().coins.front().coins != coins)
            fail(std::format("entered with {} Akçe (expected {})", bot.view().coins.front().coins, coins));
        game::Inventory inv;
        (void)applyItemEntries(inv, bot.view().inventory->items);
        if (unitTotals(inv) != expected)
            fail("the inventory after re-entering differs from the one after looting");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("'{}' entered with {} Akçe and {} template(s) as stored", name, coins, expected.size());
    }

    const std::uint32_t charId = createCharacter(bot, c.opts.race); // a warrior
    std::string name = "?";
    for (const auto& s : listChars(bot))
        if (s.charId == charId)
            name = s.name;
    Self self;
    const auto enter = enterWorld(bot, charId, t, self);
    const std::uint32_t me = enter.entityId;
    auto& v = bot.view();
    std::uint32_t balance = v.coins.front().coins;
    if (balance != 0)
        fail(std::format("a new character entered with {} Akçe", balance));
    game::Inventory inv;
    (void)applyItemEntries(inv, v.inventory->items);
    if (!bot.pumpUntil([&] { return v.count(vn::kEntityTypeMonster) > 0; }, Ms(3000)))
        fail("no monster spawned around the village");

    int takes = 0, itemEntries = 0;
    std::uint32_t coinsTotal = 0;
    std::set<std::uint32_t> killed;
    const auto healthy = [&] {
        // Another fight only with most of the hp left (no waiting for regeneration in this test).
        const auto& s = v.selfStats;
        return s.empty() || s.back().hp * 4 >= s.back().maxHp * 3;
    };
    for (int round = 0; round < 3 && (takes == 0 || (itemEntries == 0 && healthy())); ++round)
    {
        Point corpse;
        const std::uint32_t victim = killNearestThrall(bot, t, self, me, *c.monsters, killed, corpse);
        if (!victim)
            break;
        killed.insert(victim);
        // §1.3: a thrall always drops coins, so a container appears at the corpse, ours (LootMine).
        std::uint32_t loot = 0;
        if (!bot.pumpUntil(
                [&] {
                    for (const auto& [id, r] : v.entities)
                        if (r.spawn.entityType == vn::kEntityTypeLoot && distance({r.spawn.x, r.spawn.z}, corpse) < 1.5f)
                        {
                            loot = id;
                            return true;
                        }
                    return false;
                },
                Ms(2000)))
            fail("no loot container appeared at the thrall's corpse");
        const vn::EntitySpawn spawn = v.entities.at(loot).spawn;
        if (spawn.state != vn::kEntityStateLootMine || spawn.templateId == 0 || spawn.hp != 0)
            fail(std::format("loot ENTITY_SPAWN state {} template {} hp {}", spawn.state, spawn.templateId, spawn.hp));
        const auto at = [&]() -> std::optional<Point> { return Point{spawn.x, spawn.z}; };
        if (!approach(bot, t, self, me, at, 1.5f, Ms(20000)))
            fail("could not walk to the loot container");

        // Open: LOOT_OPEN_RES(Ok), then LOOT_CONTENTS with coins in the table's range.
        const std::size_t contentsBefore = v.lootContents.size();
        bot.dropPongs();
        bot.send(vn::LootOpenReq{loot});
        const auto open = bot.expect<vn::LootOpenRes>();
        if (open.lootId != loot || open.result != vn::LootResult::Ok)
            fail(std::format("LOOT_OPEN_RES {} for container {}", vn::lootResultName(open.result), loot));
        if (!bot.pumpUntil([&] { return v.lootContents.size() > contentsBefore; }, Ms(2000)))
            fail("no LOOT_CONTENTS after LOOT_OPEN_RES(Ok)");
        const vn::LootContents contents = v.lootContents.back();
        if (contents.lootId != loot || contents.coins < 3 || contents.coins > 9)
            fail(std::format("LOOT_CONTENTS of {} with {} Akçe (a thrall drops 3-9)", contents.lootId, contents.coins));

        // Take everything: ITEM_UPDATE(s) and COINS before LOOT_TAKE_RES(Ok), totals add up.
        const auto before = unitTotals(inv);
        const std::size_t updatesBefore = v.itemUpdates.size();
        const std::size_t coinsBefore = v.coins.size();
        const std::size_t closedBefore = v.lootClosed.size();
        const std::uint32_t seq = bot.nextSeq();
        bot.dropPongs();
        bot.send(vn::LootTakeReq{seq, loot, vn::kLootTakeAll});
        const auto res = bot.expect<vn::LootTakeRes>();
        if (res.seq != seq || res.lootId != loot || res.result != vn::LootResult::Ok)
            fail(std::format("LOOT_TAKE_RES seq {} loot {} {}", res.seq, res.lootId, vn::lootResultName(res.result)));
        for (std::size_t i = updatesBefore; i < v.itemUpdates.size(); ++i)
            if (!applyItemEntries(inv, v.itemUpdates[i].items))
                fail("ITEM_UPDATE with an invalid place");
        if (v.lastItemUpdateIndex > bot.lastIndex() || v.lastCoinsIndex > bot.lastIndex())
            fail("ITEM_UPDATE / COINS arrived after LOOT_TAKE_RES");
        auto expected = before;
        for (const auto& e : contents.entries)
            expected[e.templateId] += e.quantity;
        if (unitTotals(inv) != expected)
            fail("the bag after the take does not hold the container's entries");
        if (v.coins.size() != coinsBefore + 1)
            fail(std::format("{} COINS for one take with coins", v.coins.size() - coinsBefore));
        const vn::Coins& gain = v.coins.back();
        if (gain.gained != contents.coins || gain.coins != balance + contents.coins)
            fail(std::format("COINS {} (+{}) after {} + {}", gain.coins, gain.gained, balance, contents.coins));
        balance = gain.coins;
        // Emptied: LOOT_CLOSED(Gone), then the container despawns.
        if (!bot.pumpUntil([&] { return !v.entities.contains(loot); }, Ms(2000)))
            fail("the emptied container did not despawn");
        if (v.lootClosed.size() != closedBefore + 1 || v.lootClosed.back().lootId != loot ||
            v.lootClosed.back().reason != vn::LootClosedReason::Gone)
            fail("no LOOT_CLOSED(Gone) for the emptied container");
        ++takes;
        itemEntries += static_cast<int>(contents.entries.size());
        coinsTotal += contents.coins;
        // A take from the gone container: NotFound.
        bot.dropPongs();
        bot.send(vn::LootTakeReq{bot.nextSeq(), loot, vn::kLootTakeAll});
        if (bot.expect<vn::LootTakeRes>().result != vn::LootResult::NotFound)
            fail("a take from a despawned container was not NotFound");
    }
    if (takes == 0)
        fail("could not loot a single Kül Çömezi");
    expectNoViewErrors(bot, "loot view");
    if (!c.opts.stateFile.empty())
    {
        std::ofstream out(c.opts.stateFile, std::ios::trunc);
        out << name << "\n" << balance << "\n";
        for (const auto& [tmpl, units] : unitTotals(inv))
            out << tmpl << " " << units << "\n";
        if (!out)
            fail("cannot write --state-file");
    }
    bot.send(vn::Logout{});
    bot.expectClosed(true, bot.timeout());
    return std::format("looted {} Kül Çömezi container(s): {} Akçe, {} item entr(ies); balance {}, bag consistent", takes,
                       coinsTotal, itemEntries, balance);
}

// ---- stats: CHAR_STATS, stat points, allocation, damage follows (docs/PROTOCOL_STATS.md) ----

namespace {

using game::Attributes;

// The stats the server must report for this character now (shared formulas).
game::Stats expectedStats(const Context& c, Bot& bot, std::uint8_t charClass, std::uint16_t level,
                          const Attributes& allocated)
{
    game::Inventory inv;
    if (!bot.view().inventory || !applyItemEntries(inv, bot.view().inventory->items))
        fail("no usable INVENTORY");
    for (const auto& u : bot.view().itemUpdates)
        (void)applyItemEntries(inv, u.items);
    return game::deriveStats(charClass, level, allocated, game::equipmentBonus(*c.items, inv));
}

void expectCharStats(const vn::CharStats& s, const game::Stats& e, std::uint16_t level, std::string_view what)
{
    bool ok = s.level == level && s.unspent == e.unspent && s.maxHp == e.maxHp && s.maxMp == e.maxMp &&
              s.attack == e.attack && s.defense == e.defense && s.critPermille == e.critPermille &&
              s.dodgePermille == e.dodgePermille && s.attackIntervalMs == e.attackIntervalMs;
    for (std::size_t i = 0; i < game::kAttributeCount; ++i)
        ok = ok && s.attributes[i].base == e.base[i] && s.attributes[i].bonus == e.bonus[i];
    if (!ok)
        fail(std::format("{}: CHAR_STATS level {} unspent {} str {} vit {} hp {} mp {} atk {} def {} crit {} dodge {} "
                         "(expected level {} unspent {} str {} vit {} hp {} mp {} atk {} def {} crit {} dodge {})",
                         what, s.level, s.unspent, s.attributes[0].base, s.attributes[2].base, s.maxHp, s.maxMp,
                         s.attack, s.defense, s.critPermille, s.dodgePermille, level, e.unspent, e.base[0], e.base[2],
                         e.maxHp, e.maxMp, e.attack, e.defense, e.critPermille, e.dodgePermille));
}

// The next CHAR_STATS after the first `seen` ones.
vn::CharStats waitCharStats(Bot& bot, std::size_t seen, Ms timeout, std::string_view what)
{
    if (!bot.pumpUntil([&] { return bot.view().charStats.size() > seen || bot.closed(); }, timeout) ||
        bot.view().charStats.size() <= seen)
        fail(std::format("no CHAR_STATS {}", what));
    return bot.view().charStats[seen];
}

vn::StatAllocateRes allocate(Bot& bot, const Attributes& points)
{
    bot.dropPongs();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::StatAllocateReq{seq, points});
    const auto res = bot.expect<vn::StatAllocateRes>();
    if (res.seq != seq)
        fail(std::format("STAT_ALLOCATE_RES seq {} (expected {})", res.seq, seq));
    return res;
}

// After entering: CHAR_STATS right after COINS and before the first ENTITY_SPAWN (§3.2), with the
// values of the shared formulas.
vn::CharStats checkEntryStats(const Context& c, Bot& bot, std::uint8_t charClass, std::uint16_t level,
                              const Attributes& allocated)
{
    auto& v = bot.view();
    const vn::CharStats s = waitCharStats(bot, 0, bot.timeout(), "after ENTER_WORLD_RES");
    if (v.firstCharStatsIndex != v.firstCoinsIndex + 1)
        fail("CHAR_STATS did not come right after COINS");
    if (v.firstSpawnIndex != 0 && v.firstSpawnIndex < v.firstCharStatsIndex)
        fail("ENTITY_SPAWN arrived before CHAR_STATS");
    expectCharStats(s, expectedStats(c, bot, charClass, level, allocated), level, "on entry");
    return s;
}

enum class FightOutcome
{
    Killed,
    Died,
    NoTarget,
};

// Walks to the nearest live level-1 Kül Çömezi (one without neighbours if possible), auto-attacks
// it (Ağır Darbe when ready) until it or the bot dies.
FightOutcome fightThrall(Bot& bot, const assets::TerrainData& t, Self& self, std::uint32_t me,
                         const game::MonsterData& data, std::uint32_t& victim)
{
    const game::MonsterTemplate* thrall = nullptr;
    for (const auto& m : data.templates())
        if (m.model == "ash_thrall" && m.level == 1)
            thrall = &m;
    if (!thrall)
        fail("no level-1 ash_thrall template");
    std::uint32_t pick = 0;
    float best = 1e9f;
    for (const auto& [id, r] : bot.view().entities)
    {
        if (r.spawn.entityType != vn::kEntityTypeMonster || r.spawn.templateId != thrall->id ||
            (r.spawn.state & vn::kEntityStateDead) != 0 || isDeadOf(bot, id))
            continue;
        const Point at = *positionOf(bot, id);
        bool crowded = false;
        for (const auto& [other, o] : bot.view().entities)
            if (other != id && o.spawn.entityType == vn::kEntityTypeMonster && !isDeadOf(bot, other) &&
                distance(at, *positionOf(bot, other)) < 10.0f)
                crowded = true;
        const float d = distance(self.p(), at) + (crowded ? 1000.0f : 0.0f);
        if (d < best)
        {
            best = d;
            pick = id;
        }
    }
    if (!pick)
        return FightOutcome::NoTarget;
    const auto goal = [&]() { return positionOf(bot, pick); };
    if (!approach(bot, t, self, me, goal, 1.8f, Ms(40000)))
        return isDeadOf(bot, me) ? FightOutcome::Died : FightOutcome::NoTarget;
    bot.send(vn::AttackReq{pick});
    const game::SkillDef& heavy = *game::findSkill(game::kSkillHeavyBlow);
    auto lastSkill = Clock::now() - Ms(10000);
    const auto fightEnd = Clock::now() + Ms(40000);
    while (Clock::now() < fightEnd && !isDeadOf(bot, pick) && !isDeadOf(bot, me))
    {
        waitPumping(bot, kStep, nullptr);
        bot.keepAlive();
        const auto pos = positionOf(bot, pick);
        if (!pos)
            break;
        if (distance(self.p(), *pos) > 2.4f)
        {
            (void)approach(bot, t, self, me, goal, 1.8f, Ms(10000));
            continue;
        }
        const std::uint32_t mp = bot.view().selfStats.empty() ? heavy.mpCost : bot.view().selfStats.back().mp;
        if (mp >= heavy.mpCost && Clock::now() - lastSkill > Ms(heavy.cooldownMs + 300))
        {
            bot.send(vn::SkillReq{heavy.id, pick});
            lastSkill = Clock::now();
        }
    }
    if (isDeadOf(bot, me))
        return FightOutcome::Died;
    if (!isDeadOf(bot, pick))
        return FightOutcome::NoTarget;
    victim = pick;
    return FightOutcome::Killed;
}

// Every basic attack (skill 0) of `me` on `target` must lie in the damage range of `attack`
// against the thrall's defense (variance 0.9..1.1, x1.5 when critical). Returns the hits checked.
std::size_t checkHitsFollow(Bot& bot, std::uint32_t me, std::uint32_t target, std::uint16_t attack,
                            std::uint32_t defense, std::uint16_t oldAttack)
{
    const auto bound = [&](std::uint16_t a, float roll, bool crit) {
        return game::computeDamage(a, 1.0f, defense, crit ? 1.0f : 0.0f, roll, crit ? 0.0f : 1.0f).amount;
    };
    std::size_t n = 0;
    for (const auto& e : bot.view().combatEvents)
    {
        if (e.sourceId != me || e.targetId != target || e.skillId != 0 || e.kind != vn::kCombatKindDamage)
            continue;
        const bool crit = (e.flags & vn::kCombatFlagCritical) != 0;
        const bool killing = (e.flags & vn::kCombatFlagKillingBlow) != 0;
        const std::uint32_t lo = bound(attack, 0.0f, crit), hi = bound(attack, 1.0f, crit);
        // The killing blow is cut to the hp that was left.
        if (e.amount > hi || (!killing && e.amount < lo))
            fail(std::format("a {}hit of {} does not follow attack {} (range {}..{}; before the allocation {}..{})",
                             crit ? "critical " : "", e.amount, attack, lo, hi, bound(oldAttack, 0.0f, crit),
                             bound(oldAttack, 1.0f, crit)));
        ++n;
    }
    return n;
}

const game::MonsterTemplate& levelOneThrall(const game::MonsterData& data)
{
    for (const auto& m : data.templates())
        if (m.model == "ash_thrall" && m.level == 1)
            return m;
    fail("no level-1 ash_thrall template");
}

} // namespace

std::string scenarioStats(const Context& c)
{
    namespace vg = vo::game;
    const auto& t = terrainOf(c);
    if (!c.monsters || !c.items)
        fail("gamedata/monsters.toml and gamedata/items.toml are needed");
    const vg::MonsterTemplate& thrall = levelOneThrall(*c.monsters);
    constexpr std::uint8_t kWarrior = 1;
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);

    if (c.opts.phase == "check" || c.opts.phase == "reset")
    {
        // check: after `vo_admin set-level <name> 10` (the 3 points spent at level 2 still fit):
        // 24 unspent; spend them all on Güç, the damage follows. reset: after `vo_admin
        // reset-stats <name>`: every point unspent again, the starting spread.
        std::ifstream in(c.opts.stateFile);
        std::string name;
        if (!(in >> name))
            fail("cannot read the character name from --state-file");
        const auto chars = listChars(bot);
        const auto it = std::find_if(chars.begin(), chars.end(), [&](const vn::CharSummary& s) { return s.name == name; });
        if (it == chars.end())
            fail("character '" + name + "' not listed");
        Self self;
        const auto e = enterWorld(bot, it->charId, t, self);
        const bool reset = c.opts.phase == "reset";
        const Attributes before = reset ? Attributes{} : Attributes{2, 0, 1, 0, 0};
        const vn::CharStats s = checkEntryStats(c, bot, kWarrior, e.level, before);
        if (reset)
        {
            if (s.unspent != vg::pointsForLevel(e.level) || s.attributes[0].base != vg::classStartAttributes(kWarrior)[0])
                fail(std::format("after reset-stats: unspent {}, Güç {}", s.unspent, s.attributes[0].base));
            bot.send(vn::Logout{});
            bot.expectClosed(true, bot.timeout());
            return std::format("'{}' (level {}) has all {} points unspent after reset-stats", name, e.level, s.unspent);
        }
        if (e.level != vg::kLevelCap || s.unspent != 24)
            fail(std::format("entered at level {} with {} unspent point(s) (expected level 10, 24)", e.level, s.unspent));
        const std::uint16_t oldAttack = s.attack;
        const Attributes all{24, 0, 0, 0, 0};
        const auto res = allocate(bot, all);
        if (res.result != vn::StatAllocateResult::Ok || res.unspent != 0)
            fail(std::format("allocating 24 Güç: {} (unspent {})", vn::statAllocateResultName(res.result), res.unspent));
        const Attributes after{26, 0, 1, 0, 0};
        const vn::CharStats s2 = waitCharStats(bot, 1, Ms(2000), "after the allocation");
        expectCharStats(s2, expectedStats(c, bot, kWarrior, e.level, after), e.level, "after 24 Güç");
        if (s2.attack <= oldAttack)
            fail(std::format("attack {} after 24 Güç (before {})", s2.attack, oldAttack));
        if (!bot.pumpUntil([&] { return bot.view().count(vn::kEntityTypeMonster) > 0; }, Ms(3000)))
            fail("no monster spawned around the village");
        std::size_t hits = 0;
        for (int round = 0; round < 4 && hits == 0; ++round)
        {
            std::uint32_t victim = 0;
            const FightOutcome o = fightThrall(bot, t, self, e.entityId, *c.monsters, victim);
            if (o == FightOutcome::Died)
                respawn(bot, self, e.entityId);
            else if (o == FightOutcome::NoTarget)
                bot.pumpFor(Ms(1000));
            else
                hits = checkHitsFollow(bot, e.entityId, victim, s2.attack, thrall.defense, oldAttack);
        }
        if (hits == 0)
            fail("no basic attack on a Kül Çömezi to check");
        expectNoViewErrors(bot, "stats view");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("'{}' spent 24 points on Güç at level 10: attack {} -> {}, {} hit(s) in range", name, oldAttack,
                           s2.attack, hits);
    }

    const std::uint32_t charId = createCharacter(bot, c.opts.race); // a warrior
    std::string name = "?";
    for (const auto& s : listChars(bot))
        if (s.charId == charId)
            name = s.name;
    Self self;
    auto enter = enterWorld(bot, charId, t, self);
    std::uint32_t me = enter.entityId;
    auto& v = bot.view();

    // 1. Entry: CHAR_STATS with the starting spread, no points at level 1.
    const vn::CharStats first = checkEntryStats(c, bot, kWarrior, 1, {});
    if (first.unspent != 0 || first.attributes[0].base != 12 || first.attack != enter.attack ||
        first.maxHp != enter.maxHp)
        fail("CHAR_STATS on entry does not match ENTER_WORLD_RES / the warrior's start");
    if (allocate(bot, {}).result != vn::StatAllocateResult::InvalidAmount)
        fail("an allocation of nothing was not InvalidAmount");
    const auto none = allocate(bot, {1, 0, 0, 0, 0});
    if (none.result != vn::StatAllocateResult::NotEnoughPoints || none.unspent != 0)
        fail("a point at level 1 was not NotEnoughPoints");

    // 2. Hunt level-1 Kül Çömezis until level 2 (respawning after a death: a level-1 warrior can
    // lose to an unlucky pull, the check is the level-up, not the fight).
    if (!bot.pumpUntil([&] { return v.count(vn::kEntityTypeMonster) > 0; }, Ms(3000)))
        fail("no monster spawned around the village");
    int kills = 0, deaths = 0;
    const auto huntEnd = Clock::now() + Ms(170000);
    while (v.levelUps.empty() && Clock::now() < huntEnd)
    {
        std::uint32_t victim = 0;
        switch (fightThrall(bot, t, self, me, *c.monsters, victim))
        {
        case FightOutcome::Killed: ++kills; break;
        case FightOutcome::Died:
            ++deaths;
            respawn(bot, self, me);
            break;
        case FightOutcome::NoTarget: bot.pumpFor(Ms(1000)); break;
        }
        // Rest a little between fights (out-of-combat regeneration) when hurt.
        const auto& ss = v.selfStats;
        if (v.levelUps.empty() && !ss.empty() && ss.back().hp * 2 < ss.back().maxHp)
            bot.pumpFor(Ms(8000));
    }
    if (v.levelUps.empty())
        fail(std::format("no level-up after {} kill(s) and {} death(s)", kills, deaths));
    if (v.levelUps.front().entityId != me || v.levelUps.front().level != 2)
        fail("LEVEL_UP is not this character's level 2");
    // §3.2: CHAR_STATS after the level-up with 3 unspent points.
    if (!bot.pumpUntil([&] { return v.charStats.back().level == 2; }, Ms(2000)))
        fail("no CHAR_STATS with level 2 after LEVEL_UP");
    const vn::CharStats lvl2 = v.charStats.back();
    expectCharStats(lvl2, expectedStats(c, bot, kWarrior, 2, {}), 2, "after the level-up");
    if (lvl2.unspent != 3)
        fail(std::format("{} unspent point(s) at level 2 (expected 3)", lvl2.unspent));

    // 3. Spend them: 2 Güç, 1 Dayanıklılık. Too many / nothing first.
    if (allocate(bot, {4, 0, 0, 0, 0}).result != vn::StatAllocateResult::NotEnoughPoints)
        fail("4 points at level 2 were not NotEnoughPoints");
    const std::size_t statsBefore = v.charStats.size();
    const std::size_t selfBefore = v.selfStats.size();
    const Attributes spent{2, 0, 1, 0, 0};
    const auto ok = allocate(bot, spent);
    if (ok.result != vn::StatAllocateResult::Ok || ok.unspent != 0)
        fail(std::format("allocating 2 Güç + 1 Dayanıklılık: {} (unspent {})", vn::statAllocateResultName(ok.result),
                         ok.unspent));
    const vn::CharStats after = waitCharStats(bot, statsBefore, Ms(2000), "after the allocation");
    const vg::Stats expected = expectedStats(c, bot, kWarrior, 2, spent);
    expectCharStats(after, expected, 2, "after 2 Güç + 1 Dayanıklılık");
    if (after.attributes[0].base != 14 || after.attributes[2].base != 12 || after.attack <= lvl2.attack ||
        after.maxHp != lvl2.maxHp + 5)
        fail("the allocation did not raise Güç / Dayanıklılık, attack and max hp");
    // SELF_STATS follows with the new maximum (hp itself is not filled).
    if (!bot.pumpUntil(
            [&] { return v.selfStats.size() > selfBefore && v.selfStats.back().maxHp == expected.maxHp; }, Ms(2000)))
        fail("no SELF_STATS with the new max hp");
    if (allocate(bot, {1, 0, 0, 0, 0}).result != vn::StatAllocateResult::NotEnoughPoints)
        fail("a fourth point at level 2 was not NotEnoughPoints");

    // 4. The damage dealt follows the new attack.
    std::size_t hits = 0;
    for (int round = 0; round < 4 && hits == 0; ++round)
    {
        std::uint32_t victim = 0;
        const FightOutcome o = fightThrall(bot, t, self, me, *c.monsters, victim);
        if (o == FightOutcome::Died)
            respawn(bot, self, me);
        else if (o == FightOutcome::NoTarget)
            bot.pumpFor(Ms(1000));
        else
            hits = checkHitsFollow(bot, me, victim, after.attack, thrall.defense, lvl2.attack);
    }
    if (hits == 0)
        fail("no basic attack on a Kül Çömezi after the allocation");

    // 5. Leave and enter again: the allocation is stored.
    bot.dropPongs();
    bot.send(vn::LeaveWorldReq{});
    (void)bot.expect<vn::LeaveWorldRes>();
    enter = enterWorld(bot, charId, t, self);
    me = enter.entityId;
    const vn::CharStats again = checkEntryStats(c, bot, kWarrior, 2, spent);
    if (again.attack != after.attack || again.unspent != 0)
        fail("the allocation was not kept after re-entering");

    // 6. A STAT_ALLOCATE_REQ flood: 5 per second are answered, the 6th closes the connection.
    for (int i = 0; i < 5; ++i)
        if (allocate(bot, {}).result != vn::StatAllocateResult::InvalidAmount)
            fail("a flood request was not InvalidAmount");
    bot.send(vn::StatAllocateReq{bot.nextSeq(), {}});
    for (;;)
    {
        const Received m = bot.next("waiting for ERROR(Flood)");
        if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::Pong))
            continue;
        vn::Error err;
        if (m.opcode != static_cast<std::uint16_t>(vn::Opcode::Error) || !vn::decodeMessage(m.payload, err) ||
            err.errorCode != vn::ErrorCode::Flood)
            fail(std::format("the 6th STAT_ALLOCATE_REQ within a second got {} (expected ERROR(Flood))",
                             vn::opcodeName(static_cast<vn::Opcode>(m.opcode))));
        break;
    }
    bot.expectClosed(true, bot.timeout());
    expectNoViewErrors(bot, "stats view");
    if (!c.opts.stateFile.empty())
    {
        std::ofstream out(c.opts.stateFile, std::ios::trunc);
        out << name << "\n";
        if (!out)
            fail("cannot write --state-file");
    }
    return std::format("level 2 after {} kill(s), {} death(s); spent 2 Güç + 1 Dayanıklılık: attack {} -> {}, max hp {} "
                       "-> {}, {} hit(s) in range; kept after re-entering; flood closed",
                       kills, deaths, lvl2.attack, after.attack, lvl2.maxHp, after.maxHp, hits);
}

} // namespace vo::tools::botclient
