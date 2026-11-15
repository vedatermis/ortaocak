// Shared pieces of the bot client: options, scenario context, the synchronous Bot (one
// ClientSession driven by its own io_context) and the per-bot view of the world (ENTITY_* and
// POSITION_CORRECT are consumed into it instead of the inbox, so request/response steps keep working
// while the server streams world updates).
#pragma once

#include "shared/assets/Terrain.h"
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
};

struct Context
{
    vn::ClientSettings settings;
    Options opts;
    std::string user;
    std::shared_ptr<const assets::TerrainData> terrain; // loaded on demand by world scenarios
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
        default: return false;
        }
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
std::string scenarioAoi(const Context& c);
std::string scenarioPersist(const Context& c);
int runLoadTest(const Context& c);

} // namespace vo::tools::botclient
