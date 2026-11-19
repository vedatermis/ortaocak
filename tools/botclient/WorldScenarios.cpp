// World scenarios of the bot client (docs/SERVER_WORLD.md §9): legal walking, speed hack, wall hack,
// MOVE_REQ flood, area of interest between two bots, position persistence. All moves are planned on
// the same zone terrain the server uses (--terrain, default assets/terrain/zone1/zone1.vterrain).

#include "shared/game/Combat.h"
#include "shared/game/Movement.h"
#include "shared/game/Npcs.h"
#include "shared/game/PathFind.h"
#include "shared/game/Shop.h"
#include "shared/game/Upgrade.h"
#include "shared/game/Skills.h"
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
vn::EnterWorldRes enterWorld(Bot& bot, std::uint32_t charId, const assets::TerrainData& t, Self& self,
                            std::uint16_t expectZone = 1)
{
    bot.view() = {}; // a new world session starts with no entities
    bot.trackWorld(true);
    bot.send(vn::EnterWorldReq{charId});
    const auto enter = bot.expect<vn::EnterWorldRes>();
    if (enter.result != vn::EnterWorldResult::Ok || enter.entityId == 0)
        fail(std::format("ENTER_WORLD_RES result {}", static_cast<int>(enter.result)));
    if (enter.zoneId != expectZone || enter.walkSpeed != game::kWalkSpeed || enter.runSpeed != game::kRunSpeed)
        fail(std::format("ENTER_WORLD_RES zone {} speeds {}/{} (expected zone {}, {}/{})", enter.zoneId, enter.walkSpeed,
                         enter.runSpeed, expectZone, game::kWalkSpeed, game::kRunSpeed));
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
    // PROTOCOL_SKILLS.md §4: then SKILL_STATE and SKILL_BAR, still before the first ENTITY_SPAWN.
    if (!bot.pumpUntil([&] { return !v.skillBars.empty() || bot.closed(); }, bot.timeout()) || v.skillStates.empty() ||
        v.skillBars.empty())
        fail("no SKILL_STATE / SKILL_BAR after COINS");
    if (v.firstSkillStateIndex != v.firstCoinsIndex + 1)
        fail("SKILL_STATE did not come right after COINS");
    if (v.firstSpawnIndex != 0 && v.firstSpawnIndex < v.firstSkillStateIndex)
        fail("ENTITY_SPAWN arrived before SKILL_STATE");
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

// ---- Gates while hunting (docs/PROTOCOL_ZONES.md §2) ----
//
// Zone 1 has gates; a hunting bot that walks into one changes zone and its scenario falls apart far
// from the cause. approach() therefore keeps out of every gate trigger + kGateMargin unless the
// caller asks for a gate, and the hunting scenarios fail at once, with a clear message, on any
// ZONE_CHANGE_BEGIN.

constexpr float kGateMargin = 3.0f; // keep-out beyond a gate's trigger radius

struct GateKeepOut
{
    std::string marker;
    Point at;
    float radius = 0.0f; // trigger radius + kGateMargin
};

// The gate markers (gate_*) of the terrain with their trigger radius from gamedata/zones.toml (2.5 m
// if the marker is not listed there).
std::vector<GateKeepOut> gateKeepOuts(const assets::TerrainData& t)
{
    std::vector<GateKeepOut> out;
    for (const auto& s : t.spawns)
    {
        if (!s.name.starts_with("gate_"))
            continue;
        float radius = 2.5f;
        for (const auto& z : game::zones().zones())
            for (const auto& g : z.gates)
                if (g.marker == s.name)
                    radius = g.radius;
        out.push_back({s.name, {s.position.x, s.position.z}, radius + kGateMargin});
    }
    return out;
}

// The keep-out `p` lies in (nullptr if none).
const GateKeepOut* keepOutAt(const std::vector<GateKeepOut>& gates, Point p)
{
    for (const auto& g : gates)
        if (distance(p, g.at) <= g.radius)
            return &g;
    return nullptr;
}

// Whether the straight walk a -> b passes through a keep-out.
bool segmentNearGate(const std::vector<GateKeepOut>& gates, Point a, Point b)
{
    for (const auto& g : gates)
    {
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float len2 = dx * dx + dz * dz;
        const float u = len2 > 1e-6f ? std::clamp(((g.at.x - a.x) * dx + (g.at.z - a.z) * dz) / len2, 0.0f, 1.0f) : 0.0f;
        if (distance(g.at, {a.x + dx * u, a.z + dz * u}) <= g.radius)
            return true;
    }
    return false;
}

// Hunting scenarios never change zone: fails with where it happened if a ZONE_CHANGE_BEGIN came.
void expectNoZoneChange(Bot& bot, const Self& self, std::string_view what)
{
    const auto& b = bot.view().zoneBegins;
    if (!b.empty())
        fail(std::format("{}: the bot changed zone unexpectedly (ZONE_CHANGE_BEGIN {} -> {}) near ({:.1f}, {:.1f}); "
                         "a hunting walk must keep out of the gates",
                         what, b.front().fromZone, b.front().toZone, self.x, self.z));
}

// Walks (runs) towards a moving goal until within `stopAt` metres, legal moves only (straight
// segments checked against the walk mask, grid paths when blocked), at 90 % of the run speed. False
// if the bot died, `goal` became unknown or `timeout` passed. `onStep` runs every step. Unless
// `allowGate`, it never steps into a gate trigger + kGateMargin (paths go around; a goal inside one
// is given up: false) and fails the scenario on a zone change.
bool approach(Bot& bot, const assets::TerrainData& t, Self& self, std::uint32_t me,
              const std::function<std::optional<Point>()>& goal, float stopAt, Ms timeout,
              const std::function<void()>& onStep = nullptr, bool allowGate = false)
{
    const auto deadline = Clock::now() + timeout;
    const std::vector<GateKeepOut> gates = allowGate ? std::vector<GateKeepOut>{} : gateKeepOuts(t);
    const Nav nav(t);
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
        if (!allowGate)
            expectNoZoneChange(bot, self, "approach");
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
        if (keepOutAt(gates, *g))
        {
            stop(); // the goal stands at a gate: not worth a zone change
            return false;
        }
        const auto now = Clock::now();
        if (next >= path.size() || distance(planned, *g) > 2.0f || now - lastPlan > Ms(1500))
        {
            lastPlan = now;
            planned = *g;
            path.clear();
            next = 0;
            if (game::isSegmentWalkable(t, self.x, self.z, g->x, g->z) && !segmentNearGate(gates, self.p(), *g))
                path.push_back(*g);
            else if (gates.empty())
            {
                if (auto found = game::findWalkPathNear(t, {self.x, self.z}, {g->x, g->z}, 3.0f, 0.5f, 0.3f))
                    for (const auto& p : found->points)
                        path.push_back({p.x, p.z});
            }
            else
            {
                // Grid path around the keep-outs to a cell within stopAt of the goal.
                const float reach = std::max(stopAt - 0.3f, 0.4f);
                if (auto found = nav.bfs(
                        self.p(), [&](int i, int j) { return distance(nav.centre(i, j), *g) <= reach; }, 120000,
                        [&](int i, int j) { return keepOutAt(gates, nav.centre(i, j)) != nullptr; }))
                    path.assign(found->begin() + 1, found->end());
            }
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
        // Last line of defence: never a step deeper into a keep-out (out of one is fine: a bot that
        // stands in one, e.g. after a respawn, walks away).
        if (const GateKeepOut* k = keepOutAt(gates, pos); k && distance(pos, k->at) < distance(self.p(), k->at))
        {
            stop();
            return false;
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
        // After `vo_admin set-level <name> 99` (clamped to the cap): entered at level 20 (M15), xp 0, full hp/mp.
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
            expectNoZoneChange(bot, self, "fighting a Kül Çömezi");
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
        if (const auto* m = data.find(s.monster); m && m->elite && s.zone == 1 && m->name == "Kor Bekçisi") // M15: 3 bosses here
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
    expectNoZoneChange(bot, self, "combat");
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
            expectNoZoneChange(bot, self, "fighting a Kül Çömezi");
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
    expectNoZoneChange(bot, self, "loot");
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
    if (v.firstCharStatsIndex != v.firstSkillStateIndex + 2) // v6: SKILL_STATE, SKILL_BAR in between
        fail("CHAR_STATS did not come right after COINS, SKILL_STATE and SKILL_BAR");
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
                         const game::MonsterData& data, std::uint32_t& victim, bool onlyAlone = false,
                         const std::set<std::uint32_t>* avoid = nullptr)
{
    const game::MonsterTemplate* thrall = nullptr;
    for (const auto& m : data.templates())
        if (m.model == "ash_thrall" && m.level == 1)
            thrall = &m;
    if (!thrall)
        fail("no level-1 ash_thrall template");
    // M15: the Kül Çömezi stand in slots of six that respawn in 12 s - a level-1 warrior that goes from one
    // fight straight into the next dies. Below 70 % hp it sits until 90 % (at most 45 s) first.
    if (const auto& st = bot.view().selfStats; !st.empty() && st.back().hp * 10 < st.back().maxHp * 7 && !isDeadOf(bot, me))
    {
        bot.send(vn::StanceReq{vn::kStanceSit});
        (void)bot.pumpUntil(
            [&] {
                const auto& s = bot.view().selfStats;
                return isDeadOf(bot, me) || (!s.empty() && s.back().hp * 10 >= s.back().maxHp * 9);
            },
            Ms(45000));
        bot.send(vn::StanceReq{vn::kStanceStand});
    }
    expectNoZoneChange(bot, self, "picking a Kül Çömezi");
    const std::vector<GateKeepOut> gates = gateKeepOuts(t);
    std::uint32_t pick = 0;
    float best = 1e9f;
    for (const auto& [id, r] : bot.view().entities)
    {
        if (r.spawn.entityType != vn::kEntityTypeMonster || r.spawn.templateId != thrall->id ||
            (r.spawn.state & vn::kEntityStateDead) != 0 || isDeadOf(bot, id) || (avoid && avoid->count(id)))
            continue;
        const Point at = *positionOf(bot, id);
        // Never hunt across a zone gate: the straight walk would carry the bot into another zone
        // (docs/PROTOCOL_ZONES.md), where there is no Kül Çömezi to find.
        bool throughGate = false;
        for (const auto& s : t.spawns)
        {
            if (!s.name.starts_with("gate_"))
                continue;
            const Point g{s.position.x, s.position.z};
            const Point a = self.p();
            const float dx = at.x - a.x, dz = at.z - a.z;
            const float len2 = dx * dx + dz * dz;
            const float u = len2 > 1e-6f ? std::clamp(((g.x - a.x) * dx + (g.z - a.z) * dz) / len2, 0.0f, 1.0f) : 0.0f;
            if (distance(g, Point{a.x + dx * u, a.z + dz * u}) < 8.0f)
                throughGate = true;
        }
        if (throughGate || keepOutAt(gates, at))
            continue;
        bool crowded = false;
        for (const auto& [other, o] : bot.view().entities)
            if (other != id && o.spawn.entityType == vn::kEntityTypeMonster && !isDeadOf(bot, other) &&
                distance(at, *positionOf(bot, other)) < 10.0f)
                crowded = true;
        if (crowded && onlyAlone)
            continue; // the caller waits until one wanders away from its neighbours
        const float d = distance(self.p(), at) + (crowded ? 1000.0f : 0.0f);
        if (d < best)
        {
            best = d;
            pick = id;
        }
    }
    if (!pick)
        return FightOutcome::NoTarget;
    victim = pick; // also on a death: the caller may want to avoid this one
    const auto goal = [&]() { return positionOf(bot, pick); };
    if (!approach(bot, t, self, me, goal, 1.8f, Ms(40000)))
        return isDeadOf(bot, me) ? FightOutcome::Died : FightOutcome::NoTarget;
    bot.send(vn::AttackReq{pick});
    const game::SkillDef& heavy = *game::findSkill(game::kSkillHeavyBlow);
    auto lastSkill = Clock::now() - Ms(10000);
    std::size_t failsSeen = bot.view().actionFails.size();
    const auto fightEnd = Clock::now() + Ms(40000);
    while (Clock::now() < fightEnd && !isDeadOf(bot, pick) && !isDeadOf(bot, me))
    {
        waitPumping(bot, kStep, nullptr);
        bot.keepAlive();
        expectNoZoneChange(bot, self, "fighting a Kül Çömezi");
        // A request that met the global cooldown of a swing (ACTION_FAIL OnCooldown) is retried in
        // ~300 ms instead of after the skill's whole cooldown (the swing interval would otherwise
        // keep catching it at the same phase).
        const auto& fails = bot.view().actionFails;
        for (; failsSeen < fails.size(); ++failsSeen)
            if (fails[failsSeen].skillId == heavy.id && fails[failsSeen].reason == vn::ActionFailReason::OnCooldown)
                lastSkill = Clock::now() - Ms(heavy.cooldownMs);
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
        if (e.level != 10 || s.unspent != 24)

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
        for (int round = 0; round < 12 && hits == 0; ++round)
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
        expectNoZoneChange(bot, self, "stats");
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
    std::set<std::uint32_t> avoid;
    auto aloneUntil = Clock::now() + Ms(20000);
    const auto huntEnd = Clock::now() + Ms(250000);
    while (v.levelUps.empty() && Clock::now() < huntEnd)
    {
        // Only a thrall with no other monster within 10 m; after 20 s without one, any. One that
        // killed the bot is left alone afterwards (with a fixed world seed the same pull would
        // fail the same way again and again).
        std::uint32_t victim = 0;
        switch (fightThrall(bot, t, self, me, *c.monsters, victim, Clock::now() < aloneUntil, &avoid))
        {
        case FightOutcome::Killed:
            ++kills;
            aloneUntil = Clock::now() + Ms(20000);
            break;
        case FightOutcome::Died:
            ++deaths;
            if (victim)
                avoid.insert(victim);
            respawn(bot, self, me);
            aloneUntil = Clock::now() + Ms(20000);
            break;
        case FightOutcome::NoTarget:
            if (Clock::now() >= aloneUntil)
                avoid.clear();
            bot.pumpFor(Ms(1000));
            bot.keepAlive();
            break;
        }
        // Rest a little between fights (out-of-combat regeneration) when hurt.
        const auto& ss = v.selfStats;
        // Until 85 % hp (at most 45 s): entering the next fight half dead is how a level-1 warrior
        // dies over and over and never reaches level 2 in time.
        const auto restEnd = Clock::now() + Ms(45000);
        while (v.levelUps.empty() && !ss.empty() && ss.back().hp > 0 && ss.back().hp * 100 < ss.back().maxHp * 85 &&
               Clock::now() < restEnd)
        {
            bot.pumpFor(Ms(1000));
            bot.keepAlive();
        }
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
    for (int round = 0; round < 12 && hits == 0; ++round)
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
    expectNoZoneChange(bot, self, "stats");
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

// ---- Skills (docs/PROTOCOL_SKILLS.md) ----

namespace {

vn::SkillLearnRes learnSkill(Bot& bot, std::uint16_t skill)
{
    bot.dropPongs();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::SkillLearnReq{seq, skill});
    const auto res = bot.expect<vn::SkillLearnRes>();
    if (res.seq != seq || res.skillId != skill)
        fail(std::format("SKILL_LEARN_RES seq {} skill {} (expected {} / {})", res.seq, res.skillId, seq, skill));
    return res;
}

vn::SkillBarSetRes setSkillBar(Bot& bot, const vn::SkillBarSlots& slots)
{
    bot.dropPongs();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::SkillBarSetReq{seq, slots});
    const auto res = bot.expect<vn::SkillBarSetRes>();
    if (res.seq != seq)
        fail(std::format("SKILL_BAR_SET_RES seq {} (expected {})", res.seq, seq));
    return res;
}

std::string describeSkillState(const vn::SkillState& s)
{
    std::string out = std::format("points {} unspent {} [", s.points, s.unspent);
    for (const auto& e : s.skills)
        out += std::format(" {}:{}", e.skillId, e.rank);
    return out + " ]";
}

void expectSkillState(const vn::SkillState& s, std::uint16_t points, std::uint16_t unspent,
                      std::vector<vn::SkillRankEntry> skills, std::string_view what)
{
    if (s.points != points || s.unspent != unspent || s.skills != skills)
        fail(std::format("{}: SKILL_STATE {} (expected points {} unspent {})", what, describeSkillState(s), points, unspent));
}

// Uses a self skill and waits for its SKILL_USED and EFFECT_APPLY on `me` (retried while the global
// cooldown of an attack swing refuses it).
vn::EffectApply useSelfBuff(Bot& bot, std::uint32_t me, std::uint16_t skill, std::uint8_t kind)
{
    auto& v = bot.view();
    const std::size_t before = v.effectApplies.size();
    const auto end = Clock::now() + Ms(5000);
    while (Clock::now() < end)
    {
        const std::size_t fails = v.actionFails.size();
        bot.send(vn::SkillReq{skill, 0});
        if (bot.pumpUntil(
                [&] {
                    for (std::size_t i = before; i < v.effectApplies.size(); ++i)
                        if (v.effectApplies[i].entityId == me && v.effectApplies[i].effect.skillId == skill &&
                            v.effectApplies[i].effect.kind == kind)
                            return true;
                    return v.actionFails.size() > fails;
                },
                Ms(2000)))
        {
            for (std::size_t i = before; i < v.effectApplies.size(); ++i)
                if (v.effectApplies[i].entityId == me && v.effectApplies[i].effect.skillId == skill)
                    return v.effectApplies[i];
            const auto& f = v.actionFails.back();
            if (f.reason != vn::ActionFailReason::OnCooldown)
                fail(std::format("skill {} refused: {}", skill, vn::actionFailReasonName(f.reason)));
        }
        bot.pumpFor(Ms(300));
    }
    fail(std::format("no EFFECT_APPLY of skill {} on the bot", skill));
}

} // namespace

std::string scenarioSkills(const Context& c)
{
    namespace vg = vo::game;
    const auto& t = terrainOf(c);
    if (!c.monsters || !c.items)
        fail("gamedata/monsters.toml and gamedata/items.toml are needed");
    const vg::SkillCatalog& catalog = vg::skills();
    if (catalog.size() < 28)
        fail("gamedata/skills.toml is needed");
    constexpr std::uint16_t kHeavy = 1, kWall = 8, kTaunt = 9, kBreath = 10, kCleave = 5;
    const auto defenseUp = static_cast<std::uint8_t>(vg::EffectKind::DefenseUp);
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);

    if (c.opts.phase == "check" || c.opts.phase == "reset")
    {
        // check: after `vo_admin set-level <name> 10`: 9 points, 1 spent (Kalkan Duvarı); spend the
        // rest through both branches (tier gates), use Dirençli Soluk (heal over time), stored
        // after re-entering. reset: after `vo_admin reset-skills <name>`: the free skill only.
        std::ifstream in(c.opts.stateFile);
        std::string name;
        if (!(in >> name))
            fail("cannot read the character name from --state-file");
        const auto chars = listChars(bot);
        const auto it = std::find_if(chars.begin(), chars.end(), [&](const vn::CharSummary& s) { return s.name == name; });
        if (it == chars.end())
            fail("character '" + name + "' not listed");
        Self self;
        auto e = enterWorld(bot, it->charId, t, self);
        auto& v = bot.view();
        if (c.opts.phase == "reset")
        {
            expectSkillState(v.skillStates.front(), 9, 9, {{kHeavy, 1}}, "after reset-skills");
            if (v.skillBars.front().slots != vn::SkillBarSlots{kHeavy, 0, 0, 0, 0, 0, 0, 0})
                fail("the bar is not the default after reset-skills");
            bot.send(vn::Logout{});
            bot.expectClosed(true, bot.timeout());
            return std::format("'{}' has all 9 skill points unspent and the default bar after reset-skills", name);
        }
        expectSkillState(v.skillStates.front(), 9, 8, {{kHeavy, 1}, {kWall, 1}}, "after set-level 10");
        if (v.skillBars.front().slots != vn::SkillBarSlots{kHeavy, kWall, 0, 0, 0, 0, 0, 0})
            fail("the bar was not kept");
        if (learnSkill(bot, kCleave).result != vn::SkillLearnResult::TierLocked)
            fail("Yarma with no point in Hücum was not TierLocked");
        if (learnSkill(bot, kBreath).result != vn::SkillLearnResult::TierLocked)
            fail("Dirençli Soluk with 1 point in Siper was not TierLocked");
        for (const std::uint16_t s : {kWall, kWall, kTaunt, kTaunt, kBreath, kHeavy, kHeavy, kCleave})
        {
            const auto r = learnSkill(bot, s);
            if (r.result != vn::SkillLearnResult::Ok)
                fail(std::format("learning skill {}: {}", s, vn::skillLearnResultName(r.result)));
            bot.pumpFor(Ms(200)); // stays below the 10/s cap
        }
        if (learnSkill(bot, kCleave).result != vn::SkillLearnResult::NotEnoughPoints)
            fail("a tenth point was not NotEnoughPoints");
        if (!bot.pumpUntil([&] { return v.skillStates.back().unspent == 0; }, Ms(2000)))
            fail("no SKILL_STATE with 0 unspent");
        expectSkillState(v.skillStates.back(), 9, 0, {{kHeavy, 3}, {kCleave, 1}, {kWall, 3}, {kTaunt, 2}, {kBreath, 1}},
                         "after spending 9");
        // Dirençli Soluk: heal-over-time ticks (periodic flag) on the bot.
        const auto hot = useSelfBuff(bot, e.entityId, kBreath, static_cast<std::uint8_t>(vg::EffectKind::Hot));
        if (hot.effect.durationMs != 10000 || hot.effect.amount == 0)
            fail("Dirençli Soluk's EFFECT_APPLY has the wrong duration / amount");
        const std::size_t heals = v.combatEvents.size();
        bot.pumpFor(Ms(3500));
        const auto ticks = std::count_if(v.combatEvents.begin() + static_cast<std::ptrdiff_t>(heals), v.combatEvents.end(),
                                         [&](const vn::CombatEvent& ev) {
                                             return ev.skillId == kBreath && ev.kind == vn::kCombatKindHeal &&
                                                    (ev.flags & vn::kCombatFlagPeriodic) != 0;
                                         });
        if (ticks < 2)
            fail(std::format("{} Dirençli Soluk tick(s) in 3.5 s", ticks));
        if (setSkillBar(bot, {kBreath, kTaunt, kWall, kCleave, kHeavy, 0, 0, 0}).result != vn::SkillBarResult::Ok)
            fail("setting a five-skill bar failed");
        bot.dropPongs();
        bot.send(vn::LeaveWorldReq{});
        (void)bot.expect<vn::LeaveWorldRes>();
        e = enterWorld(bot, it->charId, t, self);
        expectSkillState(v.skillStates.front(), 9, 0, {{kHeavy, 3}, {kCleave, 1}, {kWall, 3}, {kTaunt, 2}, {kBreath, 1}},
                         "after re-entering");
        if (v.skillBars.front().slots != vn::SkillBarSlots{kBreath, kTaunt, kWall, kCleave, kHeavy, 0, 0, 0})
            fail("the five-skill bar was not kept");
        expectNoViewErrors(bot, "skills view");
        expectNoZoneChange(bot, self, "skills");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("'{}' spent 9 skill points at level 10 (Hücum 3, Siper 6), heal over time ticked {}x, "
                           "ranks and bar kept",
                           name, ticks);
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

    // 1. Entry: the free skill at rank 1, no points, the default bar.
    expectSkillState(v.skillStates.front(), 0, 0, {{kHeavy, 1}}, "on entry");
    if (v.skillBars.front().slots != vn::SkillBarSlots{kHeavy, 0, 0, 0, 0, 0, 0, 0})
        fail("the default bar is not Ağır Darbe on key 1");
    if (learnSkill(bot, kWall).result != vn::SkillLearnResult::NotEnoughPoints)
        fail("learning at level 1 was not NotEnoughPoints");
    if (learnSkill(bot, 2).result != vn::SkillLearnResult::UnknownSkill)
        fail("a rogue skill was not UnknownSkill");
    if (setSkillBar(bot, {kWall, 0, 0, 0, 0, 0, 0, 0}).result != vn::SkillBarResult::NotLearned)
        fail("an unlearned skill on the bar was not NotLearned");
    bot.send(vn::SkillReq{kWall, 0});
    if (!bot.pumpUntil([&] {
            return std::any_of(v.actionFails.begin(), v.actionFails.end(), [&](const vn::ActionFail& f) {
                return f.skillId == kWall && f.reason == vn::ActionFailReason::NotLearned;
            });
        }, Ms(2000)))
        fail("SKILL_REQ of an unlearned skill was not ACTION_FAIL(NotLearned)");

    // 2. Hunt lone level-1 Kül Çömezis until level 2 (Ağır Darbe whenever ready: its SKILL_USED and
    // cooldown report are checked below), resting to 85 % hp, never the one that killed it.
    if (!bot.pumpUntil([&] { return v.count(vn::kEntityTypeMonster) > 0; }, Ms(3000)))
        fail("no monster spawned around the village");
    int kills = 0, deaths = 0;
    std::set<std::uint32_t> avoid;
    auto aloneUntil = Clock::now() + Ms(20000);
    const auto huntEnd = Clock::now() + Ms(250000);
    while (v.levelUps.empty() && Clock::now() < huntEnd)
    {
        std::uint32_t victim = 0;
        switch (fightThrall(bot, t, self, me, *c.monsters, victim, Clock::now() < aloneUntil, &avoid))
        {
        case FightOutcome::Killed:
            ++kills;
            aloneUntil = Clock::now() + Ms(20000);
            break;
        case FightOutcome::Died:
            ++deaths;
            if (victim)
                avoid.insert(victim);
            respawn(bot, self, me);
            aloneUntil = Clock::now() + Ms(20000);
            break;
        case FightOutcome::NoTarget:
            if (Clock::now() >= aloneUntil)
                avoid.clear();
            bot.pumpFor(Ms(1000));
            bot.keepAlive();
            break;
        }
        const auto& ss = v.selfStats;
        const auto restEnd = Clock::now() + Ms(45000);
        while (v.levelUps.empty() && !ss.empty() && ss.back().hp > 0 && ss.back().hp * 100 < ss.back().maxHp * 85 &&
               Clock::now() < restEnd)
        {
            bot.pumpFor(Ms(1000));
            bot.keepAlive();
        }
    }
    if (v.levelUps.empty())
        fail(std::format("no level-up after {} kill(s) and {} death(s)", kills, deaths));
    const auto heavyUsed = std::count_if(v.skillsUsed.begin(), v.skillsUsed.end(), [&](const vn::SkillUsed& u) {
        return u.casterId == me && u.skillId == kHeavy && u.rank == 1;
    });
    const bool heavyCooldown = std::any_of(v.cooldowns.begin(), v.cooldowns.end(), [&](const vn::SkillCooldowns& cd) {
        return cd.entries.size() == 1 && cd.entries[0].skillId == kHeavy && cd.entries[0].totalMs == 6000 &&
               cd.entries[0].remainingMs == 6000;
    });
    if (heavyUsed == 0 || !heavyCooldown)
    {
        std::string reasons;
        for (const auto& f : v.actionFails)
            if (f.skillId == kHeavy)
                reasons += std::format(" {}", vn::actionFailReasonName(f.reason));
        const auto events = std::count_if(v.combatEvents.begin(), v.combatEvents.end(),
                                          [&](const vn::CombatEvent& e) { return e.sourceId == me && e.skillId == kHeavy; });
        fail(std::format("Ağır Darbe: {} SKILL_USED ({} in total), {} hit(s), cooldown report {}, {} cooldown packet(s), "
                         "failures:{}",
                         heavyUsed, v.skillsUsed.size(), events, heavyCooldown, v.cooldowns.size(), reasons));
    }
    // §4: a new SKILL_STATE with the level's point after the level-up.
    if (!bot.pumpUntil([&] { return v.skillStates.back().points == 1; }, Ms(3000)))
        fail("no SKILL_STATE with 1 point after LEVEL_UP");
    expectSkillState(v.skillStates.back(), 1, 1, {{kHeavy, 1}}, "after the level-up");

    // 3. Learn Kalkan Duvarı (Siper, tier 1): stored, SKILL_STATE follows; the bar takes it.
    if (learnSkill(bot, kCleave).result != vn::SkillLearnResult::TierLocked)
        fail("Yarma at level 2 was not TierLocked");
    const auto learned = learnSkill(bot, kWall);
    if (learned.result != vn::SkillLearnResult::Ok || learned.rank != 1 || learned.unspent != 0)
        fail(std::format("learning Kalkan Duvarı: {} rank {} unspent {}", vn::skillLearnResultName(learned.result),
                         learned.rank, learned.unspent));
    if (!bot.pumpUntil([&] { return v.skillStates.back().unspent == 0; }, Ms(2000)))
        fail("no SKILL_STATE after learning");
    expectSkillState(v.skillStates.back(), 1, 0, {{kHeavy, 1}, {kWall, 1}}, "after learning");
    if (learnSkill(bot, kWall).result != vn::SkillLearnResult::NotEnoughPoints)
        fail("a second point at level 2 was not NotEnoughPoints");
    const auto barRes = setSkillBar(bot, {kHeavy, kWall, 0, 0, 0, 0, 0, 0});
    if (barRes.result != vn::SkillBarResult::Ok || barRes.slots != vn::SkillBarSlots{kHeavy, kWall, 0, 0, 0, 0, 0, 0})
        fail("setting the bar failed");

    // 4. Use it: EFFECT_APPLY on the bot (defense +40 % for 12 s), the cooldown report, CHAR_STATS with
    // the higher defense; EFFECT_REMOVE(Expired) after 12 s and the defense back.
    const std::uint16_t defenseBefore = v.charStats.back().defense;
    const auto buff = useSelfBuff(bot, me, kWall, defenseUp);
    if (buff.effect.amount != 40 || buff.effect.durationMs != 12000 || buff.effect.sourceId != me)
        fail(std::format("Kalkan Duvarı's EFFECT_APPLY amount {} duration {}", buff.effect.amount, buff.effect.durationMs));
    if (!bot.pumpUntil([&] { return v.charStats.back().defense > defenseBefore; }, Ms(2000)))
        fail("CHAR_STATS did not show the higher defense");
    const std::uint16_t defenseBuffed = v.charStats.back().defense;
    if (!bot.pumpUntil([&] {
            return std::any_of(v.cooldowns.begin(), v.cooldowns.end(), [&](const vn::SkillCooldowns& cd) {
                return cd.entries.size() == 1 && cd.entries[0].skillId == kWall && cd.entries[0].totalMs == 25000;
            });
        }, Ms(2000)))
        fail("no SKILL_COOLDOWNS for Kalkan Duvarı");
    if (!v.hasEffect(me, kWall, defenseUp))
        fail("the bot's effect list does not hold Kalkan Duvarı");
    if (!bot.pumpUntil([&] { return !v.hasEffect(me, kWall, defenseUp); }, Ms(14000)))
        fail("Kalkan Duvarı did not expire");
    if (v.effectRemoves.empty() || v.effectRemoves.back().reason != vn::EffectRemoveReason::Expired)
        fail("no EFFECT_REMOVE(Expired)");
    if (!bot.pumpUntil([&] { return v.charStats.back().defense == defenseBefore; }, Ms(2000)))
        fail("the defense did not go back after the buff");

    // 5. Leave and enter again: ranks and bar stored, the running cooldown remembered (§1.7).
    bot.dropPongs();
    bot.send(vn::LeaveWorldReq{});
    (void)bot.expect<vn::LeaveWorldRes>();
    enter = enterWorld(bot, charId, t, self);
    me = enter.entityId;
    expectSkillState(v.skillStates.front(), 1, 0, {{kHeavy, 1}, {kWall, 1}}, "after re-entering");
    if (v.skillBars.front().slots != vn::SkillBarSlots{kHeavy, kWall, 0, 0, 0, 0, 0, 0})
        fail("the bar was not kept after re-entering");
    if (!bot.pumpUntil([&] { return !v.cooldowns.empty(); }, Ms(2000)) || v.cooldowns.front().entries.empty() ||
        v.cooldowns.front().entries[0].skillId != kWall || v.cooldowns.front().entries[0].remainingMs > 13000)
        fail("the running Kalkan Duvarı cooldown was not reported on entry");
    const std::uint32_t remembered = v.cooldowns.front().entries[0].remainingMs;

    // 6. A flood of skill requests: 10 per second are fine, the 11th closes the connection.
    bot.pumpFor(Ms(1100));
    for (int i = 0; i < 10; ++i)
        bot.send(vn::SkillLearnReq{bot.nextSeq(), kWall});
    bot.send(vn::SkillLearnReq{bot.nextSeq(), kWall});
    for (;;)
    {
        const Received m = bot.next("waiting for ERROR(Flood)");
        if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::Pong) ||
            m.opcode == static_cast<std::uint16_t>(vn::Opcode::SkillLearnRes))
            continue;
        vn::Error err;
        if (m.opcode != static_cast<std::uint16_t>(vn::Opcode::Error) || !vn::decodeMessage(m.payload, err) ||
            err.errorCode != vn::ErrorCode::Flood)
            fail(std::format("the 11th skill request within a second got {} (expected ERROR(Flood))",
                             vn::opcodeName(static_cast<vn::Opcode>(m.opcode))));
        break;
    }
    bot.expectClosed(true, bot.timeout());
    expectNoViewErrors(bot, "skills view");
    expectNoZoneChange(bot, self, "skills");
    if (!c.opts.stateFile.empty())
    {
        std::ofstream out(c.opts.stateFile, std::ios::trunc);
        out << name << "\n";
        if (!out)
            fail("cannot write --state-file");
    }
    return std::format("level 2 after {} kill(s), {} death(s), Ağır Darbe used {}x; learned Kalkan Duvarı, defense {} -> "
                       "{} (+40 %) for 12 s, expired; ranks, bar and cooldown ({} ms left) kept after re-entering; flood closed",
                       kills, deaths, heavyUsed, defenseBefore, defenseBuffed, remembered);
}

// ---- zones: homelands, gates, refusals, relogs (docs/PROTOCOL_ZONES.md) ----

namespace {

const assets::TerrainData& zoneTerrainOf(const Context& c, std::uint16_t zone)
{
    const auto it = c.zoneTerrains.find(zone);
    if (it == c.zoneTerrains.end() || !it->second)
        fail(std::format("no terrain for zone {} (zones.toml terrain next to --terrain)", zone));
    return *it->second;
}

const game::ZoneDef& zoneDef(std::uint16_t id)
{
    const game::ZoneDef* z = game::zones().find(id);
    if (!z)
        fail(std::format("zone {} is not in gamedata/zones.toml", id));
    return *z;
}

Point markerOf(const assets::TerrainData& t, std::string_view name, std::uint16_t zone)
{
    const assets::SpawnPoint* s = t.findSpawn(name);
    if (!s)
        fail(std::format("zone {} has no marker '{}'", zone, name));
    return {s->position.x, s->position.z};
}

const game::GateDef& gateTo(const game::ZoneDef& from, std::uint16_t to)
{
    for (const auto& g : from.gates)
        if (g.toZone == to)
            return g;
    fail(std::format("zone {} has no gate to zone {}", from.id, to));
}

// A BFS path from `from` to the first cell for which `goal` holds that never enters a gate trigger
// of the zone (+1 m) other than `through` (empty = none).
std::vector<Point> pathInZone(const assets::TerrainData& t, const game::ZoneDef& z, Point from,
                              const std::function<bool(Point)>& goal, std::string_view through = {})
{
    const Nav nav(t);
    std::vector<std::pair<Point, float>> avoid;
    for (const auto& g : z.gates)
        if (g.marker != through)
            avoid.push_back({markerOf(t, g.marker, z.id), g.radius + 1.0f});
    auto path = nav.bfs(
        from, [&](int i, int j) { return goal(nav.centre(i, j)); }, 2'000'000,
        [&](int i, int j) {
            const Point p = nav.centre(i, j);
            for (const auto& [at, r] : avoid)
                if (distance(p, at) <= r)
                    return true;
            return false;
        });
    if (!path)
        fail(std::format("no walkable path in zone {} from ({:.1f}, {:.1f})", z.id, from.x, from.z));
    return *path;
}

std::vector<Point> pathIntoGate(const assets::TerrainData& t, const game::ZoneDef& z, Point from, const game::GateDef& g)
{
    const Point at = markerOf(t, g.marker, z.id);
    return pathInZone(
        t, z, from, [&](Point p) { return distance(p, at) <= g.radius - 0.75f; }, g.marker);
}

std::vector<Point> pathAwayInZone(const assets::TerrainData& t, const game::ZoneDef& z, Point from, float minDist,
                                  std::string_view through = {})
{
    return pathInZone(t, z, from, [&](Point p) { return distance(p, from) >= minDist; }, through);
}

// Walks `path` like walkPath but stops sending as soon as ZONE_CHANGE_BEGIN arrives (moves after it
// would be dropped anyway); at the end of the path waits up to `linger` for it. Fails on a correction.
std::optional<vn::ZoneChangeBegin> walkIntoGate(Bot& bot, const assets::TerrainData& t, Self& self,
                                                const std::vector<Point>& path, Ms linger,
                                                const std::function<void()>& idle = nullptr)
{
    const std::size_t begins = bot.view().zoneBegins.size();
    const std::size_t corrections = bot.view().corrections.size();
    const auto begun = [&] { return bot.view().zoneBegins.size() > begins; };
    const std::uint8_t moving = static_cast<std::uint8_t>(game::kMoveMoving | game::kMoveRunning);
    const float speed = game::kRunSpeed * kSpeedSafety;
    sendMove(bot, t, self, self.p(), moving);
    auto last = Clock::now();
    std::size_t next = 1;
    while (next < path.size() && !begun())
    {
        waitPumping(bot, kStep, idle);
        bot.keepAlive();
        if (begun())
            break;
        const auto now = Clock::now();
        const float budget = speed * std::chrono::duration<float>(now - last).count();
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
        if (bot.view().corrections.size() > corrections)
            fail(std::format("POSITION_CORRECT while walking into a gate at ({:.2f}, {:.2f})", self.x, self.z));
    }
    if (!begun())
    {
        sendMove(bot, t, self, self.p(), game::kMoveRunning); // stop inside the trigger
        const auto until = Clock::now() + linger;
        while (!begun() && Clock::now() < until)
        {
            waitPumping(bot, Ms(20), idle);
            bot.keepAlive();
        }
    }
    if (!begun())
        return std::nullopt;
    return bot.view().zoneBegins.back();
}

// ZONE_CHANGE after ZONE_CHANGE_BEGIN, then the entry packets in order (INVENTORY, COINS,
// SKILL_STATE, SKILL_BAR before any ENTITY_SPAWN), then ZONE_LOADED_REQ (PROTOCOL_ZONES.md §3.3).
vn::ZoneChange arrive(Bot& bot, const Context& c, std::uint16_t from, std::uint16_t to, Self& self)
{
    const auto zc = bot.expectSkippingPongs<vn::ZoneChange>(); // typically 1-3 ticks after ZONE_CHANGE_BEGIN
    if (zc.result != vn::ZoneChangeResult::Ok || zc.fromZone != from || zc.enter.zoneId != to)
        fail(std::format("ZONE_CHANGE result {} from {} to {} (expected Ok from {} to {})", static_cast<int>(zc.result),
                         zc.fromZone, zc.enter.zoneId, from, to));
    const auto& t = zoneTerrainOf(c, to);
    const auto& e = zc.enter;
    if (e.walkSpeed != game::kWalkSpeed || e.runSpeed != game::kRunSpeed || e.isDead || e.entityId == 0)
        fail("ZONE_CHANGE: unexpected speeds, entity or dead flag");
    if (!t.isWalkable(e.x, e.z) || std::abs(e.y - t.heightAt(e.x, e.z)) > kPosEps)
        fail(std::format("ZONE_CHANGE position ({:.2f}, {:.2f}, {:.2f}) is not on zone {}'s walkable ground", e.x, e.y,
                         e.z, to));
    const auto& v = bot.view();
    if (v.zoneBeginIndex == 0 || v.zoneBeginIndex >= bot.lastIndex())
        fail("ZONE_CHANGE without a ZONE_CHANGE_BEGIN before it");
    if (!bot.pumpUntil([&] { return !v.skillBars.empty() || bot.closed(); }, bot.timeout()) || !v.inventory ||
        v.coins.empty() || v.skillStates.empty())
        fail("no INVENTORY / COINS / SKILL_STATE / SKILL_BAR after ZONE_CHANGE");
    if (v.inventoryIndex != bot.lastIndex() + 1 || v.firstCoinsIndex != v.inventoryIndex + 1 ||
        v.firstSkillStateIndex != v.firstCoinsIndex + 1)
        fail("the entry packets after ZONE_CHANGE are not INVENTORY, COINS, SKILL_STATE in that order");
    if (v.firstSpawnIndex != 0 && v.firstSpawnIndex < v.firstSkillStateIndex)
        fail("ENTITY_SPAWN arrived before the entry packets of the new zone");
    bot.send(vn::ZoneLoadedReq{to});
    self = {e.x, e.y, e.z, e.yaw};
    return zc;
}

// Every monster in view belongs to a spawn of `zone` (its spawn circle + leash + 10 m). Returns how many.
std::size_t checkMonstersOfZone(Bot& bot, const Context& c, std::uint16_t zone)
{
    std::size_t n = 0;
    for (const auto& [id, r] : bot.view().entities)
    {
        if (r.spawn.entityType != vn::kEntityTypeMonster)
            continue;
        ++n;
        const Point at = r.last ? Point{r.last->x, r.last->z} : Point{r.spawn.x, r.spawn.z};
        bool ok = !c.monsters; // without the data nothing to check against
        if (c.monsters)
            for (const auto& s : c.monsters->spawns())
            {
                const game::MonsterTemplate* tm = c.monsters->find(s.monster);
                if (s.zone == zone && s.monster == r.spawn.templateId && tm &&
                    distance(at, {s.x, s.z}) <= s.radius + tm->leashRadius + 10.0f)
                    ok = true;
            }
        if (!ok)
            fail(std::format("zone {}: monster {} '{}' (template {}) at ({:.1f}, {:.1f}) belongs to no spawn of this zone",
                             zone, id, r.spawn.name, r.spawn.templateId, at.x, at.z));
    }
    return n;
}

void leaveWorld(Bot& bot)
{
    bot.send(vn::LeaveWorldReq{});
    if (bot.expectSkippingPongs<vn::LeaveWorldRes>().result != vn::kLeaveWorldOk)
        fail("LEAVE_WORLD_RES not ok");
}

// Walks a few metres (away from every gate), leaves, enters again: same zone, same position.
void relogHere(Bot& bot, const Context& c, std::uint32_t charId, std::uint16_t zone, Self& self)
{
    const auto& t = zoneTerrainOf(c, zone);
    if (walkPath(bot, t, self, pathAwayInZone(t, zoneDef(zone), self.p(), 6.0f), false) != 0)
        fail(std::format("corrections while walking in zone {}", zone));
    leaveWorld(bot);
    Self again;
    (void)enterWorld(bot, charId, t, again, zone);
    if (distance(again.p(), self.p()) > kPosEps)
        fail(std::format("zone {}: re-entered at ({:.2f}, {:.2f}) instead of the saved ({:.2f}, {:.2f})", zone, again.x,
                         again.z, self.x, self.z));
    self = again;
}

bool seesPlayer(Bot& bot, const std::string& name)
{
    for (const auto& [id, r] : bot.view().entities)
        if (r.spawn.entityType == vn::kEntityTypePlayer && r.spawn.name == name)
            return true;
    return false;
}

struct ZoneBot
{
    Bot bot;
    Self self;
    std::uint32_t charId = 0;
    std::string name;
    std::uint16_t zone = 0;
    explicit ZoneBot(Ms timeout) : bot(timeout) {}
};

// Creates a character of `race` (it must start in its home zone) and enters the world.
void createAndEnter(ZoneBot& z, const Context& c, const std::string& user, int race)
{
    z.bot.connectOrFail(c.settings);
    loginAs(z.bot, user, c.opts.password);
    vn::CharCreateReq req;
    req.name = randomCharacterName();
    req.race = static_cast<std::uint8_t>(race);
    req.charClass = 1;
    z.bot.send(req);
    const auto res = z.bot.expect<vn::CharCreateRes>();
    if (res.result != vn::CharCreateResult::Ok)
        fail(std::format("CHAR_CREATE_RES result {}", static_cast<int>(res.result)));
    const std::uint16_t home = game::zones().homeZone(static_cast<std::uint8_t>(race));
    if (res.character.zoneId != home)
        fail(std::format("a new race-{} character is in zone {} (expected its home zone {})", race, res.character.zoneId,
                         home));
    z.charId = res.character.charId;
    z.name = res.character.name;
    const auto& t = zoneTerrainOf(c, home);
    (void)enterWorld(z.bot, z.charId, t, z.self, home);
    z.zone = home;
    const Point spawn = markerOf(t, game::raceSpawnMarker(static_cast<std::uint8_t>(race)), home);
    if (distance(z.self.p(), spawn) > 2.5f)
        fail(std::format("a new character entered {:.1f} m from its home spawn", distance(z.self.p(), spawn)));
}

// Takes the gate of the bot's zone to `to`: walk in, ZONE_CHANGE_BEGIN, ZONE_CHANGE at the arrival
// marker, ZONE_LOADED_REQ.
void takeGate(ZoneBot& z, const Context& c, std::uint16_t to, const std::function<void()>& idle = nullptr)
{
    const auto& def = zoneDef(z.zone);
    const auto& gate = gateTo(def, to);
    const auto& t = zoneTerrainOf(c, z.zone);
    const auto begin = walkIntoGate(z.bot, t, z.self, pathIntoGate(t, def, z.self.p(), gate), Ms(4000), idle);
    if (!begin)
        fail(std::format("walked into gate '{}' of zone {} but no ZONE_CHANGE_BEGIN came", gate.marker, z.zone));
    if (begin->fromZone != z.zone || begin->toZone != to)
        fail(std::format("ZONE_CHANGE_BEGIN {} -> {} (expected {} -> {})", begin->fromZone, begin->toZone, z.zone, to));
    (void)arrive(z.bot, c, z.zone, to, z.self);
    const auto& target = zoneTerrainOf(c, to);
    const Point marker = markerOf(target, gate.arrive, to);
    if (distance(z.self.p(), marker) > 2.05f) // spread like a spawn point (world.spawn_spread_m 2 m)
        fail(std::format("arrived {:.2f} m from '{}' of zone {}", distance(z.self.p(), marker), gate.arrive, to));
    for (const auto& g : zoneDef(to).gates)
        if (distance(z.self.p(), markerOf(target, g.marker, to)) <= g.radius)
            fail(std::format("arrived inside gate '{}' of zone {}", g.marker, to));
    z.zone = to;
}

} // namespace

std::string scenarioZones(const Context& c)
{
    if (game::zones().empty())
        fail("gamedata/zones.toml not found or invalid");
    if (c.opts.phase == "check")
    {
        // After vo_admin move-character: --state-file holds "name\nzone\nmarker"; the character must
        // enter exactly at that marker of that zone.
        std::ifstream in(c.opts.stateFile);
        std::string name, zoneText, marker;
        if (!std::getline(in, name) || !std::getline(in, zoneText) || !std::getline(in, marker))
            fail("--phase check needs --state-file with name, zone and marker");
        const auto zone = static_cast<std::uint16_t>(std::stoi(zoneText));
        Bot bot(c.opts.timeout);
        bot.connectOrFail(c.settings);
        loginAs(bot, c.user, c.opts.password);
        std::uint32_t charId = 0;
        for (const auto& s : listChars(bot))
            if (s.name == name)
            {
                charId = s.charId;
                if (s.zoneId != zone)
                    fail(std::format("'{}' is listed in zone {} (expected {})", name, s.zoneId, zone));
            }
        if (charId == 0)
            fail(std::format("no character '{}'", name));
        const auto& t = zoneTerrainOf(c, zone);
        Self self;
        (void)enterWorld(bot, charId, t, self, zone);
        const Point at = markerOf(t, marker, zone);
        if (distance(self.p(), at) > 0.05f)
            fail(std::format("entered {:.2f} m from '{}'", distance(self.p(), at), marker));
        bot.send(vn::Logout{});
        return std::format("'{}' entered zone {} at '{}'", name, zone, marker);
    }
    if (c.opts.phase == "pair")
    {
        // Two Aydor in Tanyeri see each other, a Kargan in Demirağız sees neither; one Aydor goes
        // to Eşik (the other sees it go), the second follows (the first sees it appear).
        ZoneBot a1(c.opts.timeout), a2(c.opts.timeout), k(c.opts.timeout);
        createAndEnter(a1, c, truncUser(c.user, "_a1"), 1);
        createAndEnter(a2, c, truncUser(c.user, "_a2"), 1);
        createAndEnter(k, c, truncUser(c.user, "_k"), 2);
        const auto pumpAll = [&] {
            for (ZoneBot* z : {&a1, &a2, &k})
            {
                z->bot.poll();
                z->bot.keepAlive(); // the others idle while one walks (the server's idle timeout is 30 s)
            }
        };
        const auto until = [&](const std::function<bool()>& cond, std::string_view what) {
            const auto deadline = Clock::now() + c.opts.timeout;
            while (!cond())
            {
                if (Clock::now() > deadline)
                    fail(std::string(what));
                pumpAll();
                a1.bot.keepAlive();
                a2.bot.keepAlive();
                k.bot.keepAlive();
                std::this_thread::sleep_for(Ms(2));
            }
        };
        until([&] { return seesPlayer(a1.bot, a2.name) && seesPlayer(a2.bot, a1.name); },
              "the two Aydor in Tanyeri do not see each other");
        waitPumping(a1.bot, Ms(1000), pumpAll);
        if (seesPlayer(k.bot, a1.name) || seesPlayer(k.bot, a2.name) || seesPlayer(a1.bot, k.name) ||
            seesPlayer(a2.bot, k.name))
            fail("players of Tanyeri and Demirağız see each other");
        std::this_thread::sleep_for(Ms(2100)); // armed
        takeGate(a1, c, 1, pumpAll);
        until([&] { return !seesPlayer(a2.bot, a1.name); }, "the Aydor left in Tanyeri still sees the one that went to Eşik");
        takeGate(a2, c, 1, pumpAll);
        until([&] { return seesPlayer(a1.bot, a2.name) && seesPlayer(a2.bot, a1.name); },
              "the two Aydor do not see each other appear in Eşik");
        waitPumping(a1.bot, Ms(500), pumpAll);
        if (seesPlayer(k.bot, a1.name) || seesPlayer(k.bot, a2.name))
            fail("the Kargan in Demirağız sees players of Eşik");
        for (ZoneBot* z : {&a1, &a2, &k})
        {
            expectNoViewErrors(z->bot, z->name);
            if (!z->bot.view().corrections.empty())
                fail(std::format("{} got {} correction(s)", z->name, z->bot.view().corrections.size()));
            z->bot.send(vn::Logout{});
        }
        return "two Aydor see each other in Tanyeri, a Kargan in Demirağız sees neither; one goes to Eşik (despawn), "
               "the second follows (appears to the first)";
    }

    // One race through its homeland, Eşik and the closed gate of the other race's homeland.
    const int race = c.opts.race == 2 ? 2 : 1;
    const std::uint8_t r8 = static_cast<std::uint8_t>(race);
    const std::uint16_t home = game::zones().homeZone(r8);
    const std::uint16_t other = game::zones().homeZone(static_cast<std::uint8_t>(3 - race));
    const std::uint16_t esik = 1;
    ZoneBot z(c.opts.timeout);
    createAndEnter(z, c, c.user, race);
    waitPumping(z.bot, Ms(800), nullptr);
    const std::size_t homeMonsters = checkMonstersOfZone(z.bot, c, home);
    relogHere(z.bot, c, z.charId, home, z.self);
    std::this_thread::sleep_for(Ms(2100)); // armed

    // Homeland -> Eşik.
    takeGate(z, c, esik);
    waitPumping(z.bot, Ms(1000), nullptr);
    std::size_t esikMonsters = checkMonstersOfZone(z.bot, c, esik);
    relogHere(z.bot, c, z.charId, esik, z.self);
    std::this_thread::sleep_for(Ms(2100));

    // The other race's gate: refused once, nothing else.
    const auto& esikDef = zoneDef(esik);
    const auto& t1 = zoneTerrainOf(c, esik);
    const auto& closed = gateTo(esikDef, other);
    const std::size_t refusalsBefore = z.bot.view().gateRefusals.size();
    {
        const auto begin = walkIntoGate(z.bot, t1, z.self, pathIntoGate(t1, esikDef, z.self.p(), closed), Ms(3500));
        if (begin)
            fail(std::format("a race-{} character was let through '{}' into zone {}", race, closed.marker, other));
    }
    esikMonsters = std::max(esikMonsters, checkMonstersOfZone(z.bot, c, esik));
    if (!z.bot.pumpUntil([&] { return z.bot.view().gateRefusals.size() > refusalsBefore; }, Ms(3500)))
        fail(std::format("no ZONE_GATE_REFUSED at '{}'", closed.marker));
    waitPumping(z.bot, Ms(4000), nullptr); // standing in the gate: no more
    const auto& refusals = z.bot.view().gateRefusals;
    if (refusals.size() != refusalsBefore + 1)
        fail(std::format("{} ZONE_GATE_REFUSED while standing in the gate (expected 1)", refusals.size() - refusalsBefore));
    if (refusals.back().toZone != other || refusals.back().reason != vn::GateRefusal::RaceForbidden)
        fail("ZONE_GATE_REFUSED with the wrong zone or reason");
    if (!z.bot.view().zoneBegins.empty() && z.bot.view().zoneBegins.back().toZone == other)
        fail("a ZONE_CHANGE_BEGIN to the closed homeland");
    if (walkPath(z.bot, t1, z.self, pathAwayInZone(t1, esikDef, z.self.p(), closed.radius + 4.0f, closed.marker), true) != 0)
        fail("corrections while stepping out of the closed gate");

    // Back home.
    std::this_thread::sleep_for(Ms(500));
    takeGate(z, c, home);
    waitPumping(z.bot, Ms(500), nullptr);
    (void)checkMonstersOfZone(z.bot, c, home);
    relogHere(z.bot, c, z.charId, home, z.self);
    expectNoViewErrors(z.bot, "zones view");
    if (!z.bot.view().corrections.empty())
        fail(std::format("{} correction(s)", z.bot.view().corrections.size()));
    z.bot.send(vn::Logout{});
    if (!c.opts.stateFile.empty())
    {
        std::ofstream out(c.opts.stateFile, std::ios::trunc);
        out << z.name << "\n" << home << "\n";
        if (!out)
            fail("cannot write --state-file");
    }
    return std::format("race {}: created in zone {}, relogged there, through '{}' to Eşik (arrival '{}'), relogged, "
                       "refused once at '{}', back home; monsters in view: {} in the homeland, {} in Eşik, all of their zone",
                       race, home, gateTo(zoneDef(home), esik).marker, gateTo(zoneDef(home), esik).arrive, closed.marker,
                       homeMonsters, esikMonsters);
}

// ---- zonesrace: zone changes that overlap (docs/PROTOCOL_ZONES.md §3) ----
//
// --phase same: two Aydor walk into gate_esik of Tanyeri in lockstep (their MOVE_REQs that cross
//   the trigger leave in the same millisecond, so the server sees both in one tick), arrive in Eşik,
//   see each other once, and come back the same way together.
// --phase cross: one Aydor goes Tanyeri -> Eşik while another goes Eşik -> Tanyeri through the same
//   gate pair in lockstep; an observer in each zone sees the right one leave and the right one appear.
// --phase logout: (server with world.testing_zone_arrival_delay_ms) LOGOUT right after
//   ZONE_CHANGE_BEGIN: the close waits for the arrival's write, the character is in Eşik afterwards and
//   nobody saw it appear there; then the same with LEAVE_WORLD_REQ and a new ENTER_WORLD_REQ.
// --phase drop: (same server) the connection dropped right after ZONE_CHANGE_BEGIN; a new login enters
//   at once (while the change is still pending) and once after it finished: Eşik, the arrival marker,
//   the observer sees the character exactly once.

namespace {

// A spot to stand on just outside `gate` (trigger radius + 1.6 m) from which a straight walk reaches
// 0.8 m inside the trigger, and that inside point.
struct GateApproach
{
    Point stand;
    Point inside;
};

std::vector<GateApproach> gateApproaches(const assets::TerrainData& t, const game::ZoneDef& z, const game::GateDef& gate)
{
    const Point m = markerOf(t, gate.marker, z.id);
    std::vector<GateApproach> out;
    for (int k = 0; k < 36; ++k)
    {
        const float a = static_cast<float>(k) * 6.2831853f / 36.0f;
        const Point d{std::sin(a), std::cos(a)};
        const Point stand{m.x + d.x * (gate.radius + 1.6f), m.z + d.z * (gate.radius + 1.6f)};
        const Point inside{m.x + d.x * (gate.radius - 0.8f), m.z + d.z * (gate.radius - 0.8f)};
        if (!game::isClearSpot(t, stand.x, stand.z, 0.5f) || !t.isWalkable(inside.x, inside.z) ||
            !game::isSegmentWalkable(t, stand.x, stand.z, inside.x, inside.z))
            continue;
        bool nearOther = false;
        for (const auto& g : z.gates)
            if (g.marker != gate.marker && distance(stand, markerOf(t, g.marker, z.id)) <= g.radius + 1.5f)
                nearOther = true;
        if (!nearOther)
            out.push_back({stand, inside});
    }
    if (out.empty())
        fail(std::format("zone {}: no straight approach into gate '{}'", z.id, gate.marker));
    return out;
}

// The approach farthest from the first one but at most 3.5 m away (players do not collide: the first
// one itself if it is the only one).
const GateApproach& secondApproach(const std::vector<GateApproach>& app)
{
    const GateApproach* best = &app.front();
    for (const auto& g : app)
        if (distance(g.stand, app.front().stand) <= 3.5f &&
            distance(g.stand, app.front().stand) > distance(best->stand, app.front().stand))
            best = &g;
    return *best;
}

// Walks a bot (outside every trigger) to `to`; fails on a correction or a zone change.
void walkToSpot(ZoneBot& z, const Context& c, Point to, const std::function<void()>& idle)
{
    const auto& t = zoneTerrainOf(c, z.zone);
    if (distance(z.self.p(), to) < 0.05f)
        return;
    auto path = pathInZone(t, zoneDef(z.zone), z.self.p(), [&](Point p) { return distance(p, to) <= 0.9f; });
    if (game::isSegmentWalkable(t, path.back().x, path.back().z, to.x, to.z))
        path.push_back(to);
    const std::size_t begins = z.bot.view().zoneBegins.size();
    if (walkPath(z.bot, t, z.self, path, true, idle) != 0)
        fail(std::format("{}: corrections while walking to ({:.1f}, {:.1f}) in zone {}", z.name, to.x, to.z, z.zone));
    if (z.bot.view().zoneBegins.size() != begins)
        fail(std::format("{}: changed zone while walking to a spot outside the gates", z.name));
}

// Bots that walk from their stand point into a gate together (the same step every 100 ms).
struct LockstepWalker
{
    ZoneBot* z = nullptr;
    Point inside;
    std::size_t beginsBefore = 0;
    std::optional<Clock::time_point> begunAt; // when this side saw ZONE_CHANGE_BEGIN
};

// Starts every walker in the same instant, moves them along their straight line at the same speed
// (so the steps that cross the triggers leave together) and stops sending for one as soon as its
// ZONE_CHANGE_BEGIN arrived. Fails unless every one got it within `linger` after its last step.
void walkIntoGatesTogether(std::vector<LockstepWalker>& ws, const Context& c, const std::function<void()>& others, Ms linger)
{
    const auto pollAll = [&] {
        for (auto& w : ws)
        {
            w.z->bot.poll();
            w.z->bot.keepAlive();
            if (!w.begunAt && w.z->bot.view().zoneBegins.size() > w.beginsBefore)
                w.begunAt = Clock::now();
        }
        if (others)
            others();
    };
    const std::uint8_t moving = static_cast<std::uint8_t>(game::kMoveMoving | game::kMoveRunning);
    for (auto& w : ws)
    {
        w.beginsBefore = w.z->bot.view().zoneBegins.size();
        sendMove(w.z->bot, zoneTerrainOf(c, w.z->zone), w.z->self, w.z->self.p(), moving);
    }
    const float speed = game::kRunSpeed * kSpeedSafety;
    auto last = Clock::now();
    const auto pending = [&] {
        return std::any_of(ws.begin(), ws.end(), [](const LockstepWalker& w) { return !w.begunAt; });
    };
    while (pending())
    {
        waitPumping(ws.front().z->bot, kStep, pollAll);
        const auto now = Clock::now();
        const float budget = speed * std::chrono::duration<float>(now - last).count();
        last = now;
        bool stepped = false;
        for (auto& w : ws)
        {
            if (w.begunAt)
                continue;
            const auto& t = zoneTerrainOf(c, w.z->zone);
            const float d = distance(w.z->self.p(), w.inside);
            if (d < 1e-3f)
                continue; // at the end: waits for ZONE_CHANGE_BEGIN
            Point pos = w.inside;
            if (d > budget)
                pos = {w.z->self.x + (w.inside.x - w.z->self.x) * budget / d, w.z->self.z + (w.inside.z - w.z->self.z) * budget / d};
            sendMove(w.z->bot, t, w.z->self, pos, moving);
            stepped = true;
            if (!w.z->bot.view().corrections.empty())
                fail(std::format("{}: POSITION_CORRECT while walking into the gate", w.z->name));
        }
        if (!stepped)
            break;
    }
    const auto until = Clock::now() + linger;
    while (pending() && Clock::now() < until)
        waitPumping(ws.front().z->bot, Ms(10), pollAll);
    for (auto& w : ws)
        if (!w.begunAt)
            fail(std::format("{}: no ZONE_CHANGE_BEGIN after walking into the gate together with the others", w.z->name));
}

// ZONE_CHANGE and the entry packets after a lockstep walk; checks the arrival marker.
void arriveAt(ZoneBot& z, const Context& c, std::uint16_t to, const game::GateDef& gate)
{
    const std::uint16_t from = z.zone;
    (void)arrive(z.bot, c, from, to, z.self);
    const auto& target = zoneTerrainOf(c, to);
    const Point marker = markerOf(target, gate.arrive, to);
    if (distance(z.self.p(), marker) > 2.05f)
        fail(std::format("{}: arrived {:.2f} m from '{}' of zone {}", z.name, distance(z.self.p(), marker), gate.arrive, to));
    z.zone = to;
}

// A spot 6+ m from `from` in zone `zone`, outside every gate (to step out of the way after arriving).
Point spotAway(const Context& c, std::uint16_t zone, Point from)
{
    const auto& t = zoneTerrainOf(c, zone);
    return pathAwayInZone(t, zoneDef(zone), from, 6.0f).back();
}

void pumpUntilAll(const std::function<bool()>& cond, const std::function<void()>& pumpAll, Ms timeout, std::string_view what)
{
    const auto deadline = Clock::now() + timeout;
    while (!cond())
    {
        if (Clock::now() > deadline)
            fail(std::string(what));
        pumpAll();
        std::this_thread::sleep_for(Ms(2));
    }
}

// How many ENTITY_SPAWNs of player `name` the bot received since its view was reset (the view keeps
// only the live ones, so count the spawn events by name through the entity map's history).
std::size_t spawnsOf(Bot& bot, const std::string& name, std::set<std::uint32_t>& seen)
{
    for (const auto& [id, r] : bot.view().entities)
        if (r.spawn.entityType == vn::kEntityTypePlayer && r.spawn.name == name)
            seen.insert(id);
    return seen.size();
}

void expectClean(ZoneBot& z)
{
    expectNoViewErrors(z.bot, z.name);
    if (!z.bot.view().corrections.empty())
        fail(std::format("{} got {} correction(s)", z.name, z.bot.view().corrections.size()));
}

} // namespace

std::string scenarioZonesRace(const Context& c)
{
    if (game::zones().empty())
        fail("gamedata/zones.toml not found or invalid");
    constexpr std::uint16_t kEsik = 1;
    const std::uint16_t tanyeri = game::zones().homeZone(1);
    const auto& tanDef = zoneDef(tanyeri);
    const auto& esikDef = zoneDef(kEsik);
    const auto& toEsik = gateTo(tanDef, kEsik);      // gate_esik in Tanyeri
    const auto& toTanyeri = gateTo(esikDef, tanyeri); // gate_aydor in Eşik
    const auto& tTan = zoneTerrainOf(c, tanyeri);
    const auto& tEsik = zoneTerrainOf(c, kEsik);
    const std::string phase = c.opts.phase.empty() ? "same" : c.opts.phase;
    const auto armed = [] { std::this_thread::sleep_for(Ms(game::kGateRearmMs + 200)); };

    if (phase == "same" || phase == "cross")
    {
        ZoneBot a(c.opts.timeout), b(c.opts.timeout), o1(c.opts.timeout), o2(c.opts.timeout);
        std::vector<ZoneBot*> all{&a, &b};
        if (phase == "cross")
            all = {&a, &b, &o1, &o2};
        createAndEnter(a, c, truncUser(c.user, "_a"), 1);
        createAndEnter(b, c, truncUser(c.user, "_b"), 1);
        if (phase == "cross")
        {
            createAndEnter(o1, c, truncUser(c.user, "_o1"), 1);
            createAndEnter(o2, c, truncUser(c.user, "_o2"), 1);
        }
        const auto pumpAll = [&] {
            for (ZoneBot* z : all)
            {
                z->bot.poll();
                z->bot.keepAlive();
            }
        };
        armed();
        if (phase == "same")
        {
            // ---- Two bots into the same gate in the same tick, there and back ----
            const auto app = gateApproaches(tTan, tanDef, toEsik);
            const GateApproach& pa = app.front();
            const GateApproach* pb = &secondApproach(app);
            walkToSpot(a, c, pa.stand, pumpAll);
            walkToSpot(b, c, pb->stand, pumpAll);
            waitPumping(a.bot, Ms(300), pumpAll);
            std::vector<LockstepWalker> ws{{&a, pa.inside}, {&b, pb->inside}};
            walkIntoGatesTogether(ws, c, pumpAll, Ms(4000));
            const double apartThere = std::abs(std::chrono::duration<double, std::milli>(*ws[0].begunAt - *ws[1].begunAt).count());
            arriveAt(a, c, kEsik, toEsik);
            arriveAt(b, c, kEsik, toEsik);
            if (a.bot.view().zoneBegins.back().toZone != kEsik || b.bot.view().zoneBegins.back().toZone != kEsik)
                fail("ZONE_CHANGE_BEGIN to the wrong zone");
            pumpUntilAll([&] { return seesPlayer(a.bot, b.name) && seesPlayer(b.bot, a.name); }, pumpAll, c.opts.timeout,
                         "the two Aydor that arrived together do not see each other in Eşik");
            waitPumping(a.bot, Ms(1000), pumpAll);
            std::set<std::uint32_t> seenA, seenB;
            if (spawnsOf(a.bot, b.name, seenA) != 1 || spawnsOf(b.bot, a.name, seenB) != 1)
                fail("a bot that arrived together with another sees it more than once");
            // And back home together (Eşik's gate_aydor; the arrival is outside every trigger, so the
            // gate re-arms after 2 s once they stepped away).
            walkToSpot(a, c, spotAway(c, kEsik, a.self.p()), pumpAll);
            armed();
            const auto back = gateApproaches(tEsik, esikDef, toTanyeri);
            const GateApproach& qa = back.front();
            const GateApproach* qb = &secondApproach(back);
            walkToSpot(a, c, qa.stand, pumpAll);
            walkToSpot(b, c, qb->stand, pumpAll);
            waitPumping(a.bot, Ms(300), pumpAll);
            std::vector<LockstepWalker> ws2{{&a, qa.inside}, {&b, qb->inside}};
            walkIntoGatesTogether(ws2, c, pumpAll, Ms(4000));
            const double apartBack = std::abs(std::chrono::duration<double, std::milli>(*ws2[0].begunAt - *ws2[1].begunAt).count());
            arriveAt(a, c, tanyeri, toTanyeri);
            arriveAt(b, c, tanyeri, toTanyeri);
            pumpUntilAll([&] { return seesPlayer(a.bot, b.name) && seesPlayer(b.bot, a.name); }, pumpAll, c.opts.timeout,
                         "the two Aydor do not see each other back in Tanyeri");
            waitPumping(a.bot, Ms(500), pumpAll);
            for (ZoneBot* z : all)
            {
                expectClean(*z);
                z->bot.send(vn::Logout{});
            }
            return std::format("two Aydor through gate '{}' together (ZONE_CHANGE_BEGIN {:.0f} ms apart), both in Eşik "
                               "seeing each other once, back through '{}' together ({:.0f} ms apart)",
                               toEsik.marker, apartThere, toTanyeri.marker, apartBack);
        }

        // ---- cross: a Tanyeri -> Eşik while b Eşik -> Tanyeri; o1 waits in Eşik, o2 in Tanyeri ----
        takeGate(b, c, kEsik, pumpAll);
        takeGate(o1, c, kEsik, pumpAll);
        const auto app = gateApproaches(tTan, tanDef, toEsik).front();
        const auto back = gateApproaches(tEsik, esikDef, toTanyeri).front();
        // The observers stand ~6 m from the gate the other side leaves by (and arrives next to).
        walkToSpot(o1, c, spotAway(c, kEsik, back.stand), pumpAll);
        walkToSpot(o2, c, spotAway(c, tanyeri, app.stand), pumpAll);
        walkToSpot(a, c, app.stand, pumpAll);
        walkToSpot(b, c, back.stand, pumpAll);
        armed();
        pumpUntilAll([&] { return seesPlayer(o1.bot, b.name) && seesPlayer(o2.bot, a.name); }, pumpAll, c.opts.timeout,
                     "the observers do not see the bots before the crossing");
        if (seesPlayer(o1.bot, a.name) || seesPlayer(o2.bot, b.name))
            fail("an observer sees a bot of the other zone before the crossing");
        std::vector<LockstepWalker> ws{{&a, app.inside}, {&b, back.inside}};
        walkIntoGatesTogether(ws, c, pumpAll, Ms(4000));
        const double apart = std::abs(std::chrono::duration<double, std::milli>(*ws[0].begunAt - *ws[1].begunAt).count());
        arriveAt(a, c, kEsik, toEsik);
        arriveAt(b, c, tanyeri, toTanyeri);
        pumpUntilAll(
            [&] {
                return seesPlayer(o1.bot, a.name) && !seesPlayer(o1.bot, b.name) && seesPlayer(o2.bot, b.name) &&
                       !seesPlayer(o2.bot, a.name);
            },
            pumpAll, c.opts.timeout, "after the crossing the observers do not see the right bots");
        waitPumping(a.bot, Ms(1000), pumpAll);
        if (seesPlayer(a.bot, b.name) || seesPlayer(b.bot, a.name))
            fail("the two bots that crossed see each other across zones");
        if (!seesPlayer(a.bot, o1.name) || !seesPlayer(b.bot, o2.name))
            fail("a bot that crossed does not see the observer of its new zone");
        for (ZoneBot* z : all)
        {
            expectClean(*z);
            z->bot.send(vn::Logout{});
        }
        return std::format("crossed '{}' (Tanyeri -> Eşik) and '{}' (Eşik -> Tanyeri) together ({:.0f} ms apart); the "
                           "observers saw the right one leave and the right one appear",
                           toEsik.marker, toTanyeri.marker, apart);
    }

    if (phase == "logout" || phase == "drop")
    {
        // An observer stands next to Eşik's arrival marker of gate_esik and watches.
        ZoneBot o(c.opts.timeout), m(c.opts.timeout);
        createAndEnter(o, c, truncUser(c.user, "_o"), 1);
        const std::string mUser = truncUser(c.user, "_m");
        createAndEnter(m, c, mUser, 1);
        std::vector<ZoneBot*> live{&o, &m};
        std::set<std::uint32_t> seenByObserver; // every entity of m's character the observer had in view
        const auto pumpAll = [&] {
            for (ZoneBot* z : live)
            {
                z->bot.poll();
                z->bot.keepAlive();
            }
            (void)spawnsOf(o.bot, m.name, seenByObserver);
        };
        armed();
        takeGate(o, c, kEsik, pumpAll);
        const Point arrival = markerOf(tEsik, toEsik.arrive, kEsik);
        walkToSpot(o, c, spotAway(c, kEsik, o.self.p()), pumpAll);
        const auto app = gateApproaches(tTan, tanDef, toEsik).front();
        seenByObserver.clear(); // m was in view in Tanyeri
        std::vector<std::string> done;

        // One gate walk of m that stops at ZONE_CHANGE_BEGIN; returns when it came.
        const auto intoGate = [&] {
            walkToSpot(m, c, app.stand, pumpAll);
            armed();
            std::vector<LockstepWalker> ws{{&m, app.inside}};
            walkIntoGatesTogether(ws, c, pumpAll, Ms(4000));
            return *ws.front().begunAt;
        };
        // A fresh connection of m's account: enters m's character, which must be in Eşik at the arrival.
        const auto reenter = [&](ZoneBot& again, std::string_view what) {
            again.bot.connectOrFail(c.settings);
            loginAs(again.bot, mUser, c.opts.password);
            const auto chars = listChars(again.bot);
            if (chars.size() != 1 || chars.front().charId != m.charId)
                fail(std::format("{}: the account lists {} character(s)", what, chars.size()));
            if (chars.front().zoneId != kEsik)
                fail(std::format("{}: the character is listed in zone {} (expected Eşik)", what, chars.front().zoneId));
            (void)enterWorld(again.bot, m.charId, tEsik, again.self, kEsik);
            if (distance(again.self.p(), arrival) > 2.05f)
                fail(std::format("{}: entered {:.2f} m from '{}'", what, distance(again.self.p(), arrival), toEsik.arrive));
            again.charId = m.charId;
            again.name = m.name;
            again.zone = kEsik;
        };
        const auto leaveTanyeri = [&](ZoneBot& z, Self at) {
            // Back to Tanyeri for the next round: m is in Eşik now.
            z.self = at;
            z.zone = kEsik;
            walkToSpot(z, c, spotAway(c, kEsik, z.self.p()), pumpAll);
            armed();
            takeGate(z, c, tanyeri, pumpAll);
        };
        const Ms delay = Ms(1000); // RunScenario sets world.testing_zone_arrival_delay_ms = 1500

        if (phase == "logout")
        {
            // 1. LOGOUT right after ZONE_CHANGE_BEGIN.
            const auto begun = intoGate();
            m.bot.send(vn::Logout{});
            m.bot.expectClosed(true, Ms(15000));
            const double waited = std::chrono::duration<double, std::milli>(Clock::now() - begun).count();
            if (waited < static_cast<double>(delay.count()))
                fail(std::format("the LOGOUT closed {:.0f} ms after ZONE_CHANGE_BEGIN: it did not wait for the arrival's "
                                 "write (is world.testing_zone_arrival_delay_ms set?)",
                                 waited));
            waitPumping(o.bot, Ms(1500), pumpAll);
            if (seenByObserver.size() != 0)
                fail("the observer saw the character that logged out between two zones appear");
            live = {&o};
            done.push_back(std::format("LOGOUT closed {:.0f} ms after ZONE_CHANGE_BEGIN", waited));
            // 2. A new login: in Eşik at the arrival.
            {
                ZoneBot again(c.opts.timeout);
                again.bot.connectOrFail(c.settings);
                loginAs(again.bot, mUser, c.opts.password);
                const auto chars = listChars(again.bot);
                if (chars.empty() || chars.front().zoneId != kEsik)
                    fail("after LOGOUT between two zones the character is not listed in Eşik");
                (void)enterWorld(again.bot, m.charId, tEsik, again.self, kEsik);
                if (distance(again.self.p(), arrival) > 2.05f)
                    fail(std::format("after LOGOUT: entered {:.2f} m from '{}'", distance(again.self.p(), arrival), toEsik.arrive));
                again.charId = m.charId;
                again.name = m.name;
                live = {&o, &again};
                pumpUntilAll([&] { return seesPlayer(o.bot, m.name); }, pumpAll, c.opts.timeout,
                             "the observer does not see the character after its new entry");
                // 3. LEAVE_WORLD_REQ right after ZONE_CHANGE_BEGIN, then ENTER_WORLD_REQ on the same connection.
                leaveTanyeri(again, again.self);
                const Point stand = app.stand;
                walkToSpot(again, c, stand, pumpAll);
                armed();
                std::vector<LockstepWalker> ws{{&again, app.inside}};
                walkIntoGatesTogether(ws, c, pumpAll, Ms(4000));
                const auto begun2 = *ws.front().begunAt;
                again.bot.send(vn::LeaveWorldReq{});
                const auto res = again.bot.expectSkippingPongs<vn::LeaveWorldRes>();
                const double waited2 = std::chrono::duration<double, std::milli>(Clock::now() - begun2).count();
                if (res.result != vn::kLeaveWorldOk)
                    fail("LEAVE_WORLD_RES between two zones not ok");
                if (waited2 < static_cast<double>(delay.count()))
                    fail(std::format("LEAVE_WORLD_RES {:.0f} ms after ZONE_CHANGE_BEGIN: it did not wait for the arrival", waited2));
                Self in;
                (void)enterWorld(again.bot, m.charId, tEsik, in, kEsik);
                if (distance(in.p(), arrival) > 2.05f)
                    fail(std::format("after LEAVE_WORLD_REQ: entered {:.2f} m from '{}'", distance(in.p(), arrival), toEsik.arrive));
                pumpUntilAll([&] { return seesPlayer(o.bot, m.name); }, pumpAll, c.opts.timeout,
                             "the observer does not see the character after LEAVE_WORLD_REQ and a new entry");
                done.push_back(std::format("LEAVE_WORLD_RES {:.0f} ms after ZONE_CHANGE_BEGIN, entered again in Eşik", waited2));
                expectNoViewErrors(again.bot, "the re-entered bot");
                again.bot.send(vn::Logout{});
                again.bot.expectClosed(true, Ms(15000));
            }
        }
        else
        {
            // 1. The connection drops right after ZONE_CHANGE_BEGIN; a new login enters at once, while
            //    the change is still pending (World::onEnter takes the arrival over).
            const auto begun = intoGate();
            m.bot.dropConnection();
            live = {&o};
            ZoneBot again(c.opts.timeout);
            reenter(again, "entering right after the drop");
            const double after = std::chrono::duration<double, std::milli>(Clock::now() - begun).count();
            live = {&o, &again};
            pumpUntilAll([&] { return seesPlayer(o.bot, m.name); }, pumpAll, c.opts.timeout,
                         "the observer does not see the character entered again after the drop");
            waitPumping(o.bot, Ms(2500), pumpAll); // past the testing delay: the old change must not show up
            if (seenByObserver.size() != 1)
                fail(std::format("the observer saw the dropped and re-entered character as {} entities (expected 1)",
                                 seenByObserver.size()));
            done.push_back(std::format("dropped, entered again {:.0f} ms after ZONE_CHANGE_BEGIN, seen once", after));
            // 2. The same, but the new login comes only after the dropped change finished.
            leaveTanyeri(again, again.self);
            walkToSpot(again, c, app.stand, pumpAll);
            armed();
            std::vector<LockstepWalker> ws{{&again, app.inside}};
            walkIntoGatesTogether(ws, c, pumpAll, Ms(4000));
            again.bot.dropConnection();
            live = {&o};
            seenByObserver.clear();
            waitPumping(o.bot, delay + Ms(2000), pumpAll);
            if (!seenByObserver.empty())
                fail("the observer saw a character that dropped between two zones arrive");
            ZoneBot third(c.opts.timeout);
            reenter(third, "entering after the dropped change finished");
            live = {&o, &third};
            pumpUntilAll([&] { return seesPlayer(o.bot, m.name); }, pumpAll, c.opts.timeout,
                         "the observer does not see the character entered after the dropped change");
            done.push_back("dropped, not seen arriving, entered again after the change finished");
            expectNoViewErrors(third.bot, "the re-entered bot");
            third.bot.send(vn::Logout{});
            third.bot.expectClosed(true, Ms(15000));
        }
        expectClean(o);
        o.bot.send(vn::Logout{});
        std::string text;
        for (const auto& d : done)
            text += (text.empty() ? "" : "; ") + d;
        return std::format("{}: {}; the character is in Eşik at '{}' every time", phase, text, toEsik.arrive);
    }
    fail("unknown --phase for zonesrace (same | cross | logout | drop)");
}

// ---- shop: merchants, buying, selling, potions, bar item slots (docs/PROTOCOL_SHOP.md) ----
//
// Deterministic: no monster is involved. Akçe comes from selling the mage's spare staff, the missing
// hp / mp from the server's testing hook world.testing_enter_vitals_percent = 10 (every entry
// starts at 10 %), the cooldown waits are fixed.

namespace {

constexpr std::uint8_t kShopMage = 3;
constexpr std::uint16_t kHpSmall = 16, kHpMedium = 17, kMpSmall = 18;
constexpr std::uint16_t kFrostStaffItem = 8, kEmberStaffItem = 4, kAshDustItem = 9;
constexpr std::uint16_t kEmberBoltSkill = 3;
constexpr std::uint32_t kTestVitalsPercent = 10;

std::uint32_t createCharacterOf(Bot& bot, int race, std::uint8_t cls, std::string& name)
{
    vn::CharCreateReq req;
    req.name = randomCharacterName();
    req.race = static_cast<std::uint8_t>(race);
    req.charClass = cls;
    req.face = 1;
    req.hair = 2;
    bot.send(req);
    const auto res = bot.expect<vn::CharCreateRes>();
    if (res.result != vn::CharCreateResult::Ok)
        fail(std::format("CHAR_CREATE_RES result {}", static_cast<int>(res.result)));
    name = res.character.name;
    return res.character.charId;
}

// The merchants of `zone` in npcs.toml: the potion seller (sells item 16) and the smith.
struct TownMerchants
{
    const game::NpcDef* potion = nullptr;
    const game::NpcDef* smith = nullptr;
};

TownMerchants merchantsOf(std::uint16_t zone)
{
    TownMerchants m;
    for (const auto& n : game::npcs().npcs())
    {
        if (n.zone != zone)
            continue;
        if (std::find(n.stock.begin(), n.stock.end(), kHpSmall) != n.stock.end())
            m.potion = &n;
        else
            m.smith = &n;
    }
    if (!m.potion || !m.smith)
        fail(std::format("gamedata/npcs.toml has no potion seller and smith in zone {}", zone));
    return m;
}

// Waits for the ENTITY_SPAWN of NPC `def` and checks it against the data (PROTOCOL_SHOP.md §1.3).
std::uint32_t expectNpc(Bot& bot, const game::NpcDef& def)
{
    std::uint32_t found = 0;
    if (!bot.pumpUntil(
            [&] {
                for (const auto& [id, r] : bot.view().entities)
                    if (r.spawn.entityType == vn::kEntityTypeNpc && r.spawn.templateId == def.id)
                        found = id;
                return found != 0;
            },
            Ms(3000)))
        fail(std::format("NPC {} '{}' was not spawned", def.id, def.name));
    const vn::EntitySpawn& s = bot.view().entities.at(found).spawn;
    if (s.name != def.name || std::abs(s.x - def.x) > kPosEps || std::abs(s.z - def.z) > kPosEps ||
        std::abs(game::normalizeYaw(s.yaw - def.yaw)) > 0.01f || s.level != 0 || s.hp != 0 || s.maxHp != 0 ||
        s.state != 0 || s.race != 0 || s.charClass != 0 || s.mainHandTemplate != 0 || s.flags != 0)
        fail(std::format("ENTITY_SPAWN of NPC {}: name '{}' at ({:.2f}, {:.2f}) yaw {:.3f} level {} hp {}/{} state {}",
                         def.id, s.name, s.x, s.z, s.yaw, s.level, s.hp, s.maxHp, s.state));
    return found;
}

// Shop requests share a cap of 10 per second (PROTOCOL_SHOP.md §6): the scenario sends at most one
// every 130 ms (the flood at the end is the only burst).
void paceShop(Bot& bot)
{
    static Clock::time_point last{};
    const auto wait = last + Ms(130) - Clock::now();
    if (wait > Clock::duration::zero())
        bot.pumpFor(std::chrono::duration_cast<Ms>(wait) + Ms(1));
    last = Clock::now();
}

vn::ShopOpenRes openShop(Bot& bot, std::uint32_t npc)
{
    paceShop(bot);
    bot.dropPongs();
    bot.send(vn::ShopOpenReq{npc});
    const auto res = bot.expect<vn::ShopOpenRes>();
    if (res.npcId != npc)
        fail(std::format("SHOP_OPEN_RES for {} (asked {})", res.npcId, npc));
    return res;
}

vn::ShopBuyRes buyItem(Bot& bot, std::uint32_t npc, std::uint16_t tmpl, std::uint16_t quantity)
{
    paceShop(bot);
    bot.dropPongs();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::ShopBuyReq{seq, npc, tmpl, quantity});
    const auto res = bot.expect<vn::ShopBuyRes>();
    if (res.seq != seq || res.templateId != tmpl || res.quantity != quantity)
        fail(std::format("SHOP_BUY_RES seq {} {} x {} (asked seq {} {} x {})", res.seq, res.quantity, res.templateId, seq,
                         quantity, tmpl));
    if (res.result != vn::ShopResult::Ok && res.cost != 0)
        fail("a refused SHOP_BUY_RES with a cost");
    return res;
}

vn::ShopSellRes sellItem(Bot& bot, std::uint32_t npc, std::uint8_t slot, std::uint16_t quantity)
{
    paceShop(bot);
    bot.dropPongs();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::ShopSellReq{seq, npc, slot, quantity});
    const auto res = bot.expect<vn::ShopSellRes>();
    if (res.seq != seq || res.quantity != quantity)
        fail(std::format("SHOP_SELL_RES seq {} quantity {} (asked seq {} quantity {})", res.seq, res.quantity, seq, quantity));
    if (res.result != vn::ShopResult::Ok && res.gained != 0)
        fail("a refused SHOP_SELL_RES with Akçe");
    return res;
}

vn::ItemUseRes useItem(Bot& bot, std::uint16_t tmpl, std::uint8_t slot)
{
    paceShop(bot);
    bot.dropPongs();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::ItemUseReq{seq, tmpl, slot});
    const auto res = bot.expect<vn::ItemUseRes>();
    if (res.seq != seq || res.templateId != tmpl)
        fail(std::format("ITEM_USE_RES seq {} template {} (asked seq {} template {})", res.seq, res.templateId, seq, tmpl));
    return res;
}

