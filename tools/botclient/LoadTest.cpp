// Load mode of the bot client: --bots N --duration S (docs/SERVER_WORLD.md §9).
//
// N asynchronous bots spread over a few threads (each thread one io_context) connect one after
// another (--ramp-ms apart), log in (auto-registration), create a character if they have none, enter
// the world and wander: straight legal walks/runs to random targets 5-30 m away (checked against the
// walk mask), stop, idle 0.5-3 s, repeat; MOVE_REQ at 10 Hz while moving and PING every second.
// Once every bot is in the world (or the ramp timed out) the tool measures for S seconds and reports
// connects, corrections, disconnects, received bytes/packets per client per second and the PING
// round-trip percentiles, then logs everybody out. Every third bot also makes a legal equip/unequip
// (planned with the shared item rules) every 3-8 s and checks ITEM_UPDATE + ITEM_MOVE_RES(Ok).
// Combat (docs/PROTOCOL_COMBAT.md): every fifth bot is a fighter: it walks to the nearest monster of
// its level (never the elite), auto-attacks it, uses its class skill when ready (the priest heals
// itself), and when it dies waits 5 s and sends RESPAWN_REQ. The other bots wander inside the
// village's safe zone, where no monster can reach them. Loot (docs/PROTOCOL_LOOT.md): a fighter walks
// to every container of its own kills it sees and takes everything (LOOT_TAKE_REQ 0xFF), checking
// that the answer comes and is not ServerError.
// PASS if all bots got in, nobody was disconnected, no item move was answered badly, no fighter's
// RESPAWN_REQ after 5 s was refused and corrections <= --max-corrections (default 0); corrections of
// a dead fighter's moves still in flight when it died are counted separately and allowed.

#include "shared/game/Character.h"
#include "shared/game/Combat.h"
#include "shared/game/Movement.h"
#include "shared/game/PathFind.h"
#include "tools/botclient/Bot.h"
#include "tools/botclient/Nav.h"

#include <asio/executor_work_guard.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>

namespace vo::tools::botclient {

namespace {

constexpr std::size_t kFrameOverhead = 2 + 16 + 2; // length + AEAD tag + opcode per encrypted frame

struct Shared
{
    const Context* ctx = nullptr;
    const assets::TerrainData* terrain = nullptr;
    Clock::time_point start;
    std::atomic<int> inWorld{0};
    std::atomic<int> failed{0};
    std::atomic<bool> measuring{false};
    std::atomic<bool> finishing{false};
    std::mutex errorMutex;
    std::map<std::string, int> errors; // reason -> count
    void error(const std::string& why)
    {
        const std::scoped_lock lock(errorMutex);
        ++errors[why];
    }
};

struct Counters
{
    std::atomic<std::uint64_t> bytesIn{0};
    std::atomic<std::uint64_t> packetsIn{0};
    std::atomic<std::uint64_t> entityMoves{0};
    std::atomic<std::uint64_t> spawns{0};
    std::atomic<std::uint64_t> despawns{0};
    std::atomic<std::uint64_t> corrections{0};
    std::atomic<std::uint64_t> movesSent{0};
    std::atomic<std::uint64_t> itemMovesOk{0};
    std::atomic<std::uint64_t> itemMovesBad{0}; // refused although planned legal, or out of order
    std::atomic<std::uint64_t> equips{0};       // ENTITY_EQUIP received (own and others')
    // Combat.
    std::atomic<std::uint64_t> combatEvents{0};    // COMBAT_EVENT received (own and others')
    std::atomic<std::uint64_t> kills{0};           // XP_GAIN received by fighters
    std::atomic<std::uint64_t> deaths{0};          // own deaths of fighters
    std::atomic<std::uint64_t> respawns{0};        // RESPAWN_RES(Ok)
    std::atomic<std::uint64_t> respawnsBad{0};     // RESPAWN_REQ after 5 s refused
    std::atomic<std::uint64_t> deadCorrections{0}; // POSITION_CORRECT while dead (moves in flight)
    std::atomic<std::uint64_t> levelUps{0};
    // Loot (docs/PROTOCOL_LOOT.md).
    std::atomic<std::uint64_t> lootTakes{0};  // LOOT_TAKE_RES Ok / BagFull / PurseFull
    std::atomic<std::uint64_t> lootMissed{0}; // NotFound / OutOfRange / AlreadyTaken / NotYours / YouAreDead
    std::atomic<std::uint64_t> lootBad{0};    // ServerError, an unexpected answer, or no answer in 10 s
    std::atomic<std::uint64_t> coinsGained{0};
    std::atomic<std::uint64_t> lootItems{0}; // bag slots changed by loot (ITEM_UPDATE entries)
};

struct Snapshot
{
    std::uint64_t bytesIn = 0, packetsIn = 0, entityMoves = 0, spawns = 0, despawns = 0, corrections = 0,
                  movesSent = 0, itemMovesOk = 0, itemMovesBad = 0, equips = 0, combatEvents = 0, kills = 0,
                  deaths = 0, respawns = 0, respawnsBad = 0, deadCorrections = 0, levelUps = 0, lootTakes = 0,
                  lootMissed = 0, lootBad = 0, coinsGained = 0, lootItems = 0;
};

class LoadBot
{
public:
    enum class State
    {
        Waiting,
        Connecting,
        LoggingIn,
        Listing,
        Creating,
        Entering,
        InWorld,
        LoggingOut,
        Done,
    };

