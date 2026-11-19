// Shared pieces of the bot client: options, scenario context, the synchronous Bot (one
// ClientSession driven by its own io_context) and the per-bot view of the world (ENTITY_* and
// POSITION_CORRECT are consumed into it instead of the inbox, so request/response steps keep working
// while the server streams world updates).
#pragma once

#include "shared/assets/Terrain.h"
#include "shared/game/Items.h"
#include "shared/game/Monsters.h"
#include "shared/game/ZoneData.h"
#include "shared/net/ClientSession.h"
#include "shared/net/Messages.h"
#include "shared/net/Opcodes.h"

#include <asio/io_context.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace vo::tools::botclient {

namespace vn = vo::net;
using Ms = std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

constexpr int kExitPass = 0;
constexpr int kExitFail = 1;
constexpr int kExitUsage = 2;

struct Options
{
    std::string scenario;
    std::optional<std::string> host;
    std::optional<std::uint16_t> port;
    std::optional<std::string> serverKeyHex;
    std::optional<std::string> configPath;
    std::optional<std::string> user;
    std::string password = "botpassword";
    int count = 5;
    std::string expect = "ok";
    Ms timeout{10000};
    bool help = false;
    // World scenarios
    std::optional<std::string> terrainPath; // zone 1 .vterrain
    int race = 1;
    std::string phase;     // persist: walk (default) | stay | check
    std::string stateFile; // persist: where stay/walk write and check reads the expected position
    // Load mode
    int bots = 0;
    int durationS = 30;
    int threads = 0; // 0 = auto
    int maxCorrections = 0;
    int rampMs = 5; // delay between bot starts
    bool zones = false; // load mode: commuters and homeland residents go through the gates (--zones)
};

struct Context
{
    vn::ClientSettings settings;
    Options opts;
    std::string user;
    std::shared_ptr<const assets::TerrainData> terrain; // loaded on demand by world scenarios
    std::shared_ptr<const game::ItemCatalog> items;     // gamedata/items.toml (items scenario, load mode)
    std::shared_ptr<const game::MonsterData> monsters;  // gamedata/monsters.toml (combat scenario, load mode)
    // Zones (docs/PROTOCOL_ZONES.md): the terrain of every zone of gamedata/zones.toml found next to
    // --terrain (<assets>/terrain/zone1/zone1.vterrain -> <assets>/<zone terrain>); zone 1 = `terrain`.
    std::map<std::uint16_t, std::shared_ptr<const assets::TerrainData>> zoneTerrains;
};

// Thrown by scenario steps; caught in runScenario and turned into a FAIL line.
struct Failure
{
    std::string reason;
};

[[noreturn]] inline void fail(std::string reason)
{
    throw Failure{std::move(reason)};
}

struct Received
{
    std::uint16_t opcode = 0;
    std::vector<std::uint8_t> payload;
    std::uint64_t index = 0; // arrival order across all messages of this bot
};

// What one bot knows about the world: the entities its client has spawned and what the server
// streamed about them, plus the corrections of its own position.
struct WorldView
{
    struct Remote
    {
        vn::EntitySpawn spawn;
        std::optional<vn::EntityMove> last;
        Clock::time_point spawnedAt;
    };
    struct MoveEvent
    {
        vn::EntityMove move;
        Clock::time_point at;
    };
    std::map<std::uint32_t, Remote> entities;
    std::map<std::uint32_t, std::vector<MoveEvent>> moves; // ENTITY_MOVE history per entity
    std::vector<vn::PositionCorrect> corrections;
    std::uint64_t spawns = 0;
    std::uint64_t despawns = 0;
    std::uint64_t entityMoves = 0;
    std::uint64_t firstSpawnIndex = 0; // arrival index of the first ENTITY_SPAWN (0 = none)
    std::vector<std::string> errors;   // protocol inconsistencies (duplicate spawn, unknown despawn, ...)