void expectShop(vn::ShopResult got, vn::ShopResult want, std::string_view what)
{
    if (got != want)
        fail(std::format("{}: {} (expected {})", what, vn::shopResultName(got), vn::shopResultName(want)));
}

void expectUse(vn::ItemUseResult got, vn::ItemUseResult want, std::string_view what)
{
    if (got != want)
        fail(std::format("{}: {} (expected {})", what, vn::itemUseResultName(got), vn::itemUseResultName(want)));
}

vn::SkillBarSetRes setBar(Bot& bot, const vn::SkillBarSlots& slots)
{
    bot.dropPongs();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::SkillBarSetReq{seq, slots});
    const auto res = bot.expect<vn::SkillBarSetRes>();
    if (res.seq != seq)
        fail("SKILL_BAR_SET_RES seq mismatch");
    return res;
}

// Applies every ITEM_UPDATE received since `from` to `inv` and returns the new count.
std::size_t applyUpdatesSince(Bot& bot, game::Inventory& inv, std::size_t from)
{
    const auto& u = bot.view().itemUpdates;
    for (std::size_t i = from; i < u.size(); ++i)
        if (!applyItemEntries(inv, u[i].items))
            fail("ITEM_UPDATE with an invalid place");
    return u.size();
}

std::optional<std::uint8_t> bagSlotOf(const game::Inventory& inv, std::uint16_t tmpl)
{
    for (std::uint8_t s = 0; s < game::kBagSlots; ++s)
        if (inv.at(game::ItemPlace::bag(s)).templateId == tmpl && !inv.at(game::ItemPlace::bag(s)).empty())
            return s;
    return std::nullopt;
}