    LoadBot(asio::io_context& io, Shared& shared, int index, std::string user)
        : m_shared(shared), m_session(io), m_index(index), m_user(std::move(user)),
          m_rng(static_cast<std::uint32_t>(std::random_device{}()) ^ static_cast<std::uint32_t>(index * 7919)),
          m_nav(*shared.terrain), m_itemBot(index % 3 == 0 && shared.ctx->items != nullptr)
    {
        m_fighter = index % 5 == 2 && shared.ctx->monsters != nullptr;
        m_itemBot = m_itemBot && !m_fighter; // a move in flight at death would be refused (SlotBlocked)
        if (shared.ctx->monsters)
            for (const auto& z : shared.ctx->monsters->safeZones())
                if (z.zone == 1)
                    m_village.push_back(z);
        m_session.setMessageHandler([this](std::uint16_t op, std::span<const std::uint8_t> p) { onMessage(op, p); });
        m_session.setClosedHandler([this](vn::CloseReason r) { onClosed(r); });
    }

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] bool disconnected() const noexcept { return m_disconnected; }
    [[nodiscard]] const Counters& counters() const noexcept { return m_counters; }
    [[nodiscard]] const std::vector<float>& rtts() const noexcept { return m_rtts; }

    void drive(Clock::time_point now)
    {
        switch (m_state)
        {
        case State::Waiting:
            if (now >= m_shared.start + Ms(static_cast<long long>(m_index) * m_shared.ctx->opts.rampMs))
                connect();
            return;
        case State::InWorld:
            if (m_shared.finishing.load(std::memory_order_relaxed))
            {
                m_state = State::LoggingOut;
                (void)m_session.sendMessage(vn::Logout{});
                return;
            }
            wander(now);
            return;
        default: return;
        }
    }

    void forceClose()
    {
        if (m_state != State::Done)
        {
            m_state = State::Done;
            m_session.close();
        }
    }

private:
    void connect()
    {
        m_state = State::Connecting;
        vn::ClientSettings s = m_shared.ctx->settings;
        s.handshakeTimeout = Ms(30000);
        m_session.connect(s, [this](vn::ConnectResult r, const std::string& detail) {
            if (r != vn::ConnectResult::Ok)
            {
                failNow(std::format("connect: {} {}", vn::connectResultName(r), detail));
                return;
            }
            m_state = State::LoggingIn;
            (void)m_session.sendMessage(vn::LoginReq{m_user, m_shared.ctx->opts.password});
        });
    }

    void failNow(const std::string& why)
    {
        m_shared.error(why);
        ++m_shared.failed;
        if (m_state == State::InWorld)
            --m_shared.inWorld;
        m_state = State::Done;
        m_session.close();
    }

    template <class T>
    bool decode(std::span<const std::uint8_t> p, T& out)
    {
        if (vn::decodeMessage(p, out))
            return true;
        failNow(std::format("malformed {}", vn::opcodeName(T::kOpcode)));
        return false;
    }