    // Items (docs/PROTOCOL_ITEMS.md): INVENTORY, ITEM_UPDATE and ENTITY_EQUIP are consumed here too.
    std::optional<vn::Inventory> inventory;
    std::uint64_t inventoryIndex = 0;     // arrival index of INVENTORY
    std::uint64_t lastItemUpdateIndex = 0; // arrival index of the last ITEM_UPDATE
    std::vector<vn::ItemUpdate> itemUpdates;
    std::vector<vn::EntityEquip> equips;

    // Combat (docs/PROTOCOL_COMBAT.md): every S->C combat packet except RESPAWN_RES (which stays in
    // the inbox as the answer to RESPAWN_REQ). ENTITY_STATS / ENTITY_STATE also update `entities`.
    std::vector<vn::CombatEvent> combatEvents;
    std::vector<vn::ActionFail> actionFails;
    std::vector<vn::CastStart> castStarts;
    std::vector<vn::CastEnd> castEnds;
    std::vector<vn::EntityStats> entityStats;
    std::vector<vn::SelfStats> selfStats;
    std::vector<vn::XpGain> xpGains;
    std::vector<vn::LevelUp> levelUps;
    std::vector<vn::EntityDeath> deaths;
    std::vector<vn::EntityState> states;

    // Loot (docs/PROTOCOL_LOOT.md): COINS, LOOT_CONTENTS and LOOT_CLOSED are consumed here;
    // LOOT_OPEN_RES / LOOT_TAKE_RES stay in the inbox as the answers to their requests.
    std::vector<vn::Coins> coins;
    std::uint64_t firstCoinsIndex = 0; // arrival index of the first COINS (0 = none)
    std::uint64_t lastCoinsIndex = 0;
    std::vector<vn::LootContents> lootContents;
    std::vector<vn::LootClosed> lootClosed;

    // Stats (docs/PROTOCOL_STATS.md): CHAR_STATS is consumed here; STAT_ALLOCATE_RES stays in the
    // inbox as the answer to STAT_ALLOCATE_REQ.
    std::vector<vn::CharStats> charStats;
    std::uint64_t firstCharStatsIndex = 0; // arrival index of the first CHAR_STATS (0 = none)

    // Skills (docs/PROTOCOL_SKILLS.md): SKILL_STATE, SKILL_BAR, SKILL_COOLDOWNS, EFFECT_*, SKILL_LEAP
    // and SKILL_USED are consumed here; SKILL_LEARN_RES / SKILL_BAR_SET_RES stay in the inbox.
    // `effects` is the effect list per entity as the client would keep it (cleared on death and
    // despawn); an EFFECT_REMOVE of an effect not in it is an error.
    std::vector<vn::SkillState> skillStates;
    std::uint64_t firstSkillStateIndex = 0;
    std::vector<vn::SkillBar> skillBars;
    std::vector<vn::SkillCooldowns> cooldowns;
    std::vector<vn::EffectApply> effectApplies;
    std::vector<vn::EffectRemove> effectRemoves;
    std::vector<vn::EffectList> effectLists;
    std::vector<vn::SkillUsed> skillsUsed;
    std::vector<vn::SkillLeap> leaps;
    std::map<std::uint32_t, std::vector<vn::EffectEntry>> effects;

    // Zones (docs/PROTOCOL_ZONES.md): ZONE_CHANGE_BEGIN (which also resets the view as a client
    // would) and ZONE_GATE_REFUSED are consumed here; ZONE_CHANGE stays in the inbox.
    std::vector<vn::ZoneChangeBegin> zoneBegins;
    std::uint64_t zoneBeginIndex = 0;
    std::vector<vn::ZoneGateRefused> gateRefusals;

    // Merchants and consumables (docs/PROTOCOL_SHOP.md): ITEM_COOLDOWN, ITEM_USED and SHOP_CLOSED are
    // consumed here; SHOP_OPEN_RES / SHOP_BUY_RES / SHOP_SELL_RES / ITEM_USE_RES stay in the inbox.
    std::vector<vn::ItemCooldown> itemCooldowns;
    std::uint64_t lastItemCooldownIndex = 0;
    std::vector<vn::ItemUsed> itemsUsed;
    std::vector<vn::ShopClosed> shopClosed;