std::uint8_t freeBagSlotAbove(const game::Inventory& inv, std::uint8_t from)
{
    for (std::uint8_t s = game::kBagSlots; s-- > from;)
        if (inv.at(game::ItemPlace::bag(s)).empty())
            return s;
    fail("no free bag slot");
}

// Walks (BFS over the walk mask) until within `stopAt` of `target`; fails on a correction.
void walkNear(Bot& bot, const assets::TerrainData& t, Self& self, Point target, float stopAt)
{
    if (distance(self.p(), target) <= stopAt)
        return;
    const Nav nav(t);
    const auto path = nav.bfs(
        self.p(), [&](int i, int j) { return distance(nav.centre(i, j), target) <= stopAt; }, 200000);
    if (!path)
        fail(std::format("no walkable path to ({:.1f}, {:.1f})", target.x, target.z));
    if (walkPath(bot, t, self, *path, true) != 0)
        fail("corrections while walking to a merchant");
}

std::uint32_t expectedVital(std::uint32_t max, bool hp)
{
    const auto v = static_cast<std::uint32_t>(static_cast<std::uint64_t>(max) * kTestVitalsPercent / 100);
    return hp ? std::max<std::uint32_t>(v, 1) : v;
}

// Entry at the testing vitals: hp / mp are 10 % of the maxima (else the server is not set up).
void expectTestVitals(const vn::EnterWorldRes& e)
{
    if (e.hp != expectedVital(e.maxHp, true) || e.mp != expectedVital(e.maxMp, false))
        fail(std::format("entered with hp {}/{} mp {}/{}: run the server with world.testing_enter_vitals_percent = {}",
                         e.hp, e.maxHp, e.mp, e.maxMp, kTestVitalsPercent));
}

} // namespace