    void onMessage(std::uint16_t op, std::span<const std::uint8_t> p)
    {
        const auto now = Clock::now();
        m_counters.bytesIn += p.size() + kFrameOverhead;
        ++m_counters.packetsIn;
        switch (static_cast<vn::Opcode>(op))
        {
        case vn::Opcode::LoginRes:
        {
            vn::LoginRes r;
            if (!decode(p, r))
                return;
            if (r.result != vn::LoginResult::Ok)
                return failNow(std::format("login result {}", static_cast<int>(r.result)));
            m_state = State::Listing;
            (void)m_session.sendMessage(vn::CharListReq{});
            return;
        }
        case vn::Opcode::CharListRes:
        {
            vn::CharListRes r;
            if (!decode(p, r))
                return;
            if (r.characters.empty())
            {
                m_state = State::Creating;
                vn::CharCreateReq req;
                req.name = randomCharacterName();
                req.race = static_cast<std::uint8_t>(1 + m_index % 2);
                req.charClass = static_cast<std::uint8_t>(1 + m_index % 4);
                req.face = static_cast<std::uint8_t>(m_index % 4);
                req.hair = static_cast<std::uint8_t>((m_index / 4) % 4);
                (void)m_session.sendMessage(req);
                return;
            }
            m_state = State::Entering;
            m_itemUser = {r.characters.front().charClass, r.characters.front().level};
            m_class = r.characters.front().charClass;
            (void)m_session.sendMessage(vn::EnterWorldReq{r.characters.front().charId});
            return;
        }
        case vn::Opcode::CharCreateRes:
        {
            vn::CharCreateRes r;
            if (!decode(p, r))
                return;
            if (r.result != vn::CharCreateResult::Ok)
                return failNow(std::format("create result {}", static_cast<int>(r.result)));
            m_state = State::Entering;
            m_itemUser = {r.character.charClass, r.character.level};
            m_class = r.character.charClass;
            (void)m_session.sendMessage(vn::EnterWorldReq{r.character.charId});
            return;
        }
        case vn::Opcode::EnterWorldRes:
        {
            vn::EnterWorldRes r;
            if (!decode(p, r))
                return;
            if (r.result != vn::EnterWorldResult::Ok)
                return failNow(std::format("enter result {}", static_cast<int>(r.result)));
            m_state = State::InWorld;
            ++m_shared.inWorld;
            m_entityId = r.entityId;
            m_hasInventory = false;
            m_nextItemMove = now + Ms(std::uniform_int_distribution<int>(1000, 4000)(m_rng));
            m_pos = {r.x, r.z};
            m_yaw = r.yaw;
            m_level = r.level;
            m_itemUser.level = r.level;
            m_hp = r.hp;
            m_maxHp = r.maxHp;
            m_mp = r.mp;
            m_dead = r.isDead;
            std::uniform_int_distribution<int> stagger(0, 99);
            m_nextStep = now + Ms(stagger(m_rng));
            m_nextPing = now + Ms(stagger(m_rng) * 10);
            m_idleUntil = now + Ms(stagger(m_rng) * 10);
            return;
        }
        case vn::Opcode::Pong:
        {
            vn::Pong r;
            if (!decode(p, r))
                return;
            if (r.clientTimeMs == m_pingStamp && m_pingOutstanding)
            {
                m_pingOutstanding = false;
                if (m_shared.measuring.load(std::memory_order_relaxed))
                    m_rtts.push_back(std::chrono::duration<float, std::milli>(now - m_pingSent).count());
            }
            return;
        }
        case vn::Opcode::PositionCorrect:
        {
            vn::PositionCorrect r;
            if (!decode(p, r))
                return;
            if (m_dead)
                ++m_counters.deadCorrections; // a move that was in flight when the fighter died
            else
                ++m_counters.corrections;
            m_pos = {r.x, r.z};
            m_path.clear();
            m_yaw = r.yaw;
            m_moving = false;
            m_idleUntil = now + Ms(300);
            return;
        }
        case vn::Opcode::Inventory:
        {
            vn::Inventory r;
            if (!decode(p, r))
                return;
            if (m_state != State::InWorld || m_hasInventory)
                return failNow("INVENTORY out of order");
            m_inventory = {};
            if (!applyItemEntries(m_inventory, r.items))
                return failNow("INVENTORY with an invalid place");
            m_hasInventory = true;
            return;
        }
        case vn::Opcode::ItemUpdate:
        {
            vn::ItemUpdate r;
            if (!decode(p, r))
                return;
            if (m_lootPending)
            {
                // The bag slots a loot take filled (they come before its LOOT_TAKE_RES).
                if (!applyItemEntries(m_inventory, r.items))
                    ++m_counters.lootBad;
                m_counters.lootItems += r.items.size();
                return;
            }
            if (!m_itemPending || !applyItemEntries(m_inventory, r.items))
            {
                ++m_counters.itemMovesBad;
                m_shared.error("unexpected or invalid ITEM_UPDATE");
                return;
            }
            m_itemUpdateSeen = true;
            return;
        }
        case vn::Opcode::ItemMoveRes:
        {
            vn::ItemMoveRes r;
            if (!decode(p, r))
                return;
            if (!m_itemPending || r.seq != m_itemSeq)
            {
                ++m_counters.itemMovesBad;
                m_shared.error("unexpected ITEM_MOVE_RES");
                return;
            }
            m_itemPending = false;
            if (r.result != vn::ItemMoveResult::Ok)
            {
                ++m_counters.itemMovesBad;
                m_shared.error(std::format("legal item move answered {}", vn::itemMoveResultName(r.result)));
            }
            else if (!m_itemUpdateSeen)
            {
                ++m_counters.itemMovesBad;
                m_shared.error("ITEM_MOVE_RES(Ok) without ITEM_UPDATE before it");
            }
            else
            {
                ++m_counters.itemMovesOk;
            }
            m_nextItemMove = now + Ms(std::uniform_int_distribution<int>(3000, 8000)(m_rng));
            return;
        }
        case vn::Opcode::EntityEquip: ++m_counters.equips; return;
        case vn::Opcode::EntityMove:
        {
            ++m_counters.entityMoves;
            if (!m_fighter)
                return;
            vn::EntityMove r;
            if (!decode(p, r))
                return;
            if (auto it = m_monsters.find(r.entityId); it != m_monsters.end())
                it->second.pos = {r.x, r.z};
            return;
        }
        case vn::Opcode::EntitySpawn:
        {
            ++m_counters.spawns;
            if (!m_fighter)
                return;
            vn::EntitySpawn r;
            if (!decode(p, r))
                return;
            if (r.entityType == vn::kEntityTypeMonster)
            {
                const auto* t = m_shared.ctx->monsters->find(r.templateId);
                m_monsters[r.entityId] = {{r.x, r.z}, r.level, t && t->elite,
                                          (r.state & vn::kEntityStateDead) != 0};
            }
            else if (r.entityType == vn::kEntityTypeLoot && (r.state & vn::kEntityStateLootMine) != 0)
            {
                m_loots[r.entityId] = {r.x, r.z}; // our own drops only (others' are locked for 30 s)
            }
            return;
        }
        case vn::Opcode::EntityDespawn:
        {
            ++m_counters.despawns;
            if (!m_fighter)
                return;
            vn::EntityDespawn r;
            if (decode(p, r))
            {
                m_monsters.erase(r.entityId);
                m_loots.erase(r.entityId);
                if (r.entityId == m_lootTarget && !m_lootPending)
                    m_lootTarget = 0;
            }
            return;
        }
        case vn::Opcode::LootTakeRes:
        {
            vn::LootTakeRes r;
            if (!decode(p, r))
                return;
            if (!m_lootPending || r.seq != m_lootSeq)
            {
                ++m_counters.lootBad;
                m_shared.error("unexpected LOOT_TAKE_RES");
                return;
            }
            m_lootPending = false;
            m_loots.erase(m_lootTarget);
            m_lootTarget = 0;
            switch (r.result)
            {
            case vn::LootResult::Ok:
            case vn::LootResult::BagFull:
            case vn::LootResult::PurseFull: ++m_counters.lootTakes; break;
            case vn::LootResult::ServerError:
                ++m_counters.lootBad;
                m_shared.error("LOOT_TAKE_REQ answered ServerError");
                break;
            default: ++m_counters.lootMissed; break;
            }
            return;
        }
        case vn::Opcode::Coins:
        {
            vn::Coins r;
            if (decode(p, r))
                m_counters.coinsGained += r.gained;
            return;
        }
        case vn::Opcode::EntityState:
        {
            if (!m_fighter)
                return;
            vn::EntityState r;
            if (!decode(p, r))
                return;
            if (auto it = m_monsters.find(r.entityId); it != m_monsters.end())
                it->second.dead = (r.state & vn::kEntityStateDead) != 0;
            return;
        }
        case vn::Opcode::CombatEvent: ++m_counters.combatEvents; return;
        case vn::Opcode::EntityDeath:
        {
            vn::EntityDeath r;
            if (!decode(p, r))
                return;
            if (r.entityId == m_entityId)
            {
                m_dead = true;
                m_diedAt = now;
                m_respawnSent = false;
                m_moving = false;
                m_path.clear();
                m_foe = 0;
                ++m_counters.deaths;
            }
            else if (auto it = m_monsters.find(r.entityId); it != m_monsters.end())
                it->second.dead = true;
            return;
        }
        case vn::Opcode::SelfStats:
        {
            vn::SelfStats r;
            if (!decode(p, r))
                return;
            m_hp = r.hp;
            m_maxHp = r.maxHp;
            m_mp = r.mp;
            return;
        }
        case vn::Opcode::XpGain: ++m_counters.kills; return;
        case vn::Opcode::LevelUp:
        {
            vn::LevelUp r;
            if (!decode(p, r))
                return;
            if (r.entityId == m_entityId)
            {
                m_level = r.level;
                m_itemUser.level = r.level;
                ++m_counters.levelUps;
            }
            return;
        }
        case vn::Opcode::RespawnRes:
        {
            vn::RespawnRes r;
            if (!decode(p, r))
                return;
            m_respawnSent = false;
            if (r.result != vn::RespawnResult::Ok)
            {
                ++m_counters.respawnsBad;
                m_shared.error(std::format("RESPAWN_REQ answered {}", static_cast<int>(r.result)));
                return;
            }
            ++m_counters.respawns;
            m_dead = false;
            m_pos = {r.x, r.z};
            m_yaw = r.yaw;
            m_hp = r.hp;
            m_mp = r.mp;
            m_moving = false;
            m_path.clear();
            m_idleUntil = now + Ms(500);
            return;
        }
        case vn::Opcode::Error:
        {
            vn::Error e;
            if (vn::decodeMessage(p, e))
                m_shared.error(std::format("ERROR({}) {}", vn::errorCodeName(e.errorCode), e.debugMessage));
            return;
        }
        default: return;
        }
    }

