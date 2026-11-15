// Load mode of the bot client: --bots N --duration S (docs/SERVER_WORLD.md §9).
//
// N asynchronous bots spread over a few threads (each thread one io_context) connect one after
// another (--ramp-ms apart), log in (auto-registration), create a character if they have none, enter
// the world and wander: straight legal walks/runs to random targets 5-30 m away (checked against the
// walk mask), stop, idle 0.5-3 s, repeat; MOVE_REQ at 10 Hz while moving and PING every second.
// Once every bot is in the world (or the ramp timed out) the tool measures for S seconds and reports
// connects, corrections, disconnects, received bytes/packets per client per second and the PING
// round-trip percentiles, then logs everybody out. PASS if all bots got in, nobody was disconnected
// and corrections <= --max-corrections (default 0).

#include "shared/game/Movement.h"
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
};

struct Snapshot
{
    std::uint64_t bytesIn = 0, packetsIn = 0, entityMoves = 0, spawns = 0, despawns = 0, corrections = 0,
                  movesSent = 0;
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
          m_nav(*shared.terrain)
    {
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
            m_pos = {r.x, r.z};
            m_yaw = r.yaw;
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
            ++m_counters.corrections;
            m_pos = {r.x, r.z};
            m_yaw = r.yaw;
            m_moving = false;
            m_idleUntil = now + Ms(300);
            return;
        }
        case vn::Opcode::EntityMove: ++m_counters.entityMoves; return;
        case vn::Opcode::EntitySpawn: ++m_counters.spawns; return;
        case vn::Opcode::EntityDespawn: ++m_counters.despawns; return;
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

    void wander(Clock::time_point now)
    {
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

        if (!m_moving)
        {
            if (now < m_idleUntil)
                return;
            const auto target = m_nav.randomStraightTarget(m_pos, m_rng, 5.0f, 30.0f);
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
    const double clients = std::max(1, inWorldAtEnd);
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
    const std::string summary =
        std::format("bots={} in_world={} ramp_s={:.1f} window_s={:.1f} corrections={} disconnects={} "
                    "recv_Bps_per_client={:.0f} rtt_p50_ms={:.2f} rtt_p99_ms={:.2f}",
                    n, inWorldAtEnd, rampS, window, after.corrections, disconnected, avgRate, p50, p99);
    if (!problems.empty())
    {
        std::printf("FAIL scenario=load %s:%s\n", summary.c_str(), problems.c_str());
        return kExitFail;
    }
    std::printf("PASS scenario=load %s\n", summary.c_str());
    return kExitPass;
}

} // namespace vo::tools::botclient