std::string scenarioShop(const Context& c)
{
    if (!c.items)
        fail("gamedata/items.toml is needed");
    if (game::npcs().empty())
        fail("gamedata/npcs.toml not found or invalid");
    const game::ItemCatalog& items = *c.items;
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    auto& v = bot.view();

    if (c.opts.phase == "check")
    {
        // A later entry (with -Database: on a new server process): balance, potions and bar as stored.
        std::ifstream in(c.opts.stateFile);
        std::string name;
        std::uint32_t zone = 0, coins = 0;
        vn::SkillBarSlots bar{};
        if (!(in >> name >> zone >> coins))
            fail("cannot read name, zone and balance from --state-file");
        for (auto& s : bar)
            if (!(in >> s))
                fail("cannot read the bar from --state-file");
        std::map<std::uint16_t, std::uint32_t> expected;
        std::uint32_t tmpl = 0, units = 0;
        while (in >> tmpl >> units)
            expected[static_cast<std::uint16_t>(tmpl)] = units;
        const auto chars = listChars(bot);
        const auto it = std::find_if(chars.begin(), chars.end(), [&](const vn::CharSummary& s) { return s.name == name; });
        if (it == chars.end())
            fail("character '" + name + "' not listed");
        Self self;
        const auto e = enterWorld(bot, it->charId, zoneTerrainOf(c, static_cast<std::uint16_t>(zone)), self,
                                  static_cast<std::uint16_t>(zone));
        expectTestVitals(e);
        if (v.coins.front().coins != coins)
            fail(std::format("entered with {} Akçe (expected {})", v.coins.front().coins, coins));
        if (v.skillBars.front().slots != bar)
            fail("the skill bar with its item slots was not stored");
        game::Inventory inv;
        (void)applyItemEntries(inv, v.inventory->items);
        std::map<std::uint16_t, std::uint32_t> totals;
        for (const auto& i : inv.items())
            totals[i.templateId] += i.quantity;
        if (totals != expected)
            fail("the inventory differs from the one stored after shopping");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("'{}' entered with {} Akçe, {} template(s) and the item bar as stored", name, coins,
                           expected.size());
    }

    // ---- A new mage in its homeland; the merchants are there ----
    std::string name;
    const std::uint32_t charId = createCharacterOf(bot, c.opts.race, kShopMage, name);
    const std::uint16_t zone = game::zones().homeZone(static_cast<std::uint8_t>(c.opts.race));
    const auto& t = zoneTerrainOf(c, zone);
    const TownMerchants town = merchantsOf(zone);
    Self self;
    auto enter = enterWorld(bot, charId, t, self, zone);
    expectTestVitals(enter);
    const std::uint32_t me = enter.entityId;
    const std::uint32_t potionNpc = expectNpc(bot, *town.potion);
    const std::uint32_t smithNpc = expectNpc(bot, *town.smith);
    game::Inventory inv;
    (void)applyItemEntries(inv, v.inventory->items);
    std::size_t updatesSeen = v.itemUpdates.size();
    std::uint32_t coins = v.coins.front().coins;
    if (coins != 0)
        fail(std::format("a new character entered with {} Akçe", coins));
    const auto staffSlot = bagSlotOf(inv, kFrostStaffItem);
    if (!staffSlot)
        fail("the mage's starter kit has no Kırağı Asası in the bag");
    const Point potionAt{town.potion->x, town.potion->z};
    const Point smithAt{town.smith->x, town.smith->z};

    // ---- Refusals before talking (§2.2) ----
    if (distance(self.p(), potionAt) <= game::kNpcTalkRange + game::kRangeTolerance)
        fail("the spawn is within talk range of the potion seller (the data changed?)");
    expectShop(openShop(bot, potionNpc).result, vn::ShopResult::OutOfRange, "opening from the spawn");
    expectShop(buyItem(bot, potionNpc, kHpSmall, 1).result, vn::ShopResult::NotOpen, "buying without an open shop");
    expectShop(sellItem(bot, potionNpc, *staffSlot, 1).result, vn::ShopResult::NotOpen, "selling without an open shop");
    expectShop(openShop(bot, 0x7FFFFFF0u).result, vn::ShopResult::NotFound, "opening an unknown entity");
    expectShop(openShop(bot, me).result, vn::ShopResult::NotFound, "opening oneself");
    const std::size_t failsBefore = v.actionFails.size();
    bot.send(vn::AttackReq{smithNpc});
    if (!bot.pumpUntil([&] { return v.actionFails.size() > failsBefore; }, Ms(2000)) ||
        v.actionFails.back().reason != vn::ActionFailReason::NotAttackable)
        fail("attacking a merchant was not refused with NotAttackable");

    // ---- The smith: sell the spare staff, refusals ----
    walkNear(bot, t, self, smithAt, 2.5f);
    const auto smithShop = openShop(bot, smithNpc);
    expectShop(smithShop.result, vn::ShopResult::Ok, "opening the smith's shop");
    if (smithShop.entries.size() != town.smith->stock.size())
        fail(std::format("the smith lists {} entries (npcs.toml has {})", smithShop.entries.size(), town.smith->stock.size()));
    std::uint32_t dearest = 0;
    std::uint16_t dearestItem = 0;
    for (std::size_t i = 0; i < smithShop.entries.size(); ++i)
    {
        const auto& e = smithShop.entries[i];
        const game::ItemTemplate* it = items.find(e.templateId);
        if (e.templateId != town.smith->stock[i] || !it || e.price != it->value)
            fail(std::format("smith entry {}: item {} for {} Akçe (expected item {} for its value)", i, e.templateId,
                             e.price, town.smith->stock[i]));
        if (e.price > dearest)
        {
            dearest = e.price;
            dearestItem = e.templateId;
        }
    }
    expectShop(buyItem(bot, smithNpc, kEmberStaffItem, 1).result, vn::ShopResult::NotEnoughCoins, "buying with 0 Akçe");
    expectShop(buyItem(bot, smithNpc, kEmberStaffItem, 2).result, vn::ShopResult::BadQuantity, "buying 2 staves");
    expectShop(buyItem(bot, smithNpc, kEmberStaffItem, 0).result, vn::ShopResult::BadQuantity, "buying 0");
    expectShop(buyItem(bot, smithNpc, kHpSmall, 1).result, vn::ShopResult::NotInStock, "buying a potion from the smith");
    expectShop(sellItem(bot, smithNpc, *staffSlot, 2).result, vn::ShopResult::BadQuantity, "selling 2 of 1");
    expectShop(sellItem(bot, smithNpc, *staffSlot, 0).result, vn::ShopResult::BadQuantity, "selling 0");
    expectShop(sellItem(bot, smithNpc, freeBagSlotAbove(inv, 1), 1).result, vn::ShopResult::NoItem, "selling an empty slot");
    expectShop(sellItem(bot, smithNpc, 28, 1).result, vn::ShopResult::NoItem, "selling slot 28");
    if (v.itemUpdates.size() != updatesSeen || v.coins.size() != 1)
        fail("a refused request changed the bag or the balance");
    const std::uint32_t staffPrice = game::sellPrice(*items.find(kFrostStaffItem));
    const auto sold = sellItem(bot, smithNpc, *staffSlot, 1);
    expectShop(sold.result, vn::ShopResult::Ok, "selling the Kırağı Asası");
    if (sold.templateId != kFrostStaffItem || sold.gained != staffPrice)
        fail(std::format("SHOP_SELL_RES item {} +{} Akçe (expected item {} +{})", sold.templateId, sold.gained,
                         kFrostStaffItem, staffPrice));
    updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
    if (!inv.at(game::ItemPlace::bag(*staffSlot)).empty())
        fail("the sold staff is still in the bag");
    if (v.coins.size() != 2 || v.coins.back().coins != staffPrice || v.coins.back().gained != staffPrice ||
        v.lastCoinsIndex > bot.lastIndex() || v.lastItemUpdateIndex > bot.lastIndex())
        fail("no COINS(+gained) / ITEM_UPDATE before SHOP_SELL_RES(Ok)");
    coins = staffPrice;
    if (dearest > coins)
        expectShop(buyItem(bot, smithNpc, dearestItem, 1).result, vn::ShopResult::NotEnoughCoins,
                   "buying the dearest weapon");

    // ---- Walking away closes the smith's shop; the potion seller ----
    walkNear(bot, t, self, potionAt, 2.5f);
    if (distance(self.p(), smithAt) > game::kShopViewDistance + 0.5f)
    {
        if (!bot.pumpUntil([&] { return !v.shopClosed.empty(); }, Ms(2000)) || v.shopClosed.back().npcId != smithNpc ||
            v.shopClosed.back().reason != vn::ShopClosedReason::OutOfRange)
            fail("no SHOP_CLOSED(OutOfRange) after walking away from the smith");
    }
    else
        bot.send(vn::ShopCloseReq{smithNpc}); // the towns where they stand close together
    expectShop(buyItem(bot, smithNpc, kEmberStaffItem, 1).result, vn::ShopResult::NotOpen, "buying from a closed shop");
    const auto potionShop = openShop(bot, potionNpc);
    expectShop(potionShop.result, vn::ShopResult::Ok, "opening the potion shop");
    for (std::size_t i = 0; i < potionShop.entries.size(); ++i)
        if (potionShop.entries[i].templateId != town.potion->stock[i] ||
            potionShop.entries[i].price != items.find(town.potion->stock[i])->value)
            fail(std::format("potion entry {} differs from npcs.toml / items.toml", i));
    const std::uint32_t hpPrice = items.find(kHpSmall)->value, mpPrice = items.find(kMpSmall)->value;
    expectShop(buyItem(bot, potionNpc, kHpSmall, 21).result, vn::ShopResult::BadQuantity, "buying 21 (max_stack 20)");
    expectShop(buyItem(bot, potionNpc, kHpMedium, 2).result, vn::ShopResult::NotEnoughCoins, "buying 2 Közlü Şerbet");
    const auto buyOne = [&](std::uint16_t tmpl, std::uint32_t price) {
        const auto r = buyItem(bot, potionNpc, tmpl, 1);
        expectShop(r.result, vn::ShopResult::Ok, std::format("buying item {}", tmpl));
        if (r.cost != price || v.coins.back().coins != coins - price || v.coins.back().gained != 0 ||
            v.lastCoinsIndex > bot.lastIndex() || v.itemUpdates.size() == updatesSeen)
            fail(std::format("SHOP_BUY_RES cost {} / COINS {} (+{}) after buying item {} for {} with {}", r.cost,
                             v.coins.back().coins, v.coins.back().gained, tmpl, price, coins));
        coins -= price;
        updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
    };
    buyOne(kHpSmall, hpPrice);
    const auto hpSlot = bagSlotOf(inv, kHpSmall);
    buyOne(kHpSmall, hpPrice); // grows the same stack
    if (!hpSlot || bagSlotOf(inv, kHpSmall) != hpSlot || inv.at(game::ItemPlace::bag(*hpSlot)).quantity != 2)
        fail("the second Kızılca Şerbet did not join the first stack");
    buyOne(kMpSmall, mpPrice);
    if (coins >= hpPrice)
        fail("the scenario expects too little Akçe left for another potion");
    expectShop(buyItem(bot, potionNpc, kHpSmall, 1).result, vn::ShopResult::NotEnoughCoins, "buying with too little Akçe");

    // ---- The bar: potions next to the skill; invalid item slots refused ----
    const vn::SkillBarSlots bar{kEmberBoltSkill, game::barItemSlot(kHpSmall), game::barItemSlot(kMpSmall), 0, 0, 0, 0, 0};
    for (const vn::SkillBarSlots& bad :
         {vn::SkillBarSlots{kEmberBoltSkill, game::barItemSlot(kAshDustItem), 0, 0, 0, 0, 0, 0},
          vn::SkillBarSlots{kEmberBoltSkill, game::barItemSlot(999), 0, 0, 0, 0, 0, 0},
          vn::SkillBarSlots{kEmberBoltSkill, game::kBarItemFlag, 0, 0, 0, 0, 0, 0}})
    {
        const auto r = setBar(bot, bad);
        if (r.result != vn::SkillBarResult::NotLearned || r.slots[1] != 0)
            fail(std::format("a bar with item slot 0x{:04X} was not refused", bad[1]));
    }
    const auto barRes = setBar(bot, bar);
    if (barRes.result != vn::SkillBarResult::Ok || barRes.slots != bar)
        fail("the bar with two potions was not stored");

    // ---- Relog: stock, Akçe and bar kept; vitals at 10 % again ----
    leaveWorld(bot);
    Self again;
    enter = enterWorld(bot, charId, t, again, zone);
    expectTestVitals(enter);
    if (distance(again.p(), self.p()) > 0.05f)
        fail("not back where it left");
    self = again;
    const std::uint32_t me2 = enter.entityId;
    (void)expectNpc(bot, *town.potion);
    inv = {};
    (void)applyItemEntries(inv, v.inventory->items);
    updatesSeen = v.itemUpdates.size();
    if (v.coins.front().coins != coins || v.skillBars.front().slots != bar)
        fail(std::format("after the relog: {} Akçe (expected {}), bar {}", v.coins.front().coins, coins,
                         v.skillBars.front().slots == bar ? "kept" : "changed"));
    if (!bagSlotOf(inv, kHpSmall) || inv.at(game::ItemPlace::bag(*bagSlotOf(inv, kHpSmall))).quantity != 2 ||
        !bagSlotOf(inv, kMpSmall) || bagSlotOf(inv, kFrostStaffItem))
        fail("after the relog the bag does not hold what was bought and sold");
    if (!v.itemCooldowns.empty())
        fail("ITEM_COOLDOWN on entry without a potion used");

    // ---- Potions (§2.3) ----
    expectUse(useItem(bot, kHpMedium, vn::kItemUseAnyStack).result, vn::ItemUseResult::NoItem, "a potion not owned");
    expectUse(useItem(bot, kAshDustItem, vn::kItemUseAnyStack).result, vn::ItemUseResult::NotUsable, "a material");
    expectUse(useItem(bot, kEmberStaffItem, vn::kItemUseAnyStack).result, vn::ItemUseResult::NotUsable, "a staff");
    expectUse(useItem(bot, kHpSmall, freeBagSlotAbove(inv, 1)).result, vn::ItemUseResult::NoItem, "an empty slot");
    if (!v.itemCooldowns.empty())
        fail("a refused use started the cooldown");
    const std::uint8_t hpSlot2 = *bagSlotOf(inv, kHpSmall);
    const std::size_t usedBefore = v.itemsUsed.size();
    const auto drank = useItem(bot, kHpSmall, hpSlot2);
    expectUse(drank.result, vn::ItemUseResult::Ok, "the Kızılca Şerbet");
    if (v.itemCooldowns.size() != 1 || v.itemCooldowns.back().totalMs != game::kConsumableCooldownMs ||
        v.itemCooldowns.back().remainingMs != game::kConsumableCooldownMs || v.lastItemCooldownIndex > bot.lastIndex())
        fail("no ITEM_COOLDOWN(10000, 10000) before ITEM_USE_RES(Ok)");
    updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
    if (inv.at(game::ItemPlace::bag(hpSlot2)).quantity != 1 || v.lastItemUpdateIndex > bot.lastIndex())
        fail("the stack did not shrink to 1 before ITEM_USE_RES(Ok)");
    if (!bot.pumpUntil([&] { return v.itemsUsed.size() > usedBefore; }, Ms(2000)))
        fail("no ITEM_USED after the use");
    const vn::ItemUsed used = v.itemsUsed.back();
    const std::uint32_t restoreHp = items.find(kHpSmall)->restoreHp;
    if (used.entityId != me2 || used.templateId != kHpSmall || used.hpRestored != restoreHp || used.mpRestored != 0)
        fail(std::format("ITEM_USED entity {} item {} +{} hp +{} mp (expected {} {} +{} hp)", used.entityId, used.templateId,
                         used.hpRestored, used.mpRestored, me2, kHpSmall, restoreHp));
    if (!bot.pumpUntil([&] { return !v.selfStats.empty() && v.selfStats.back().hp >= enter.hp + restoreHp; }, Ms(2000)))
        fail("SELF_STATS did not show the restored hp");
    expectUse(useItem(bot, kMpSmall, vn::kItemUseAnyStack).result, vn::ItemUseResult::OnCooldown, "during the cooldown");
    // The cooldown is shared: wait it out, then the mana potion from the bar (template, any stack).
    waitPumping(bot, Ms(game::kConsumableCooldownMs + 400), [&] { bot.keepAlive(); });
    const std::uint32_t mpBefore = v.selfStats.empty() ? enter.mp : v.selfStats.back().mp;
    const std::size_t usedBefore2 = v.itemsUsed.size();
    expectUse(useItem(bot, game::barItemTemplate(bar[2]), vn::kItemUseAnyStack).result, vn::ItemUseResult::Ok,
              "the Şafak Çiyi from the bar");
    updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
    if (bagSlotOf(inv, kMpSmall))
        fail("the last Şafak Çiyi is still in the bag");
    if (!bot.pumpUntil([&] { return v.itemsUsed.size() > usedBefore2; }, Ms(2000)))
        fail("no ITEM_USED after the mana potion");
    const vn::ItemUsed usedMp = v.itemsUsed.back();
    if (usedMp.templateId != kMpSmall || usedMp.hpRestored != 0 || usedMp.mpRestored == 0 ||
        usedMp.mpRestored > items.find(kMpSmall)->restoreMp || usedMp.mpRestored + mpBefore > enter.maxMp + 5)
        fail(std::format("ITEM_USED of the mana potion: +{} hp +{} mp (mp was {}/{})", usedMp.hpRestored,
                         usedMp.mpRestored, mpBefore, enter.maxMp));
    expectUse(useItem(bot, kMpSmall, vn::kItemUseAnyStack).result, vn::ItemUseResult::NoItem, "the potion just used up");

    // ---- Relog during the cooldown: it continues (ITEM_COOLDOWN after CHAR_STATS), bar and bag kept ----
    leaveWorld(bot);
    enter = enterWorld(bot, charId, t, again, zone);
    if (!bot.pumpUntil([&] { return !v.itemCooldowns.empty(); }, Ms(2000)))
        fail("no ITEM_COOLDOWN on entry while the cooldown runs");
    if (v.itemCooldowns.front().remainingMs == 0 || v.itemCooldowns.front().remainingMs >= game::kConsumableCooldownMs ||
        v.lastItemCooldownIndex < v.firstCharStatsIndex)
        fail(std::format("ITEM_COOLDOWN on entry: {} ms left, not after CHAR_STATS", v.itemCooldowns.front().remainingMs));
    if (v.skillBars.front().slots != bar || v.coins.front().coins != coins)
        fail("bar or balance changed by using potions");
    inv = {};
    (void)applyItemEntries(inv, v.inventory->items);
    std::map<std::uint16_t, std::uint32_t> totals;
    for (const auto& i : inv.items())
        totals[i.templateId] += i.quantity;
    if (totals[kHpSmall] != 1 || totals.contains(kMpSmall) || totals.contains(kFrostStaffItem))
        fail("after the second relog the bag does not hold one Kızılca Şerbet only");
    expectNoViewErrors(bot, "shop view");
    if (!c.opts.stateFile.empty())
    {
        std::ofstream out(c.opts.stateFile, std::ios::trunc);
        out << name << "\n" << zone << "\n" << coins << "\n";
        for (const auto s : bar)
            out << s << " ";
        out << "\n";
        for (const auto& [tmpl, units] : totals)
            out << tmpl << " " << units << "\n";
        if (!out)
            fail("cannot write --state-file");
    }

    // ---- Flood: more than 10 shop requests in a second close the connection (§6) ----
    for (int i = 0; i < 12 && !bot.closed(); ++i)
        bot.send(vn::ShopCloseReq{potionNpc});
    for (;;)
    {
        const Received m = bot.next("waiting for ERROR(Flood)");
        if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::Pong))
            continue;
        vn::Error err;
        if (m.opcode != static_cast<std::uint16_t>(vn::Opcode::Error) || !vn::decodeMessage(m.payload, err) ||
            err.errorCode != vn::ErrorCode::Flood)
            fail(std::format("the shop flood got {} (expected ERROR(Flood))", vn::opcodeName(static_cast<vn::Opcode>(m.opcode))));
        break;
    }
    bot.expectClosed(true, bot.timeout());
    return std::format("'{}' in zone {}: refusals ok, sold the Kırağı Asası for {} Akçe, bought 2 Kızılca Şerbet and a "
                       "Şafak Çiyi ({} Akçe left), potions on the bar, drank both (cooldown, ITEM_USED +{} hp / +{} mp), "
                       "relogged twice (state and cooldown kept), flood closed",
                       name, zone, staffPrice, coins, used.hpRestored, usedMp.mpRestored);
}