    void onClosed(vn::CloseReason r)
    {
        if (m_state == State::LoggingOut || m_state == State::Done)
        {
            m_state = State::Done;
            return;
        }
        m_disconnected = true;
        m_shared.error(std::format("disconnected ({}) in state {}", vn::closeReasonName(r), static_cast<int>(m_state)));
        if (m_state == State::InWorld)
            --m_shared.inWorld;
        ++m_shared.failed;
        m_state = State::Done;
    }

    void sendMove(std::uint8_t flags)
    {
        const auto& t = *m_shared.terrain;
        vn::MoveReq m;
        m.seq = ++m_seq;
        m.flags = flags;
        m.x = m_pos.x;
        m.y = t.heightAt(m_pos.x, m_pos.z);
        m.z = m_pos.z;
        m.yaw = m_yaw;
        if (m_session.sendMessage(m))
            ++m_counters.movesSent;
    }

    // Item bots: now and then a legal equip / unequip (planned with the shared rules, so the server
    // must answer Ok after an ITEM_UPDATE).
    void itemStep(Clock::time_point now)
    {
        if (!m_itemBot || !m_hasInventory)
            return;
        if (m_dead && !m_itemPending)
            return; // item moves are refused while dead (PROTOCOL_COMBAT.md §1.1)
        if (m_itemPending)
        {
            if (now - m_itemSentAt > Ms(10000))
            {
                m_itemPending = false;
                ++m_counters.itemMovesBad;
                m_shared.error("no ITEM_MOVE_RES within 10 s");
            }
            return;
        }
        if (now < m_nextItemMove)
            return;
        const auto& catalog = *m_shared.ctx->items;
        const auto main = game::ItemPlace::equipment(game::EquipSlot::MainHand);
        std::optional<std::pair<game::ItemPlace, game::ItemPlace>> pick;
        const auto legal = [&](game::ItemPlace from) {
            const auto to = game::quickMoveTarget(catalog, m_inventory, from);
            if (to && game::planItemMove(catalog, m_inventory, m_itemUser, from, *to).result == vn::ItemMoveResult::Ok)
                pick = std::make_pair(from, *to);
            return pick.has_value();
        };
        if (m_inventory.equipped(game::EquipSlot::MainHand).empty() || !legal(main))
        {
            for (std::uint8_t s = 0; s < game::kBagSlots && !pick; ++s)
                if (!m_inventory.at(game::ItemPlace::bag(s)).empty())
                    (void)legal(game::ItemPlace::bag(s));
        }
        m_nextItemMove = now + Ms(std::uniform_int_distribution<int>(3000, 8000)(m_rng));
        if (!pick)
            return;
        m_itemSeq = ++m_itemSeqCounter;
        m_itemPending = true;
        m_itemUpdateSeen = false;
        m_itemSentAt = now;
        (void)m_session.sendMessage(
            vn::ItemMoveReq{m_itemSeq, pick->first.container, pick->first.slot, pick->second.container, pick->second.slot});
    }