    [[nodiscard]] bool hasEffect(std::uint32_t entity, std::uint16_t skill, std::uint8_t kind) const
    {
        const auto it = effects.find(entity);
        if (it == effects.end())
            return false;
        for (const auto& e : it->second)
            if (e.skillId == skill && e.kind == kind)
                return true;
        return false;
    }

    // Players / monsters currently spawned.
    [[nodiscard]] std::size_t count(std::uint8_t entityType) const
    {
        std::size_t n = 0;
        for (const auto& [id, r] : entities)
            n += r.spawn.entityType == entityType ? 1 : 0;
        return n;
    }
};

// Drives one ClientSession synchronously by running the io_context until a condition holds.
class Bot
{
public:
    explicit Bot(Ms timeout) : m_session(m_io), m_timeout(timeout)
    {
        m_session.setMessageHandler([this](std::uint16_t op, std::span<const std::uint8_t> payload) {
            const std::uint64_t index = ++m_received;
            if (m_trackWorld && consumeWorld(op, payload, index))
                return;
            m_inbox.push_back({op, std::vector<std::uint8_t>(payload.begin(), payload.end()), index});
        });
        m_session.setClosedHandler([this](vn::CloseReason r) {
            m_closed = true;
            m_closeReason = r;
        });
    }

    vn::ConnectResult connect(const vn::ClientSettings& settings, std::string& detail)
    {
        std::optional<vn::ConnectResult> result;
        m_session.connect(settings, [&](vn::ConnectResult r, const std::string& d) {
            result = r;
            detail = d;
        });
        if (!pump([&] { return result.has_value(); }, settings.handshakeTimeout + Ms(2000)))
            fail("connect callback never fired");
        return *result;
    }

    void connectOrFail(const vn::ClientSettings& settings)
    {
        std::string detail;
        const auto r = connect(settings, detail);
        if (r != vn::ConnectResult::Ok)
            fail(std::format("connect failed: {} ({})", vn::connectResultName(r), detail));
    }

    template <class T>
    void send(const T& msg)
    {
        if (!m_session.sendMessage(msg))
            fail(std::format("cannot send {}", vn::opcodeName(T::kOpcode)));
        m_lastSend = Clock::now();
    }

    bool sendRaw(std::vector<std::uint8_t> bytes) { return m_session.sendRawForTesting(std::move(bytes)); }

    // Drops the connection without LOGOUT (a client that crashed or lost its network).
    void dropConnection()
    {
        m_session.close();
        poll();
    }