// ---- shoperrors: the shop's error answers through the real protocol (docs/PROTOCOL_SHOP.md §2) ----
//
// Default phase (server: start_zone_override = 1, world.testing_fail_first_item_write = 16): a new
// Aydor mage in Ortaocak sells its spare staff, gets ServerError for its first buy, first sell and first
// use of a Kızılca Şerbet (the testing hook fails those writes; nothing changes), AlreadyFull at full hp,
// OutOfRange for buy and sell 5.5-7.5 m from the seller (the shop is still open), dies to the Kor Bekçisi:
// YouAreDead for a use and for opening the shop (NotFound if the seller is out of view by then),
// respawns at 50 % hp and drinks after the failed first use's cooldown.
// --phase prepare / rich (with -Database; vo_admin set-coins <name> 999999990 in between): PurseFull for
// selling the staff, buying swords until BagFull, then the sale fits.

namespace {

constexpr std::uint16_t kSwordItem = 1; // Er Kılıcı, in every smith's stock

// The elite of zone 1 kills the bot (it attacks it on the way); fails after 90 s.
void dieToElite(Bot& bot, const Context& c, const assets::TerrainData& t, Self& self, std::uint32_t me)
{
    const game::MonsterTemplate* elite = nullptr;
    const game::MonsterSpawn* eliteSpawn = nullptr;
    for (const auto& s : c.monsters->spawns())
        if (const auto* m = c.monsters->find(s.monster); m && m->elite && s.zone == 1 && m->name == "Kor Bekçisi")
        {
            elite = m;
            eliteSpawn = &s;
        }
    if (!elite)
        fail("no elite monster in zone 1");
    const auto eliteEntity = [&]() -> std::uint32_t {
        for (const auto& [id, r] : bot.view().entities)
            if (r.spawn.templateId == elite->id && r.spawn.entityType == vn::kEntityTypeMonster &&
                (r.spawn.state & vn::kEntityStateDead) == 0)
                return id;
        return 0;
    };
    const auto towards = [&]() -> std::optional<Point> {
        if (const std::uint32_t id = eliteEntity())
            return positionOf(bot, id);
        return Point{eliteSpawn->x, eliteSpawn->z};
    };
    std::uint32_t attacked = 0;
    const auto until = Clock::now() + Ms(90000);
    while (!isDeadOf(bot, me) && Clock::now() < until)
    {
        (void)approach(bot, t, self, me, towards, 1.8f, Ms(20000), [&] {
            if (const std::uint32_t id = eliteEntity(); id && id != attacked)
            {
                bot.send(vn::AttackReq{id});
                attacked = id;
            }
        });
        waitPumping(bot, Ms(300), nullptr);
        expectNoZoneChange(bot, self, "walking to the Kor Bekçisi");
    }
    if (!isDeadOf(bot, me))
        fail("the mage did not die to the Kor Bekçisi within 90 s");
}

} // namespace