    void wander(Clock::time_point now)
    {
        itemStep(now);
        if (now >= m_nextPing && !m_pingOutstanding)
        {
            m_pingStamp = nowMs();
            m_pingSent = now;
            m_pingOutstanding = m_session.sendMessage(vn::Ping{m_pingStamp});
            m_nextPing = now + Ms(1000);
        }
        else if (m_pingOutstanding && now - m_pingSent > Ms(10000))
        {
            m_pingOutstanding = false; // lost? keep going
        }
        if (now < m_nextStep)
            return;
        m_nextStep += Ms(100);
        if (m_nextStep < now)
            m_nextStep = now + Ms(100);
        if (m_fighter)
        {
            fight(now);
            return;
        }

        if (!m_moving)
        {
            if (now < m_idleUntil)
                return;
            auto target = m_nav.randomStraightTarget(m_pos, m_rng, 5.0f, 30.0f);
            if (target && !inVillage(*target))
                target.reset(); // stay where no monster can reach (the safe zone is convex)
            if (!target)
            {
                m_idleUntil = now + Ms(500);
                return;
            }
            m_target = *target;
            m_run = std::uniform_int_distribution<int>(0, 2)(m_rng) != 0;
            m_moving = true;
            m_lastStep = now;
            m_yaw = game::yawFromDirection(m_target.x - m_pos.x, m_target.z - m_pos.z);
            sendMove(static_cast<std::uint8_t>(game::kMoveMoving | (m_run ? game::kMoveRunning : 0)));
            return;
        }
        const float speed = (m_run ? game::kRunSpeed : game::kWalkSpeed) * 0.9f;
        const float budget = speed * std::chrono::duration<float>(now - m_lastStep).count();
        m_lastStep = now;
        const float d = distance(m_pos, m_target);
        const std::uint8_t runFlag = m_run ? game::kMoveRunning : 0;
        if (d <= budget)
        {
            m_pos = m_target;
            sendMove(runFlag); // stop
            m_moving = false;
            m_idleUntil = now + Ms(std::uniform_int_distribution<int>(500, 3000)(m_rng));
            return;
        }
        m_pos.x += (m_target.x - m_pos.x) * budget / d;
        m_pos.z += (m_target.z - m_pos.z) * budget / d;
        sendMove(static_cast<std::uint8_t>(game::kMoveMoving | runFlag));
    }

    // Inside the village safe zone with 4 m to spare (always true without zone data).
    [[nodiscard]] bool inVillage(Point p) const
    {
        if (m_village.empty())
            return true;
        for (const auto& z : m_village)
            if (distance(p, {z.x, z.z}) <= z.radius - 4.0f)
                return true;
        return false;
    }

    void stopFighter()
    {
        if (m_moving)
            sendMove(game::kMoveRunning);
        m_moving = false;
        m_path.clear();
    }

    // One 100 ms step of a fighter (see the file comment).
    void fight(Clock::time_point now)
    {
        if (m_dead)
        {
            if (!m_respawnSent && now - m_diedAt > game::kRespawnMinDelay + Ms(500))
            {
                m_respawnSent = m_session.sendMessage(vn::RespawnReq{});
                m_respawnSentAt = now;
            }
            else if (m_respawnSent && now - m_respawnSentAt > Ms(10000))
                m_respawnSent = false; // no answer: ask again
            return;
        }
        if (now < m_idleUntil)
            return;
        if (lootStep(now))
            return;
        auto it = m_monsters.find(m_foe);
        if (m_foe == 0 || it == m_monsters.end() || it->second.dead)
        {
            m_foe = 0;
            float best = 150.0f;
            for (const auto& [id, mon] : m_monsters)
            {
                if (mon.dead || mon.elite || mon.level > m_level + 1)
                    continue;
                const float d = distance(m_pos, mon.pos);
                if (d < best)
                {
                    best = d;
                    m_foe = id;
                }
            }
            if (m_foe == 0)
            {
                stopFighter();
                m_idleUntil = now + Ms(1000);
                return;
            }
            it = m_monsters.find(m_foe);
            m_attackSent = false;
            m_path.clear();
        }
        const Point goal = it->second.pos;
        const float d = distance(m_pos, goal);
        const bool mage = m_class == static_cast<std::uint8_t>(game::CharClass::Mage);
        const float desired = mage ? 9.0f : 1.6f;
        if (!m_attackSent)
            m_attackSent = m_session.sendMessage(vn::AttackReq{m_foe});
        if (d > desired + 0.6f)
            stepTowards(now, goal);
        else if (m_moving)
            stopFighter();
        // The class skill when ready (local cooldown bookkeeping), never while moving (casts would be
        // interrupted). The priest heals itself below 70 % hp.
        const game::SkillDef* skill = game::classSkill(m_class);
        if (!skill || m_moving || now < m_skillReadyAt || m_mp < skill->mpCost)
            return;
        if (skill->target == game::SkillTarget::SelfOrFriendly)
        {
            if (m_hp * 10 >= m_maxHp * 7)
                return;
            (void)m_session.sendMessage(vn::SkillReq{skill->id, 0});
        }
        else
        {
            if (!game::inRange(d, skill->range))
                return;
            (void)m_session.sendMessage(vn::SkillReq{skill->id, m_foe});
        }
        m_skillReadyAt = now + Ms(skill->cooldownMs + skill->castMs + 400);
    }