    // Next message of type T; fails on timeout, close, ERROR or any other opcode.
    template <class T>
    T expect()
    {
        const Received m = next(std::format("waiting for {}", vn::opcodeName(T::kOpcode)));
        if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::Error))
        {
            vn::Error err;
            (void)vn::decodeMessage(m.payload, err);
            fail(std::format("got ERROR({} '{}') while waiting for {}", vn::errorCodeName(err.errorCode),
                             err.debugMessage, vn::opcodeName(T::kOpcode)));
        }
        if (m.opcode != static_cast<std::uint16_t>(T::kOpcode))
            fail(std::format("got {} while waiting for {}", vn::opcodeName(static_cast<vn::Opcode>(m.opcode)),
                             vn::opcodeName(T::kOpcode)));
        T out;
        if (!vn::decodeMessage(m.payload, out))
            fail(std::format("malformed {}", vn::opcodeName(T::kOpcode)));
        m_lastIndex = m.index;
        return out;
    }

    // Like expect(), but PONGs (answers to keepAlive()) before the message are dropped.
    template <class T>
    T expectSkippingPongs()
    {
        for (;;)
        {
            if (!pump([&] { return !m_inbox.empty() || m_closed; }, m_timeout))
                fail(std::format("timeout waiting for {}", vn::opcodeName(T::kOpcode)));
            if (!m_inbox.empty() && m_inbox.front().opcode == static_cast<std::uint16_t>(vn::Opcode::Pong))
            {
                m_inbox.pop_front();
                continue;
            }
            return expect<T>();
        }
    }

    Received next(const std::string& what)
    {
        if (!pump([&] { return !m_inbox.empty() || m_closed; }, m_timeout))
            fail("timeout " + what);
        if (m_inbox.empty())
            fail(std::format("connection closed ({}) {}", vn::closeReasonName(m_closeReason), what));
        Received m = std::move(m_inbox.front());
        m_inbox.pop_front();
        return m;
    }

    // Waits for the server to close the connection; fails if any message arrives first and
    // `allowMessages` is false.
    void expectClosed(bool allowMessages, Ms timeout)
    {
        if (!pump([&] { return m_closed || (!allowMessages && !m_inbox.empty()); }, timeout))
            fail("server did not close the connection");
        if (!m_closed)
            fail(std::format("unexpected {} before close",
                             vn::opcodeName(static_cast<vn::Opcode>(m_inbox.front().opcode))));
    }

    vn::LoginRes login(const std::string& user, const std::string& password)
    {
        send(vn::LoginReq{user, password});
        return expect<vn::LoginRes>();
    }

    [[nodiscard]] Ms timeout() const noexcept { return m_timeout; }

    // Runs ready handlers without blocking.
    void poll()
    {
        if (m_io.stopped())
            m_io.restart();
        m_io.poll();
    }

    // Keeps the io_context running for `d` (messages are collected meanwhile).
    void pumpFor(Ms d)
    {
        (void)pump([] { return false; }, d);
    }

    // Pumps until `done` holds or `timeout` passes. True if `done` held.
    bool pumpUntil(const std::function<bool()>& done, Ms timeout) { return pump(done, timeout); }

    // ---- World ----

    // From now on ENTITY_* and POSITION_CORRECT go into view() instead of the inbox.
    void trackWorld(bool on) { m_trackWorld = on; }
    [[nodiscard]] WorldView& view() noexcept { return m_view; }
    [[nodiscard]] bool closed() const noexcept { return m_closed; }
    // Arrival index of the last message returned by expect().
    [[nodiscard]] std::uint64_t lastIndex() const noexcept { return m_lastIndex; }

    // Sends a PING if nothing was sent for 5 s (keeps the server's 30 s idle timeout away). Never
    // fails: a closed session simply sends nothing.
    void keepAlive()
    {
        if (!m_closed && Clock::now() - m_lastSend >= std::chrono::seconds(5))
        {
            (void)m_session.sendMessage(vn::Ping{static_cast<std::uint32_t>(m_received)});
            m_lastSend = Clock::now();
        }
    }

    std::uint32_t nextSeq() noexcept { return ++m_seq; }

    // Drops PONGs (answers to keepAlive()) waiting in the inbox.
    void dropPongs()
    {
        poll();
        std::erase_if(m_inbox, [](const Received& r) { return r.opcode == static_cast<std::uint16_t>(vn::Opcode::Pong); });
    }