std::string scenarioShopErrors(const Context& c)
{
    if (!c.items || !c.monsters)
        fail("gamedata/items.toml and gamedata/monsters.toml are needed");
    if (game::npcs().empty())
        fail("gamedata/npcs.toml not found or invalid");
    const game::ItemCatalog& items = *c.items;
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    auto& v = bot.view();
    constexpr std::uint16_t kZone = 1;
    const auto& t = zoneTerrainOf(c, kZone);
    const TownMerchants town = merchantsOf(kZone);
    const Point potionAt{town.potion->x, town.potion->z};
    const Point smithAt{town.smith->x, town.smith->z};
    const std::uint32_t staffPrice = game::sellPrice(*items.find(kFrostStaffItem));

    if (c.opts.phase == "prepare")
    {
        std::string name;
        (void)createCharacterOf(bot, c.opts.race, kShopMage, name);
        std::ofstream out(c.opts.stateFile, std::ios::trunc);
        out << name << "\n";
        if (!out)
            fail("cannot write --state-file");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("created the mage '{}'", name);
    }
    if (c.opts.phase == "rich")
    {
        // After vo_admin set-coins <name> 999999990: PurseFull, then BagFull.
        constexpr std::uint32_t kRich = 999'999'990;
        std::ifstream in(c.opts.stateFile);
        std::string name;
        if (!(in >> name))
            fail("cannot read the name from --state-file");
        std::uint32_t charId = 0;
        for (const auto& s : listChars(bot))
            if (s.name == name)
                charId = s.charId;
        if (charId == 0)
            fail("character '" + name + "' not listed");
        Self self;
        (void)enterWorld(bot, charId, t, self, kZone);
        if (v.coins.front().coins != kRich)
            fail(std::format("entered with {} Akçe (expected {} after vo_admin set-coins)", v.coins.front().coins, kRich));
        game::Inventory inv;
        (void)applyItemEntries(inv, v.inventory->items);
        std::size_t updatesSeen = v.itemUpdates.size();
        const auto staffSlot = bagSlotOf(inv, kFrostStaffItem);
        if (!staffSlot)
            fail("the mage's spare staff is not in the bag");
        const std::uint32_t smithNpc = expectNpc(bot, *town.smith);
        walkNear(bot, t, self, smithAt, 2.5f);
        expectNoZoneChange(bot, self, "walking to the smith");
        expectShop(openShop(bot, smithNpc).result, vn::ShopResult::Ok, "opening the smith's shop");
        // PurseFull: 999 999 990 + 30 would pass 1 000 000 000; nothing is sold.
        expectShop(sellItem(bot, smithNpc, *staffSlot, 1).result, vn::ShopResult::PurseFull, "selling with a full purse");
        if (v.itemUpdates.size() != updatesSeen || v.coins.size() != 1)
            fail("PurseFull changed the bag or the balance");
        // BagFull: swords into every free slot, then one more.
        std::size_t free = 0;
        for (std::uint8_t s = 0; s < game::kBagSlots; ++s)
            free += inv.at(game::ItemPlace::bag(s)).empty() ? 1 : 0;
        const std::uint32_t swordPrice = items.find(kSwordItem)->value;
        std::uint32_t coins = kRich;
        for (std::size_t i = 0; i < free; ++i)
        {
            const auto r = buyItem(bot, smithNpc, kSwordItem, 1);
            expectShop(r.result, vn::ShopResult::Ok, std::format("buying sword {} of {}", i + 1, free));
            coins -= swordPrice;
        }
        updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
        for (std::uint8_t s = 0; s < game::kBagSlots; ++s)
            if (inv.at(game::ItemPlace::bag(s)).empty())
                fail(std::format("bag slot {} still empty after {} swords", s, free));
        const std::size_t coinsBefore = v.coins.size();
        expectShop(buyItem(bot, smithNpc, kSwordItem, 1).result, vn::ShopResult::BagFull, "buying into a full bag");
        if (v.itemUpdates.size() != updatesSeen || v.coins.size() != coinsBefore || v.coins.back().coins != coins)
            fail(std::format("BagFull changed the bag or the balance ({} Akçe, expected {})", v.coins.back().coins, coins));
        // Now the sale fits under the cap.
        const auto sold = sellItem(bot, smithNpc, *staffSlot, 1);
        expectShop(sold.result, vn::ShopResult::Ok, "selling the staff below the cap");
        if (v.coins.back().coins != coins + staffPrice)
            fail(std::format("after the sale {} Akçe (expected {})", v.coins.back().coins, coins + staffPrice));
        expectNoViewErrors(bot, "shoperrors view");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("'{}' with {} Akçe: PurseFull for a {} Akçe sale, {} swords filled the bag, then BagFull; the "
                           "sale fits afterwards ({} Akçe)",
                           name, kRich, staffPrice, free, coins + staffPrice);
    }

    // ---- A new mage in Ortaocak at full hp / mp ----
    std::string name;
    const std::uint32_t charId = createCharacterOf(bot, c.opts.race, kShopMage, name);
    Self self;
    const auto enter = enterWorld(bot, charId, t, self, kZone);
    if (enter.hp != enter.maxHp)
        fail("run the server without world.testing_enter_vitals_percent (the mage must enter at full hp)");
    const std::uint32_t me = enter.entityId;
    const std::uint32_t potionNpc = expectNpc(bot, *town.potion);
    const std::uint32_t smithNpc = expectNpc(bot, *town.smith);
    game::Inventory inv;
    (void)applyItemEntries(inv, v.inventory->items);
    std::size_t updatesSeen = v.itemUpdates.size();
    const auto staffSlot = bagSlotOf(inv, kFrostStaffItem);
    if (!staffSlot)
        fail("the mage's starter kit has no Kırağı Asası in the bag");

    // Akçe: the spare staff to the smith.
    walkNear(bot, t, self, smithAt, 2.5f);
    expectNoZoneChange(bot, self, "walking to the smith");
    expectShop(openShop(bot, smithNpc).result, vn::ShopResult::Ok, "opening the smith's shop");
    expectShop(sellItem(bot, smithNpc, *staffSlot, 1).result, vn::ShopResult::Ok, "selling the spare staff");
    updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
    std::uint32_t coins = staffPrice;
    if (v.coins.back().coins != coins)
        fail(std::format("{} Akçe after selling the staff (expected {})", v.coins.back().coins, coins));

    // ServerError: the first buy of a Kızılca Şerbet (world.testing_fail_first_item_write = 16).
    walkNear(bot, t, self, potionAt, 2.5f);
    expectNoZoneChange(bot, self, "walking to the potion seller");
    expectShop(openShop(bot, potionNpc).result, vn::ShopResult::Ok, "opening the potion shop");
    const std::uint32_t hpPrice = items.find(kHpSmall)->value;
    std::size_t coinsSeen = v.coins.size();
    expectShop(buyItem(bot, potionNpc, kHpSmall, 1).result, vn::ShopResult::ServerError,
               "the first buy (run the server with world.testing_fail_first_item_write = 16)");
    if (v.itemUpdates.size() != updatesSeen || v.coins.size() != coinsSeen)
        fail("a buy answered ServerError changed the bag or the balance");
    for (int i = 0; i < 3; ++i)
    {
        expectShop(buyItem(bot, potionNpc, kHpSmall, 1).result, vn::ShopResult::Ok, std::format("buy {} after the failed one", i + 1));
        coins -= hpPrice;
    }
    updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
    const auto hpSlot = bagSlotOf(inv, kHpSmall);
    if (!hpSlot || inv.at(game::ItemPlace::bag(*hpSlot)).quantity != 3 || v.coins.back().coins != coins)
        fail("three Kızılca Şerbet were not bought after the failed buy");

    // AlreadyFull: an hp potion at full hp (refused by the world before any write; no cooldown).
    expectUse(useItem(bot, kHpSmall, vn::kItemUseAnyStack).result, vn::ItemUseResult::AlreadyFull, "drinking at full hp");
    if (!v.itemCooldowns.empty())
        fail("AlreadyFull started the potion cooldown");

    // ServerError: the first sell of a Kızılca Şerbet; then a sale that is stored.
    coinsSeen = v.coins.size();
    expectShop(sellItem(bot, potionNpc, *hpSlot, 1).result, vn::ShopResult::ServerError, "the first sale of a Kızılca Şerbet");
    if (v.itemUpdates.size() != updatesSeen || v.coins.size() != coinsSeen)
        fail("a sale answered ServerError changed the bag or the balance");
    expectShop(sellItem(bot, potionNpc, *hpSlot, 1).result, vn::ShopResult::Ok, "the second sale of a Kızılca Şerbet");
    coins += game::sellPrice(*items.find(kHpSmall));
    updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
    if (inv.at(game::ItemPlace::bag(*hpSlot)).quantity != 2 || v.coins.back().coins != coins)
        fail("the stored sale did not leave 2 potions and the expected balance");

    // OutOfRange for buy and sell: 5.5-7.5 m from the seller the shop stays open (it closes past 8 m).
    {
        const Nav nav(t);
        const auto path = nav.bfs(
            self.p(),
            [&](int i, int j) {
                const float d = distance(nav.centre(i, j), potionAt);
                return d >= 5.8f && d <= 7.2f;
            },
            200000);
        if (!path)
            fail("no spot 6-7 m from the potion seller");
        const std::size_t closedBefore = v.shopClosed.size();
        if (walkPath(bot, t, self, *path, false) != 0)
            fail("corrections while stepping away from the seller");
        if (v.shopClosed.size() != closedBefore)
            fail(std::format("the shop closed {:.1f} m from the seller (closes past {} m)", distance(self.p(), potionAt),
                             game::kShopViewDistance));
        expectShop(buyItem(bot, potionNpc, kHpSmall, 1).result, vn::ShopResult::OutOfRange,
                   std::format("buying {:.1f} m from the seller", distance(self.p(), potionAt)));
        expectShop(sellItem(bot, potionNpc, *hpSlot, 1).result, vn::ShopResult::OutOfRange,
                   std::format("selling {:.1f} m from the seller", distance(self.p(), potionAt)));
    }

    // Dead: YouAreDead for a use and for opening the shop.
    dieToElite(bot, c, t, self, me);
    expectUse(useItem(bot, kHpSmall, vn::kItemUseAnyStack).result, vn::ItemUseResult::YouAreDead, "drinking while dead");
    const bool sellerInView = v.entities.contains(potionNpc);
    const vn::ShopResult deadOpen = openShop(bot, potionNpc).result;
    expectShop(deadOpen, sellerInView ? vn::ShopResult::YouAreDead : vn::ShopResult::NotFound,
               std::format("opening the shop while dead ({} m away, seller {} view)",
                           static_cast<int>(distance(self.p(), potionAt)), sellerInView ? "in" : "out of"));
    respawn(bot, self, me);

    // ServerError: the first use (50 % hp after the respawn); nothing consumed, the cooldown started.
    const std::size_t cooldownsBefore = v.itemCooldowns.size();
    updatesSeen = v.itemUpdates.size();
    expectUse(useItem(bot, kHpSmall, vn::kItemUseAnyStack).result, vn::ItemUseResult::ServerError, "the first use");
    if (v.itemUpdates.size() != updatesSeen || !v.itemsUsed.empty())
        fail("a use answered ServerError consumed or restored something");
    if (v.itemCooldowns.size() != cooldownsBefore + 1)
        fail("the use answered ServerError did not start the cooldown (PROTOCOL_SHOP.md: it stays after a failed write)");
    waitPumping(bot, Ms(game::kConsumableCooldownMs + 400), [&] { bot.keepAlive(); });
    expectUse(useItem(bot, kHpSmall, vn::kItemUseAnyStack).result, vn::ItemUseResult::Ok, "drinking after the cooldown");
    if (!bot.pumpUntil([&] { return !v.itemsUsed.empty(); }, Ms(2000)))
        fail("no ITEM_USED after the stored use");
    expectNoViewErrors(bot, "shoperrors view");
    expectNoZoneChange(bot, self, "shoperrors");
    bot.send(vn::Logout{});
    bot.expectClosed(true, bot.timeout());
    return std::format("'{}': ServerError for the first buy, sale and use (nothing changed; the use's cooldown ran), "
                       "AlreadyFull at full hp, OutOfRange for buy and sell inside the open shop, YouAreDead for a use, "
                       "{} for opening while dead, drank after respawning ({} Akçe left)",
                       name, vn::shopResultName(deadOpen), coins);
}