    // Fighters loot what they killed (PROTOCOL_LOOT.md): walk to their own container, take
    // everything, then fight on. True while busy with loot (no fighting in that step).
    bool lootStep(Clock::time_point now)
    {
        if (m_lootPending)
        {
            if (now - m_lootSentAt > Ms(10000))
            {
                m_lootPending = false;
                m_lootTarget = 0;
                ++m_counters.lootBad;
                m_shared.error("no LOOT_TAKE_RES within 10 s");
            }
            return true;
        }
        if (m_lootTarget == 0)
        {
            float best = 40.0f;
            for (const auto& [id, pos] : m_loots)
            {
                const float d = distance(m_pos, pos);
                if (d < best)
                {
                    best = d;
                    m_lootTarget = id;
                }
            }
            if (m_lootTarget == 0)
                return false;
            m_path.clear();
        }
        const auto it = m_loots.find(m_lootTarget);
        if (it == m_loots.end())
        {
            m_lootTarget = 0;
            return false;
        }
        if (distance(m_pos, it->second) > 1.8f)
        {
            const bool wasMoving = m_moving;
            stepTowards(now, it->second);
            if (wasMoving && !m_moving)
            {
                // Unreachable (stepTowards gave up on the path): forget this container.
                m_loots.erase(it);
                m_lootTarget = 0;
            }
            return true;
        }
        stopFighter();
        m_lootSeq = ++m_lootSeqCounter;
        m_lootPending = m_session.sendMessage(vn::LootTakeReq{m_lootSeq, m_lootTarget, vn::kLootTakeAll});
        m_lootSentAt = now;
        return true;
    }

    // Runs along a walkable path towards `goal` (re-planned when the goal moved), never past a path
    // vertex within one step (the server checks the straight line).
    void stepTowards(Clock::time_point now, Point goal)
    {
        const auto& t = *m_shared.terrain;
        if (!m_moving)
        {
            m_moving = true;
            m_run = true;
            m_lastStep = now;
            m_path.clear();
            sendMove(static_cast<std::uint8_t>(game::kMoveMoving | game::kMoveRunning)); // start where we stand
            return;
        }
        if (m_pathIndex >= m_path.size() || distance(m_pathGoal, goal) > 2.0f || now - m_lastPlan > Ms(1500))
        {
            m_lastPlan = now;
            m_pathGoal = goal;
            m_path.clear();
            m_pathIndex = 0;
            if (game::isSegmentWalkable(t, m_pos.x, m_pos.z, goal.x, goal.z))
                m_path.push_back({goal.x, goal.z});
            else if (auto found = game::findWalkPath(t, {m_pos.x, m_pos.z}, {goal.x, goal.z}, 20000))
                m_path = std::move(*found);
            if (m_path.empty())
            {
                m_foe = 0; // unreachable: pick another one
                stopFighter();
                return;
            }
        }
        const float budget =
            game::kRunSpeed * 0.9f * std::chrono::duration<float>(now - m_lastStep).count();
        m_lastStep = now;
        const game::PathPoint next = m_path[m_pathIndex];
        const float d = distance(m_pos, {next.x, next.z});
        if (d > 1e-4f)
            m_yaw = game::yawFromDirection(next.x - m_pos.x, next.z - m_pos.z);
        if (d <= budget)
        {
            m_pos = {next.x, next.z};
            ++m_pathIndex;
        }
        else
        {
            m_pos.x += (next.x - m_pos.x) * budget / d;
            m_pos.z += (next.z - m_pos.z) * budget / d;
        }
        sendMove(static_cast<std::uint8_t>(game::kMoveMoving | game::kMoveRunning));
    }

    Shared& m_shared;
    vn::ClientSession m_session;
    int m_index;
    std::string m_user;
    State m_state = State::Waiting;
    bool m_disconnected = false;
    std::mt19937 m_rng;
    Nav m_nav;
    Counters m_counters;
    std::vector<float> m_rtts;

    Point m_pos;
    float m_yaw = 0.0f;
    std::uint32_t m_seq = 0;
    bool m_moving = false;
    bool m_run = false;
    Point m_target;
    Clock::time_point m_lastStep, m_nextStep, m_idleUntil, m_nextPing, m_pingSent;
    std::uint32_t m_pingStamp = 0;
    bool m_pingOutstanding = false;

    // Items.
    bool m_itemBot = false;
    std::uint32_t m_entityId = 0;
    game::ItemUser m_itemUser;
    game::Inventory m_inventory;
    bool m_hasInventory = false;
    bool m_itemPending = false;
    bool m_itemUpdateSeen = false;
    std::uint32_t m_itemSeq = 0;
    std::uint32_t m_itemSeqCounter = 0;
    Clock::time_point m_nextItemMove, m_itemSentAt;