private:
    bool consumeWorld(std::uint16_t op, std::span<const std::uint8_t> payload, std::uint64_t index)
    {
        const auto now = Clock::now();
        switch (static_cast<vn::Opcode>(op))
        {
        case vn::Opcode::EntitySpawn:
        {
            vn::EntitySpawn s;
            if (!vn::decodeMessage(payload, s))
                m_view.errors.push_back("malformed ENTITY_SPAWN");
            else if (m_view.entities.contains(s.entityId))
                m_view.errors.push_back(std::format("duplicate ENTITY_SPAWN of entity {}", s.entityId));
            else
                m_view.entities[s.entityId] = {s, std::nullopt, now};
            ++m_view.spawns;
            if (m_view.firstSpawnIndex == 0)
                m_view.firstSpawnIndex = index;
            return true;
        }
        case vn::Opcode::EntityDespawn:
        {
            vn::EntityDespawn d;
            if (!vn::decodeMessage(payload, d))
                m_view.errors.push_back("malformed ENTITY_DESPAWN");
            else if (m_view.entities.erase(d.entityId) == 0)
                m_view.errors.push_back(std::format("ENTITY_DESPAWN of unknown entity {}", d.entityId));
            else
                m_view.effects.erase(d.entityId); // PROTOCOL_SKILLS.md §1.5: the client forgets them
            ++m_view.despawns;
            return true;
        }
        case vn::Opcode::EntityMove:
        {
            vn::EntityMove m;
            if (!vn::decodeMessage(payload, m))
                m_view.errors.push_back("malformed ENTITY_MOVE");
            else if (auto it = m_view.entities.find(m.entityId); it == m_view.entities.end())
                m_view.errors.push_back(std::format("ENTITY_MOVE of unknown entity {}", m.entityId));
            else
            {
                it->second.last = m;
                m_view.moves[m.entityId].push_back({m, now});
            }
            ++m_view.entityMoves;
            return true;
        }
        case vn::Opcode::PositionCorrect:
        {
            vn::PositionCorrect c;
            if (!vn::decodeMessage(payload, c))
                m_view.errors.push_back("malformed POSITION_CORRECT");
            else
                m_view.corrections.push_back(c);
            return true;
        }
        case vn::Opcode::Inventory:
        {
            vn::Inventory inv;
            if (!vn::decodeMessage(payload, inv))
                m_view.errors.push_back("malformed INVENTORY");
            else if (m_view.inventory)
                m_view.errors.push_back("second INVENTORY in one world session");
            else
            {
                m_view.inventory = std::move(inv);
                m_view.inventoryIndex = index;
            }
            return true;
        }
        case vn::Opcode::ItemUpdate:
        {
            vn::ItemUpdate u;
            if (!vn::decodeMessage(payload, u))
                m_view.errors.push_back("malformed ITEM_UPDATE");
            else
            {
                m_view.itemUpdates.push_back(std::move(u));
                m_view.lastItemUpdateIndex = index;
            }
            return true;
        }
        case vn::Opcode::EntityEquip:
        {
            vn::EntityEquip e;
            if (!vn::decodeMessage(payload, e))
                m_view.errors.push_back("malformed ENTITY_EQUIP");
            else
                m_view.equips.push_back(e);
            return true;
        }
        case vn::Opcode::Coins:
        {
            vn::Coins c;
            if (!vn::decodeMessage(payload, c))
                m_view.errors.push_back("malformed COINS");
            else
            {
                m_view.coins.push_back(c);
                if (m_view.firstCoinsIndex == 0)
                    m_view.firstCoinsIndex = index;
                m_view.lastCoinsIndex = index;
            }
            return true;
        }
        case vn::Opcode::CharStats:
        {
            vn::CharStats s;
            if (!vn::decodeMessage(payload, s))
                m_view.errors.push_back("malformed CHAR_STATS");
            else
            {
                m_view.charStats.push_back(s);
                if (m_view.firstCharStatsIndex == 0)
                    m_view.firstCharStatsIndex = index;
            }
            return true;
        }
        case vn::Opcode::SkillState:
            if (m_view.firstSkillStateIndex == 0)
                m_view.firstSkillStateIndex = index;
            return consumeInto(payload, m_view.skillStates, "SKILL_STATE");
        case vn::Opcode::SkillBar: return consumeInto(payload, m_view.skillBars, "SKILL_BAR");
        case vn::Opcode::SkillCooldowns: return consumeInto(payload, m_view.cooldowns, "SKILL_COOLDOWNS");
        case vn::Opcode::SkillUsed: return consumeInto(payload, m_view.skillsUsed, "SKILL_USED");
        case vn::Opcode::SkillLeap: return consumeInto(payload, m_view.leaps, "SKILL_LEAP");
        case vn::Opcode::EffectList:
        {
            vn::EffectList l;
            if (!vn::decodeMessage(payload, l))
                m_view.errors.push_back("malformed EFFECT_LIST");
            else
            {
                m_view.effects[l.entityId] = l.effects;
                m_view.effectLists.push_back(std::move(l));
            }
            return true;
        }
        case vn::Opcode::EffectApply:
        {
            vn::EffectApply a;
            if (!vn::decodeMessage(payload, a))
                m_view.errors.push_back("malformed EFFECT_APPLY");
            else
            {
                auto& list = m_view.effects[a.entityId];
                std::erase_if(list, [&](const vn::EffectEntry& e) {
                    return e.skillId == a.effect.skillId && e.kind == a.effect.kind;
                });
                list.push_back(a.effect);
                m_view.effectApplies.push_back(a);
            }
            return true;
        }
        case vn::Opcode::EffectRemove:
        {
            vn::EffectRemove r;
            if (!vn::decodeMessage(payload, r))
                m_view.errors.push_back("malformed EFFECT_REMOVE");
            else
            {
                auto& list = m_view.effects[r.entityId];
                const auto before = list.size();
                std::erase_if(list, [&](const vn::EffectEntry& e) { return e.skillId == r.skillId && e.kind == r.kind; });
                if (list.size() == before)
                    m_view.errors.push_back(std::format("EFFECT_REMOVE of an unknown effect ({}, {}) on entity {}",
                                                        r.skillId, r.kind, r.entityId));
                m_view.effectRemoves.push_back(r);
            }
            return true;
        }
        case vn::Opcode::ZoneChangeBegin:
        {
            // PROTOCOL_ZONES.md §3.3: like a client, forget everything of the old zone (no despawns
            // come) and expect the entry packets again.
            vn::ZoneChangeBegin b;
            if (!vn::decodeMessage(payload, b))
                m_view.errors.push_back("malformed ZONE_CHANGE_BEGIN");
            else
                m_view.zoneBegins.push_back(b);
            m_view.zoneBeginIndex = index;
            m_view.entities.clear();
            m_view.effects.clear();
            m_view.inventory.reset();
            m_view.inventoryIndex = 0;
            m_view.firstSpawnIndex = 0;
            m_view.firstCoinsIndex = 0;
            m_view.firstCharStatsIndex = 0;
            m_view.firstSkillStateIndex = 0;
            m_view.coins.clear();
            m_view.charStats.clear();
            m_view.skillStates.clear();
            m_view.skillBars.clear();
            m_view.cooldowns.clear();
            m_view.effectLists.clear();
            return true;
        }
        case vn::Opcode::ZoneGateRefused: return consumeInto(payload, m_view.gateRefusals, "ZONE_GATE_REFUSED");
        case vn::Opcode::ItemCooldown:
            m_view.lastItemCooldownIndex = index;
            return consumeInto(payload, m_view.itemCooldowns, "ITEM_COOLDOWN");
        case vn::Opcode::ItemUsed: return consumeInto(payload, m_view.itemsUsed, "ITEM_USED");
        case vn::Opcode::ShopClosed: return consumeInto(payload, m_view.shopClosed, "SHOP_CLOSED");
        case vn::Opcode::LootContents: return consumeInto(payload, m_view.lootContents, "LOOT_CONTENTS");
        case vn::Opcode::LootClosed: return consumeInto(payload, m_view.lootClosed, "LOOT_CLOSED");
        case vn::Opcode::CombatEvent: return consumeInto(payload, m_view.combatEvents, "COMBAT_EVENT");
        case vn::Opcode::ActionFail: return consumeInto(payload, m_view.actionFails, "ACTION_FAIL");
        case vn::Opcode::CastStart: return consumeInto(payload, m_view.castStarts, "CAST_START");
        case vn::Opcode::CastEnd: return consumeInto(payload, m_view.castEnds, "CAST_END");
        case vn::Opcode::SelfStats: return consumeInto(payload, m_view.selfStats, "SELF_STATS");
        case vn::Opcode::XpGain: return consumeInto(payload, m_view.xpGains, "XP_GAIN");
        case vn::Opcode::LevelUp: return consumeInto(payload, m_view.levelUps, "LEVEL_UP");
        case vn::Opcode::EntityDeath:
        {
            vn::EntityDeath dd;
            if (!vn::decodeMessage(payload, dd))
                m_view.errors.push_back("malformed ENTITY_DEATH");
            else
            {
                m_view.effects.erase(dd.entityId); // death clears every effect silently
                m_view.deaths.push_back(dd);
            }
            return true;
        }
        case vn::Opcode::EntityStats:
        {
            vn::EntityStats s;
            if (!vn::decodeMessage(payload, s))
                m_view.errors.push_back("malformed ENTITY_STATS");
            else
            {
                m_view.entityStats.push_back(s);
                if (auto it = m_view.entities.find(s.entityId); it != m_view.entities.end())
                {
                    it->second.spawn.hp = s.hp;
                    it->second.spawn.maxHp = s.maxHp;
                    it->second.spawn.level = s.level;
                }
            }
            return true;
        }
        case vn::Opcode::EntityState:
        {
            vn::EntityState s;
            if (!vn::decodeMessage(payload, s))
                m_view.errors.push_back("malformed ENTITY_STATE");
            else
            {
                m_view.states.push_back(s);
                if (auto it = m_view.entities.find(s.entityId); it != m_view.entities.end())
                    it->second.spawn.state = s.state;
            }
            return true;
        }
        default: return false;
        }
    }

    template <class T>
    bool consumeInto(std::span<const std::uint8_t> payload, std::vector<T>& into, const char* name)
    {
        T msg;
        if (!vn::decodeMessage(payload, msg))
            m_view.errors.push_back(std::string("malformed ") + name);
        else
            into.push_back(msg);
        return true;
    }

    bool pump(const std::function<bool()>& done, Ms timeout)
    {
        const auto deadline = Clock::now() + timeout;
        while (!done())
        {
            const auto now = Clock::now();
            if (now >= deadline)
                return false;
            if (m_io.stopped())
                m_io.restart();
            if (m_io.run_one_for(std::chrono::duration_cast<Ms>(deadline - now)) == 0)
                std::this_thread::sleep_for(Ms(2)); // no work at all: avoid spinning
        }
        return true;
    }

    asio::io_context m_io;
    vn::ClientSession m_session;
    Ms m_timeout;
    std::deque<Received> m_inbox;
    bool m_closed = false;
    vn::CloseReason m_closeReason = vn::CloseReason::LocalClose;
    bool m_trackWorld = false;
    WorldView m_view;
    std::uint64_t m_received = 0;
    std::uint64_t m_lastIndex = 0;
    std::uint32_t m_seq = 0;
    Clock::time_point m_lastSend = Clock::now();
};