// ---- upgrade: the smith's Yükselt through the real protocol (docs/PROTOCOL_UPGRADE.md) ----
//
// Server: start_zone_override = 1, world.testing_enter_coins = 5000 (every character enters with 5000 Akçe)
// and world.testing_upgrade_outcome per phase. Default phase ("success"): a new Aydor warrior in Ortaocak is
// refused NotOpen (no shop), NotSmith (the potion seller), NoStone, WrongStone (a Saf stone for a tier-1 axe),
// NotUpgradable (the stone itself), NoItem, Changed; buys Ham stones, takes the axe +0 -> +10 (ITEM_UPDATE,
// COINS, UPGRADE_RES in order, the fee of every level), MaxLevel at +10; the sword unequipped to +5; spends
// the rest on stones: NotEnoughCoins for +6; equips the +10 axe, relogs (levels kept, the attack counts
// them), floods (ERROR(Flood)). --phase keep ("fail"): +1..+3 succeed, +4 fails twice and the axe stays
// +3 (the stones and fees are spent), kept after a relog. --phase destroy ("fail_from_6"): +1..+5, then +6
// destroys the axe (its slot empties), gone after a relog. --state-file (default phase): the name and the
// sword's bag slot; --phase check (after vo_admin set-upgrade <name> bag/<slot> 7): the sword enters at +7.

namespace {

constexpr std::uint8_t kUpgradeWarrior = 1;
constexpr std::uint16_t kHamStone = 166, kSafStone = 167, kUstaStone = 168, kAxeItem = 6, kKitSword = 1;
constexpr std::uint32_t kUpgradeTestCoins = 5000;

// UPGRADE_REQ is capped at 5 a second (PROTOCOL_UPGRADE.md §5): one every 270 ms here.
void paceUpgrade(Bot& bot)
{
    static Clock::time_point last{};
    const auto wait = last + Ms(270) - Clock::now();
    if (wait > Clock::duration::zero())
        bot.pumpFor(std::chrono::duration_cast<Ms>(wait) + Ms(1));
    last = Clock::now();
}

vn::UpgradeRes upgradeItem(Bot& bot, std::uint32_t npc, std::uint8_t slot, std::uint64_t itemId, std::uint8_t from,
                           std::uint8_t stone = vn::kUpgradeAnyStone)
{
    paceUpgrade(bot);
    bot.dropPongs();
    const std::uint32_t seq = bot.nextSeq();
    bot.send(vn::UpgradeReq{seq, npc, slot, itemId, from, stone});
    const auto res = bot.expect<vn::UpgradeRes>();
    if (res.seq != seq || res.bagSlot != slot)
        fail(std::format("UPGRADE_RES seq {} slot {} (asked seq {} slot {})", res.seq, res.bagSlot, seq, slot));
    if (!vn::isUpgradeAttempt(res.result) && (res.fee != 0 || res.stoneTemplateId != 0))
        fail(std::format("a refused UPGRADE_RES ({}) with a fee or a stone", vn::upgradeResultName(res.result)));
    return res;
}

void expectUpgrade(vn::UpgradeResult got, vn::UpgradeResult want, std::string_view what)
{
    if (got != want)
        fail(std::format("{}: {} (expected {})", what, vn::upgradeResultName(got), vn::upgradeResultName(want)));
}

// One attempt that must succeed / fail / destroy: the answer, the ITEM_UPDATE (the item and the stone),
// COINS and the stored effect on `inv` / `coins`.
void attempt(Bot& bot, const game::ItemCatalog& items, std::uint32_t npc, game::Inventory& inv, std::size_t& updatesSeen,
             std::uint32_t& coins, std::uint8_t slot, vn::UpgradeResult want)
{
    auto& v = bot.view();
    const game::ItemStack before = inv.at(game::ItemPlace::bag(slot));
    const game::ItemTemplate* t = items.find(before.templateId);
    if (!t)
        fail(std::format("no known item in bag slot {}", slot));
    const auto stoneSlot = bagSlotOf(inv, game::stoneTemplate(items, game::requiredStone(*t))->id);
    if (!stoneSlot)
        fail("no fitting stone in the bag");
    const std::uint16_t stonesBefore = inv.at(game::ItemPlace::bag(*stoneSlot)).quantity;
    const std::size_t coinsBefore = v.coins.size();
    const auto target = static_cast<std::uint8_t>(before.upgrade + 1);
    const auto res = upgradeItem(bot, npc, slot, before.id, before.upgrade);
    expectUpgrade(res.result, want, std::format("{} +{} -> +{}", t->name, before.upgrade, target));
    const std::uint32_t fee = game::upgradeFee(*t, target);
    if (res.fee != fee || res.stoneTemplateId != inv.at(game::ItemPlace::bag(*stoneSlot)).templateId || res.templateId != t->id)
        fail(std::format("UPGRADE_RES fee {} stone {} template {} (expected {} / {} / {})", res.fee, res.stoneTemplateId,
                         res.templateId, fee, inv.at(game::ItemPlace::bag(*stoneSlot)).templateId, t->id));
    // ITEM_UPDATE and COINS came before the answer (PROTOCOL_UPGRADE.md §4.3).
    if (v.itemUpdates.size() == updatesSeen || v.coins.size() != coinsBefore + 1)
        fail("no ITEM_UPDATE / COINS before UPGRADE_RES");
    updatesSeen = applyUpdatesSince(bot, inv, updatesSeen);
    coins -= fee;
    if (v.coins.back().coins != coins || v.coins.back().gained != 0)
        fail(std::format("COINS {} (+{}) after the attempt (expected {})", v.coins.back().coins, v.coins.back().gained, coins));
    const game::ItemStack& stone = inv.at(game::ItemPlace::bag(*stoneSlot));
    if ((stonesBefore == 1 && !stone.empty()) || (stonesBefore > 1 && stone.quantity != stonesBefore - 1))
        fail(std::format("the stone stack holds {} after the attempt (was {})", stone.quantity, stonesBefore));
    const game::ItemStack& after = inv.at(game::ItemPlace::bag(slot));
    switch (want)
    {
    case vn::UpgradeResult::Success:
        if (after.id != before.id || after.upgrade != target || res.level != target)
            fail(std::format("after a success the slot holds item {} +{} (UPGRADE_RES level {})", after.id, after.upgrade,
                             res.level));
        break;
    case vn::UpgradeResult::Failed:
        if (after != before || res.level != before.upgrade)
            fail(std::format("after a failure the item is +{} (expected it unchanged at +{})", after.upgrade, before.upgrade));
        break;
    default:
        if (!after.empty() || res.level != before.upgrade)
            fail("after a destruction the slot is not empty");
        break;
    }
}

std::uint8_t freeBagSlot(const game::Inventory& inv)
{
    for (std::uint8_t s = 0; s < game::kBagSlots; ++s)
        if (inv.at(game::ItemPlace::bag(s)).empty())
            return s;
    fail("no free bag slot");
}

// A new warrior entering Ortaocak with the testing Akçe, next to the smith with its shop open.
struct UpgradeSetup
{
    std::string name;
    std::uint32_t charId = 0;
    std::uint32_t me = 0;
    std::uint32_t smith = 0;
    std::uint32_t potion = 0;
    Self self;
    game::Inventory inv;
    std::size_t updatesSeen = 0;
    std::uint32_t coins = 0;
};

void enterForUpgrade(Bot& bot, const Context& c, UpgradeSetup& s, bool openSmith)
{
    auto& v = bot.view();
    const auto& t = zoneTerrainOf(c, 1);
    const TownMerchants town = merchantsOf(1);
    const auto enter = enterWorld(bot, s.charId, t, s.self, 1);
    s.me = enter.entityId;
    if (v.coins.front().coins < kUpgradeTestCoins)
        fail(std::format("entered with {} Akçe: run the server with world.testing_enter_coins = {}", v.coins.front().coins,
                         kUpgradeTestCoins));
    s.coins = v.coins.front().coins;
    s.inv = {};
    (void)applyItemEntries(s.inv, v.inventory->items);
    s.updatesSeen = v.itemUpdates.size();
    s.potion = expectNpc(bot, *town.potion);
    s.smith = expectNpc(bot, *town.smith);
    if (openSmith)
    {
        walkNear(bot, t, s.self, Point{town.smith->x, town.smith->z}, 2.5f);
        expectNoZoneChange(bot, s.self, "walking to the smith");
        expectShop(openShop(bot, s.smith).result, vn::ShopResult::Ok, "opening the smith's shop");
    }
}

void buyStones(Bot& bot, UpgradeSetup& s, const game::ItemCatalog& items, std::uint16_t stone, std::uint16_t n)
{
    const auto r = buyItem(bot, s.smith, stone, n);
    expectShop(r.result, vn::ShopResult::Ok, std::format("buying {} x {}", n, items.find(stone)->name));
    s.coins -= r.cost;
    s.updatesSeen = applyUpdatesSince(bot, s.inv, s.updatesSeen);
    if (bot.view().coins.back().coins != s.coins)
        fail("the balance after buying stones");
}

void relogUpgrade(Bot& bot, const Context& c, UpgradeSetup& s)
{
    leaveWorld(bot);
    enterForUpgrade(bot, c, s, false);
}

} // namespace