    // Combat.
    bool m_fighter = false;
    std::vector<game::SafeZone> m_village;
    std::uint8_t m_class = 1;
    std::uint16_t m_level = 1;
    std::uint32_t m_hp = 0, m_maxHp = 0, m_mp = 0;
    bool m_dead = false;
    bool m_respawnSent = false;
    Clock::time_point m_diedAt, m_respawnSentAt, m_skillReadyAt;
    struct KnownMonster
    {
        Point pos;
        std::uint16_t level = 1;
        bool elite = false;
        bool dead = false;
    };
    std::map<std::uint32_t, KnownMonster> m_monsters;
    std::map<std::uint32_t, Point> m_loots; // our own loot containers in view
    std::uint32_t m_lootTarget = 0;
    bool m_lootPending = false;
    std::uint32_t m_lootSeq = 0, m_lootSeqCounter = 0;
    Clock::time_point m_lootSentAt;
    std::uint32_t m_foe = 0; // the monster a fighter attacks
    bool m_attackSent = false;
    std::vector<game::PathPoint> m_path;
    std::size_t m_pathIndex = 0;
    Point m_pathGoal;
    Clock::time_point m_lastPlan;
};

struct Worker
{
    asio::io_context io;
    std::vector<std::unique_ptr<LoadBot>> bots;
    std::thread thread;
    std::atomic<bool> stop{false};
};

Snapshot snapshot(const std::vector<std::unique_ptr<Worker>>& workers)
{
    Snapshot s;
    for (const auto& w : workers)
        for (const auto& b : w->bots)
        {
            const auto& c = b->counters();
            s.bytesIn += c.bytesIn.load();
            s.packetsIn += c.packetsIn.load();
            s.entityMoves += c.entityMoves.load();
            s.spawns += c.spawns.load();
            s.despawns += c.despawns.load();
            s.corrections += c.corrections.load();
            s.movesSent += c.movesSent.load();
            s.itemMovesOk += c.itemMovesOk.load();
            s.itemMovesBad += c.itemMovesBad.load();
            s.equips += c.equips.load();
            s.combatEvents += c.combatEvents.load();
            s.kills += c.kills.load();
            s.deaths += c.deaths.load();
            s.respawns += c.respawns.load();
            s.respawnsBad += c.respawnsBad.load();
            s.deadCorrections += c.deadCorrections.load();
            s.levelUps += c.levelUps.load();
            s.lootTakes += c.lootTakes.load();
            s.lootMissed += c.lootMissed.load();
            s.lootBad += c.lootBad.load();
            s.coinsGained += c.coinsGained.load();
            s.lootItems += c.lootItems.load();
        }
    return s;
}

std::vector<double> perBotBytes(const std::vector<std::unique_ptr<Worker>>& workers)
{
    std::vector<double> v;
    for (const auto& w : workers)
        for (const auto& b : w->bots)
            v.push_back(static_cast<double>(b->counters().bytesIn.load()));
    return v;
}

float percentile(std::vector<float>& v, double q)
{
    if (v.empty())
        return 0.0f;
    const auto k = static_cast<std::size_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

} // namespace

int runLoadTest(const Context& c)
{
    if (!c.terrain)
    {
        std::printf("FAIL scenario=load no terrain loaded (use --terrain <zone1.vterrain>)\n");
        return kExitFail;
    }
    const int n = c.opts.bots;
    const int threads = c.opts.threads > 0
                            ? c.opts.threads
                            : std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 8);
    Shared shared;
    shared.ctx = &c;
    shared.terrain = c.terrain.get();
    shared.start = Clock::now();

    const std::string prefix = "ld" + randomDigits(6) + "_";
    std::vector<std::unique_ptr<Worker>> workers;
    for (int t = 0; t < threads; ++t)
        workers.push_back(std::make_unique<Worker>());
    for (int i = 0; i < n; ++i)
    {
        auto& w = *workers[static_cast<std::size_t>(i % threads)];
        w.bots.push_back(std::make_unique<LoadBot>(w.io, shared, i, prefix + std::to_string(i)));
    }
    for (auto& wp : workers)
    {
        Worker& w = *wp;
        w.thread = std::thread([&w] {
            auto guard = asio::make_work_guard(w.io);
            while (!w.stop.load(std::memory_order_relaxed))
            {
                w.io.run_for(Ms(5));
                if (w.io.stopped())
                    w.io.restart();
                const auto now = Clock::now();
                for (auto& b : w.bots)
                    b->drive(now);
            }
            for (auto& b : w.bots)
                b->forceClose();
            guard.reset();
            w.io.run_for(Ms(500)); // flush the closes
        });
    }

    std::printf("load: %d bots on %d thread(s), one every %d ms\n", n, threads, c.opts.rampMs);
    std::fflush(stdout);
    // Ramp: until everyone is in the world (or nobody is left trying).
    const auto rampDeadline = shared.start + Ms(static_cast<long long>(n) * c.opts.rampMs) + std::chrono::seconds(180);
    int lastReported = -1;
    while (shared.inWorld.load() + shared.failed.load() < n && Clock::now() < rampDeadline)
    {
        std::this_thread::sleep_for(Ms(250));
        const int now = shared.inWorld.load();
        if (now / 100 != lastReported / 100)
        {
            std::printf("load: %d/%d in world (%.1f s)\n", now, n,
                        std::chrono::duration<double>(Clock::now() - shared.start).count());
            std::fflush(stdout);
            lastReported = now;
        }
    }
    const double rampS = std::chrono::duration<double>(Clock::now() - shared.start).count();
    const int entered = shared.inWorld.load();
    std::printf("load: %d/%d in world after %.1f s; measuring %d s\n", entered, n, rampS, c.opts.durationS);
    std::fflush(stdout);