std::string randomDigits(int n);
std::string randomCharacterName();
std::uint32_t nowMs();

// World scenarios (WorldScenarios.cpp) and load mode (LoadTest.cpp).
std::string scenarioWorld(const Context& c);
std::string scenarioSpeedhack(const Context& c);
std::string scenarioWallhack(const Context& c);
std::string scenarioFloodMove(const Context& c);
std::string scenarioLeaveRace(const Context& c);
std::string scenarioDupWorld(const Context& c);
std::string scenarioAoi(const Context& c);
std::string scenarioPersist(const Context& c);
std::string scenarioItems(const Context& c);
std::string scenarioCombat(const Context& c);
std::string scenarioLoot(const Context& c);
std::string scenarioStats(const Context& c);
std::string scenarioSkills(const Context& c);
std::string scenarioZones(const Context& c);
std::string scenarioShop(const Context& c);
std::string scenarioShopErrors(const Context& c);
std::string scenarioUpgrade(const Context& c); // M16: the smith's Yükselt (world.testing_enter_coins, testing_upgrade_outcome)
std::string scenarioHunt(const Context& c); // M15: a slot hunt to level 11+ (world.testing_xp_percent)
std::string scenarioZonesRace(const Context& c);
int runLoadTest(const Context& c);

// Applies the entries of INVENTORY / ITEM_UPDATE to a local inventory. False for an invalid place.
inline bool applyItemEntries(game::Inventory& inv, const std::vector<vn::ItemEntry>& entries)
{
    for (const auto& e : entries)
    {
        const game::ItemPlace place{e.container, e.slot};
        if (!game::isValidPlace(place))
            return false;
        inv.at(place) = e.itemId == 0 ? game::ItemStack{} : game::ItemStack{e.itemId, e.templateId, e.quantity, e.upgrade};
    }
    return true;
}

} // namespace vo::tools::botclient