std::string scenarioUpgrade(const Context& c)
{
    if (!c.items)
        fail("gamedata/items.toml is needed");
    if (game::npcs().empty())
        fail("gamedata/npcs.toml not found or invalid");
    const game::ItemCatalog& items = *c.items;
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    auto& v = bot.view();
    using U = vn::UpgradeResult;
    UpgradeSetup s;

    if (c.opts.phase == "check")
    {
        // After vo_admin set-upgrade <name> bag/<slot> 7: the sword enters at +7.
        std::ifstream in(c.opts.stateFile);
        int slot = -1;
        if (!(in >> s.name >> slot) || slot < 0 || slot >= static_cast<int>(game::kBagSlots))
            fail("cannot read the name and the sword's slot from --state-file");
        for (const auto& ch : listChars(bot))
            if (ch.name == s.name)
                s.charId = ch.charId;
        if (s.charId == 0)
            fail("character '" + s.name + "' not listed");
        enterForUpgrade(bot, c, s, false);
        const game::ItemStack& sword = s.inv.at(game::ItemPlace::bag(static_cast<std::uint8_t>(slot)));
        if (sword.templateId != kKitSword || sword.upgrade != 7)
            fail(std::format("bag slot {} holds template {} +{} (expected the sword at +7 after vo_admin set-upgrade)", slot,
                             sword.templateId, sword.upgrade));
        expectNoViewErrors(bot, "upgrade check view");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return std::format("'{}': the sword in bag slot {} entered at +7 (vo_admin set-upgrade)", s.name, slot);
    }

    s.charId = createCharacterOf(bot, c.opts.race, kUpgradeWarrior, s.name);
    enterForUpgrade(bot, c, s, c.opts.phase == "keep" || c.opts.phase == "destroy");
    const auto axeSlot = bagSlotOf(s.inv, kAxeItem);
    if (!axeSlot)
        fail("the warrior's kit has no Ocak Baltası in the bag");
    const std::uint64_t axe = s.inv.at(game::ItemPlace::bag(*axeSlot)).id;

    if (c.opts.phase == "keep" || c.opts.phase == "destroy")
    {
        const bool keep = c.opts.phase == "keep";
        buyStones(bot, s, items, kHamStone, 10);
        const int last = keep ? 3 : 5;
        for (int i = 0; i < last; ++i)
            attempt(bot, items, s.smith, s.inv, s.updatesSeen, s.coins, *axeSlot, U::Success);
        if (keep)
        {
            attempt(bot, items, s.smith, s.inv, s.updatesSeen, s.coins, *axeSlot, U::Failed);
            attempt(bot, items, s.smith, s.inv, s.updatesSeen, s.coins, *axeSlot, U::Failed);
        }
        else
            attempt(bot, items, s.smith, s.inv, s.updatesSeen, s.coins, *axeSlot, U::Destroyed);
        const std::uint32_t coins = s.coins;
        relogUpgrade(bot, c, s);
        const game::ItemStack& now = s.inv.at(game::ItemPlace::bag(*axeSlot));
        if (keep && (now.id != axe || now.upgrade != 3))
            fail(std::format("after the relog the axe is item {} +{} (expected {} +3)", now.id, now.upgrade, axe));
        if (!keep && !now.empty())
            fail("after the relog the destroyed axe is back");
        // (The testing hook tops the balance up to 5000 again on every entry.)
        if (s.coins != std::max(coins, kUpgradeTestCoins))
            fail(std::format("after the relog {} Akçe (expected {})", s.coins, std::max(coins, kUpgradeTestCoins)));
        expectNoViewErrors(bot, "upgrade view");
        bot.send(vn::Logout{});
        bot.expectClosed(true, bot.timeout());
        return keep ? std::format("'{}': the axe to +3, two failed +4 attempts kept it at +3 (stones and fees spent), "
                                  "kept after a relog ({} Akçe)",
                                  s.name, coins)
                    : std::format("'{}': the axe to +5, the +6 attempt destroyed it (gone after a relog, {} Akçe)", s.name,
                                  coins);
    }

    // ---- The default phase (world.testing_upgrade_outcome = "success") ----
    const TownMerchants town = merchantsOf(1);
    const auto& t = zoneTerrainOf(c, 1);
    // World refusals: no shop open; a merchant that does not upgrade.
    expectUpgrade(upgradeItem(bot, s.smith, *axeSlot, axe, 0).result, U::NotOpen, "no shop open");
    walkNear(bot, t, s.self, Point{town.potion->x, town.potion->z}, 2.5f);
    expectShop(openShop(bot, s.potion).result, vn::ShopResult::Ok, "opening the potion shop");
    expectUpgrade(upgradeItem(bot, s.potion, *axeSlot, axe, 0).result, U::NotSmith, "at the potion seller");
    walkNear(bot, t, s.self, Point{town.smith->x, town.smith->z}, 2.5f);
    expectNoZoneChange(bot, s.self, "walking to the smith");
    expectShop(openShop(bot, s.smith).result, vn::ShopResult::Ok, "opening the smith's shop");
    // Session refusals against the committed bag.
    expectUpgrade(upgradeItem(bot, s.smith, *axeSlot, axe, 0).result, U::NoStone, "no stone yet");
    buyStones(bot, s, items, kSafStone, 1);
    const auto safSlot = bagSlotOf(s.inv, kSafStone);
    expectUpgrade(upgradeItem(bot, s.smith, *axeSlot, axe, 0, *safSlot).result, U::WrongStone, "a Saf stone for a tier-1 axe");
    const auto stoneAsItem = upgradeItem(bot, s.smith, *safSlot, s.inv.at(game::ItemPlace::bag(*safSlot)).id, 0);
    expectUpgrade(stoneAsItem.result, U::NotUpgradable, "the stone itself");
    if (stoneAsItem.templateId != kSafStone)
        fail("NotUpgradable did not name the stone's template");
    expectUpgrade(upgradeItem(bot, s.smith, 27, 0, 0).result, U::NoItem, "an empty bag slot");
    expectUpgrade(upgradeItem(bot, s.smith, *axeSlot, axe + 99999, 0).result, U::NoItem, "another item id");
    expectUpgrade(upgradeItem(bot, s.smith, *axeSlot, axe, 2).result, U::Changed, "a stale level");
    if (v.coins.back().coins != s.coins)
        fail("a refusal changed the balance");

    // The axe +0 -> +10, then MaxLevel.
    buyStones(bot, s, items, kHamStone, 20);
    for (int i = 0; i < 10; ++i)
        attempt(bot, items, s.smith, s.inv, s.updatesSeen, s.coins, *axeSlot, U::Success);
    expectUpgrade(upgradeItem(bot, s.smith, *axeSlot, axe, 10).result, U::MaxLevel, "the axe at +10");
    // The sword: unequipped (only bag items are upgraded), +0 -> +5.
    const std::uint8_t swordSlot = freeBagSlot(s.inv);
    if (moveItem(bot, game::ItemPlace::equipment(game::EquipSlot::MainHand), game::ItemPlace::bag(swordSlot)) !=
        vn::ItemMoveResult::Ok)
        fail("unequipping the sword");
    s.updatesSeen = applyUpdatesSince(bot, s.inv, s.updatesSeen);
    for (int i = 0; i < 5; ++i)
        attempt(bot, items, s.smith, s.inv, s.updatesSeen, s.coins, swordSlot, U::Success);
    // Spend the rest on stones: +6 (36 Akçe) is NotEnoughCoins.
    const auto stoneCost = [&](std::uint16_t id) { return items.find(id)->value; };
    if (const std::uint32_t n = std::min<std::uint32_t>(s.coins / stoneCost(kUstaStone), 50); n > 0)
        buyStones(bot, s, items, kUstaStone, static_cast<std::uint16_t>(n));
    if (const std::uint32_t n = s.coins / stoneCost(kSafStone); n > 0)
        buyStones(bot, s, items, kSafStone, static_cast<std::uint16_t>(n));
    if (const std::uint32_t n = s.coins / stoneCost(kHamStone); n > 0)
        buyStones(bot, s, items, kHamStone, static_cast<std::uint16_t>(n));
    const std::uint32_t fee6 = game::upgradeFee(*items.find(kKitSword), 6);
    if (s.coins >= fee6)
        fail(std::format("{} Akçe left after buying stones (needed less than {})", s.coins, fee6));
    const std::size_t coinsSeen = v.coins.size();
    expectUpgrade(upgradeItem(bot, s.smith, swordSlot, s.inv.at(game::ItemPlace::bag(swordSlot)).id, 5).result,
                  U::NotEnoughCoins, std::format("+6 with {} Akçe", s.coins));
    if (v.coins.size() != coinsSeen)
        fail("NotEnoughCoins changed the balance");

    // Equip the +10 axe (two-handed: the shield goes to the bag), relog: levels kept, the attack counts them.
    if (moveItem(bot, game::ItemPlace::bag(*axeSlot), game::ItemPlace::equipment(game::EquipSlot::MainHand)) !=
        vn::ItemMoveResult::Ok)
        fail("equipping the +10 axe");
    s.updatesSeen = applyUpdatesSince(bot, s.inv, s.updatesSeen);
    const std::uint32_t coins = s.coins;
    leaveWorld(bot);
    const auto enter = enterWorld(bot, s.charId, t, s.self, 1);
    game::Inventory inv;
    (void)applyItemEntries(inv, v.inventory->items);
    const game::ItemStack& axeNow = inv.equipped(game::EquipSlot::MainHand);
    const game::ItemStack& swordNow = inv.at(game::ItemPlace::bag(swordSlot));
    if (axeNow.id != axe || axeNow.upgrade != 10 || swordNow.templateId != kKitSword || swordNow.upgrade != 5)
        fail(std::format("after the relog: main hand item {} +{}, bag {} template {} +{} (expected the axe +10, the "
                         "sword +5)",
                         axeNow.id, axeNow.upgrade, swordSlot, swordNow.templateId, swordNow.upgrade));
    if (v.coins.front().coins != std::max(coins, kUpgradeTestCoins)) // the testing hook tops it up again
        fail(std::format("after the relog {} Akçe (expected {})", v.coins.front().coins, std::max(coins, kUpgradeTestCoins)));
    const game::Stats expected = game::deriveStats(kUpgradeWarrior, 1, game::equipmentBonus(items, inv));
    const game::Stats plain = game::deriveStats(kUpgradeWarrior, 1, game::EquipBonus{items.find(kAxeItem)->attack, 0});
    if (enter.attack != expected.attack || enter.attack <= plain.attack)
        fail(std::format("entered with attack {} (expected {} with the +10 axe; {} at +0)", enter.attack, expected.attack,
                         plain.attack));
    if (std::ofstream out(c.opts.stateFile, std::ios::trunc); !c.opts.stateFile.empty())
    {
        out << s.name << " " << static_cast<int>(swordSlot) << "\n";
        if (!out)
            fail("cannot write --state-file");
    }

    // The flood cap: 6 UPGRADE_REQ at once (5 a second allowed).
    walkNear(bot, t, s.self, Point{town.smith->x, town.smith->z}, 2.5f);
    expectShop(openShop(bot, s.smith).result, vn::ShopResult::Ok, "opening the smith's shop again");
    for (int i = 0; i < 6; ++i)
        bot.send(vn::UpgradeReq{bot.nextSeq(), s.smith, swordSlot, swordNow.id, 5, vn::kUpgradeAnyStone});
    bool flooded = false;
    while (!flooded)
    {
        const auto m = bot.next("waiting for ERROR(Flood) after 6 UPGRADE_REQ");
        if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::Error))
        {
            vn::Error e;
            if (!vn::decodeMessage(m.payload, e) || e.errorCode != vn::ErrorCode::Flood)
                fail("an ERROR other than Flood");
            flooded = true;
        }
    }
    bot.expectClosed(true, bot.timeout());
    return std::format("'{}': NotOpen, NotSmith, NoStone, WrongStone, NotUpgradable, NoItem x2, Changed; the axe +0 -> +10 "
                       "(MaxLevel), the sword +5, NotEnoughCoins with {} Akçe; relog: axe +10 equipped (attack {}), "
                       "sword +5; ERROR(Flood) at the 6th UPGRADE_REQ",
                       s.name, coins, enter.attack);
}
// M15 "Avlaklar" (docs/PROTOCOL_LEVELS.md): the server runs with world.testing_xp_percent (RunScenario.ps1),
// so one hunt in a Kül Çukuru slot of Ortaocak carries a new warrior past level 11. Checks: the slot's
// members stand at their data homes (wander included), killing one levels the bot to 11+, the killed member
// comes back at its own home within the slot's respawn time, skill rank 4 is refused below level 11 and
// learnable from it (rank 5 LevelTooLow below 16). --state-file: the character name (the -Database run
// checks the stored level with vo_admin).
std::string scenarioHunt(const Context& c)
{
    const auto& t = terrainOf(c);
    if (!c.monsters)
        fail("gamedata/monsters.toml is needed");
    const game::MonsterData& data = *c.monsters;
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginAs(bot, c.user, c.opts.password);
    const std::uint32_t charId = createCharacter(bot, c.opts.race); // a warrior
    std::string name = "?";
    for (const auto& s : listChars(bot))
        if (s.charId == charId)
            name = s.name;
    if (!c.opts.stateFile.empty())
    {
        std::ofstream out(c.opts.stateFile);
        out << name << "\n";
    }
    Self self;
    const auto e = enterWorld(bot, charId, t, self);
    const std::uint32_t me = e.entityId;
    auto& v = bot.view();
    // Rank 2 of Ağır Darbe needs a point: none at level 1.
    if (learnSkill(bot, game::kSkillHeavyBlow).result != vn::SkillLearnResult::NotEnoughPoints)
        fail("a skill point at level 1");
    if (!bot.pumpUntil([&] { return v.count(vn::kEntityTypeMonster) > 0; }, Ms(3000)))
        fail("no monster spawned around the village");

    // 1. Every visible Kül Çömezi stands at a home of its slot (wander 1 m; idle ones only move inside it).
    const auto homeOf = [&](Point at, const game::MonsterSlot*& slotOut) -> std::optional<Point> {
        std::optional<Point> best;
        float bestD = 1e9f;
        for (const auto& s : data.slots())
            if (s.zone == 1)
                for (const auto& h : s.homes)
                    if (const float d = distance(at, {h.x, h.z}); d < bestD)
                    {
                        bestD = d;
                        best = Point{h.x, h.z};
                        slotOut = &s;
                    }
        return best;
    };
    int checked = 0;
    for (const auto& [id, r] : v.entities)
    {
        if (r.spawn.entityType != vn::kEntityTypeMonster || r.spawn.name != "Kül Çömezi" || r.last)
            continue;
        const game::MonsterSlot* slot = nullptr;
        const auto home = homeOf({r.spawn.x, r.spawn.z}, slot);
        if (!home || !slot || distance(*home, {r.spawn.x, r.spawn.z}) > slot->wander + 0.05f)
            fail(std::format("Kül Çömezi {} spawned at ({:.1f}, {:.1f}), not at a home of its slot", id, r.spawn.x, r.spawn.z));
        ++checked;
    }
    if (checked == 0)
        fail("no idle Kül Çömezi in view to check against its slot");

    // 2. Hunt until level 11 or more (one kill does it with the hook; a death respawns and goes on).
    std::uint32_t victim = 0;
    Point victimHome{};
    const game::MonsterSlot* victimSlot = nullptr;
    int kills = 0, deaths = 0;
    const auto huntEnd = Clock::now() + Ms(240000);
    while ((v.levelUps.empty() || v.levelUps.back().level < 11) && Clock::now() < huntEnd)
    {
        std::uint32_t pick = 0;
        switch (fightThrall(bot, t, self, me, data, pick))
        {
        case FightOutcome::Killed:
        {
            ++kills;
            victim = pick;
            const auto& r = v.entities.at(pick);
            victimHome = *homeOf({r.spawn.x, r.spawn.z}, victimSlot);
            break;
        }
        case FightOutcome::Died:
            ++deaths;
            respawn(bot, self, me);
            break;
        case FightOutcome::NoTarget: bot.pumpFor(Ms(1000)); break;
        }
    }
    if (v.levelUps.empty() || v.levelUps.back().level < 11)
        fail(std::format("still below level 11 after {} kill(s), {} death(s)", kills, deaths));
    const std::uint16_t level = v.levelUps.back().level;
    if (!victimSlot || victimSlot->respawnMs == 0)
        fail("the killed Kül Çömezi belongs to no slot with its own respawn time");

    // 3. The member comes back at its own home: despawn after the corpse time, ENTITY_SPAWN within the slot's
    // respawn time (+ 2 s) of its death.
    // (EntityDeath stays in the view's death list: watch the despawn, then a fresh ENTITY_SPAWN.)
    const auto died = Clock::now();
    if (!bot.pumpUntil([&] { return !v.entities.contains(victim); }, Ms(game::kMonsterCorpseTime.count() + 3000)))
        fail("the killed member's corpse was not despawned");
    const bool back = bot.pumpUntil(
        [&] {
            const auto it = v.entities.find(victim);
            return it != v.entities.end() && (it->second.spawn.state & vn::kEntityStateDead) == 0;
        },
        Ms(victimSlot->respawnMs + 6000));

    if (!back)
        fail(std::format("the killed member of '{}' did not come back within {} ms", victimSlot->key, victimSlot->respawnMs + 6000));
    const auto& again = v.entities.at(victim);
    const float off = distance(victimHome, {again.spawn.x, again.spawn.z});
    if (off > victimSlot->wander + 0.05f)
        fail(std::format("the member came back {:.2f} m from its home (wander {:.1f})", off, victimSlot->wander));
    const auto waited = std::chrono::duration_cast<Ms>(Clock::now() - died).count();

    // 4. Skill ranks 4 and 5 (v9): rank 4 from level 11, rank 5 refused below 16.
    for (int rank = 2; rank <= 4; ++rank)
    {
        const auto r = learnSkill(bot, game::kSkillHeavyBlow);
        if (r.result != vn::SkillLearnResult::Ok || r.rank != rank)
            fail(std::format("learning rank {} of Ağır Darbe at level {}: {} (rank {})", rank, level,
                             vn::skillLearnResultName(r.result), r.rank));
    }
    const auto five = learnSkill(bot, game::kSkillHeavyBlow);
    if (level < 16 && five.result != vn::SkillLearnResult::LevelTooLow)
        fail(std::format("rank 5 at level {}: {} (expected LevelTooLow)", level, vn::skillLearnResultName(five.result)));
    expectNoViewErrors(bot, "hunt view");
    expectNoZoneChange(bot, self, "hunt");
    bot.send(vn::Logout{});
    bot.expectClosed(true, bot.timeout());
    return std::format("'{}': {} slot member(s) at their homes, level {} after {} kill(s) ({} death(s)); the member of '{}' "
                       "came back at its home {:.2f} m off after {} ms; Ağır Darbe rank 4, rank 5 {}",
                       name, checked, level, kills, deaths, victimSlot->key, off, waited, vn::skillLearnResultName(five.result));
}

} // namespace vo::tools::botclient