    const auto before = snapshot(workers);
    const auto bytesBefore = perBotBytes(workers);
    const int failedBefore = shared.failed.load();
    shared.measuring = true;
    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(c.opts.durationS));
    shared.measuring = false;
    const double window = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto after = snapshot(workers);
    const auto bytesAfter = perBotBytes(workers);
    const int inWorldAtEnd = shared.inWorld.load();
    const int failedDuring = shared.failed.load() - failedBefore;

    shared.finishing = true;
    std::this_thread::sleep_for(Ms(1500)); // LOGOUTs go out and are answered with a close
    for (auto& w : workers)
        w->stop = true;
    for (auto& w : workers)
        w->thread.join();

    int disconnected = 0;
    std::vector<float> rtts;
    for (const auto& w : workers)
        for (const auto& b : w->bots)
        {
            disconnected += b->disconnected() ? 1 : 0;
            rtts.insert(rtts.end(), b->rtts().begin(), b->rtts().end());
        }
    std::vector<double> rates;
    for (std::size_t i = 0; i < bytesAfter.size(); ++i)
        rates.push_back((bytesAfter[i] - bytesBefore[i]) / window);
    std::sort(rates.begin(), rates.end());
    const double avgRate = rates.empty() ? 0.0 : std::accumulate(rates.begin(), rates.end(), 0.0) / static_cast<double>(rates.size());
    const double clients = std::max(1, entered); // bots in the world when the window started
    const auto per = [&](std::uint64_t a, std::uint64_t b) { return static_cast<double>(a - b) / window / clients; };

    std::printf("load: window %.1f s with %d bots in world\n", window, inWorldAtEnd);
    std::printf("load: received per client: %.0f B/s avg (min %.0f, median %.0f, max %.0f), %.1f packets/s, "
                "%.1f ENTITY_MOVE/s\n",
                avgRate, rates.empty() ? 0.0 : rates.front(), rates.empty() ? 0.0 : rates[rates.size() / 2],
                rates.empty() ? 0.0 : rates.back(), per(after.packetsIn, before.packetsIn),
                per(after.entityMoves, before.entityMoves));
    std::printf("load: total to clients %.2f MB/s, %.0f packets/s; MOVE_REQ sent %.1f/s per client\n",
                static_cast<double>(after.bytesIn - before.bytesIn) / window / 1e6,
                static_cast<double>(after.packetsIn - before.packetsIn) / window, per(after.movesSent, before.movesSent));
    std::printf("load: spawns %llu, despawns %llu in the window; corrections %llu in the window, %llu total\n",
                static_cast<unsigned long long>(after.spawns - before.spawns),
                static_cast<unsigned long long>(after.despawns - before.despawns),
                static_cast<unsigned long long>(after.corrections - before.corrections),
                static_cast<unsigned long long>(after.corrections));
    const std::size_t rttCount = rtts.size();
    const float p50 = percentile(rtts, 0.50), p90 = percentile(rtts, 0.90), p99 = percentile(rtts, 0.99),
                pmax = percentile(rtts, 1.0);
    std::printf("load: rtt ms p50 %.2f, p90 %.2f, p99 %.2f, max %.2f (%zu samples)\n", p50, p90, p99, pmax, rttCount);
    {
        const std::scoped_lock lock(shared.errorMutex);
        for (const auto& [why, count] : shared.errors)
            std::printf("load: %d x %s\n", count, why.c_str());
    }

    std::string problems;
    if (entered < n)
        problems += std::format(" only {}/{} entered the world;", entered, n);
    if (disconnected > 0 || failedDuring > 0)
        problems += std::format(" {} bot(s) disconnected;", std::max(disconnected, failedDuring));
    if (after.corrections > static_cast<std::uint64_t>(c.opts.maxCorrections))
        problems += std::format(" {} corrections (max {});", after.corrections, c.opts.maxCorrections);
    if (after.itemMovesBad > 0)
        problems += std::format(" {} bad item move answer(s);", after.itemMovesBad);
    if (after.respawnsBad > 0)
        problems += std::format(" {} refused RESPAWN_REQ(s);", after.respawnsBad);
    if (after.lootBad > 0)
        problems += std::format(" {} bad loot answer(s);", after.lootBad);
    std::printf("load: item moves %llu ok, %llu bad; ENTITY_EQUIP received %llu\n",
                static_cast<unsigned long long>(after.itemMovesOk), static_cast<unsigned long long>(after.itemMovesBad),
                static_cast<unsigned long long>(after.equips));
    int fighters = 0;
    for (int i = 0; i < n; ++i)
        fighters += (i % 5 == 2 && c.monsters) ? 1 : 0;
    std::printf("load: combat: %d fighter(s); %llu kills (XP_GAIN), %llu level-ups, %llu deaths, %llu respawns "
                "(%llu refused); COMBAT_EVENT received %llu (%.1f/s per client); %llu corrections of moves in "
                "flight at death (allowed)\n",
                fighters, static_cast<unsigned long long>(after.kills), static_cast<unsigned long long>(after.levelUps),
                static_cast<unsigned long long>(after.deaths), static_cast<unsigned long long>(after.respawns),
                static_cast<unsigned long long>(after.respawnsBad), static_cast<unsigned long long>(after.combatEvents),
                per(after.combatEvents, before.combatEvents), static_cast<unsigned long long>(after.deadCorrections));
    std::printf("load: loot: %llu takes, %llu Akçe, %llu bag slot update(s), %llu missed (container gone / out of "
                "range), %llu bad\n",
                static_cast<unsigned long long>(after.lootTakes), static_cast<unsigned long long>(after.coinsGained),
                static_cast<unsigned long long>(after.lootItems), static_cast<unsigned long long>(after.lootMissed),
                static_cast<unsigned long long>(after.lootBad));
    const std::string summary =
        std::format("bots={} in_world={} ramp_s={:.1f} window_s={:.1f} corrections={} disconnects={} "
                    "recv_Bps_per_client={:.0f} rtt_p50_ms={:.2f} rtt_p99_ms={:.2f} item_moves={} fighters={} "
                    "kills={} deaths={} respawns={} loot_takes={} coins={}",
                    n, inWorldAtEnd, rampS, window, after.corrections, disconnected, avgRate, p50, p99,
                    after.itemMovesOk, fighters, after.kills, after.deaths, after.respawns, after.lootTakes,
                    after.coinsGained);
    if (!problems.empty())
    {
        std::printf("FAIL scenario=load %s:%s\n", summary.c_str(), problems.c_str());
        return kExitFail;
    }
    std::printf("PASS scenario=load %s\n", summary.c_str());
    return kExitPass;
}

} // namespace vo::tools::botclient
