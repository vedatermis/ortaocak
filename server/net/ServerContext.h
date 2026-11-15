// State shared by the listener and all sessions: settings, identity key, account service, world,
// login rate limiter, session registry (for the connection cap, duplicate-login kicks and shutdown).
// Owned via shared_ptr so it outlives every session. Sessions run on several network threads: the
// registry (sessions, online, counters, nextSessionId) is guarded by `registryMutex`; settings and
// identity are immutable after construction; `accounts`, `rateLimiter` and `world.post()` are
// thread-safe. `shuttingDown` is only touched on the listener's (main) thread.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/auth/LoginRateLimiter.h"
#include "server/world/World.h"
#include "shared/core/Clock.h"
#include "shared/net/Crypto.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace vo::server::net {

class Session;

struct ServerSettings
{
    std::string listenAddress = "0.0.0.0";
    std::uint16_t listenPort = 15001; // 0 = ephemeral (tests)
    std::size_t maxConnections = 2000;
    bool maintenance = false; // HELLO_ACK result 3 for everyone
    std::chrono::milliseconds handshakeTimeout{5000};
    std::chrono::milliseconds loginTimeout{30000};
    std::chrono::milliseconds idleTimeout{30000};
    std::uint32_t maxFailedLoginsPerMinute = 5;
    std::chrono::milliseconds failedLoginWindow{60000};
    // Connections accepted beyond maxConnections only to tell them "server full"; past this many
    // they are dropped right away.
    std::size_t overflowConnections = 64;
    // Network threads; each runs its own io_context with a share of the sessions (net.io_threads).
    std::size_t ioThreads = 1;
};

struct ServerContext
{
    ServerContext(ServerSettings s, vo::net::ServerIdentity id, auth::IAccountService& acc, world::World& w)
        : settings(std::move(s)), identity(std::move(id)), accounts(acc), world(w),
          rateLimiter(settings.maxFailedLoginsPerMinute, settings.failedLoginWindow), startTime(w.epoch())
    {
    }

    ServerSettings settings;
    vo::net::ServerIdentity identity;
    auth::IAccountService& accounts;
    world::World& world; // post() only: the world runs on its own thread
    auth::LoginRateLimiter rateLimiter;
    core::MonoTime startTime; // same epoch as the world (PONG, ENTER_WORLD_RES, ENTITY_MOVE)

    std::mutex registryMutex; // guards the five members below
    std::unordered_map<std::uint64_t, std::weak_ptr<Session>> sessions; // all live sessions by id
    std::unordered_map<auth::AccountId, std::weak_ptr<Session>> online;  // logged-in accounts
    std::size_t activeSessions = 0;   // sessions counted against maxConnections
    std::size_t overflowSessions = 0; // accepted only to answer "server full"
    std::uint64_t nextSessionId = 1;
    bool shuttingDown = false;

    // Milliseconds since server start (wraps after ~49 days, as the u32 wire fields do).
    [[nodiscard]] std::uint32_t serverTimeMs() const noexcept
    {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(core::monoNow() - startTime).count());
    }
};

} // namespace vo::server::net
