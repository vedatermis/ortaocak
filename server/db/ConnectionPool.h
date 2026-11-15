// ConnectionPool: a small, thread-safe pool of configured PostgreSQL connections.
//
// - At most settings.poolSize connections; a caller waits up to settings.acquireTimeout for one.
// - Health check: a connection idle longer than settings.healthCheckIdle is pinged before use.
// - Transparent reconnect: a job that fails because its connection died (server restart,
//   pg_terminate_backend, network) is retried once on a fresh connection. Failed connects back
//   off exponentially (100 ms .. 5 s) so a dead database is not hammered; while backing off,
//   run() fails fast and the caller reports ServerError.
// - Blocking: run() must only be called from DB worker threads (or tools/tests), never from the
//   network thread (see markThreadNoDb()).
#pragma once

#include "server/db/Db.h"
#include "server/db/DbSettings.h"

#include "shared/core/Clock.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vo::server::db {

class ConnectionPool
{
public:
    explicit ConnectionPool(DbSettings settings);
    ~ConnectionPool();
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // A job runs its own transaction(s) on the connection. It may run twice (once more after a
    // connection failure), so it must not have external side effects before its commit.
    using Job = std::function<void(pqxx::connection&)>;

    // Runs `job` on a pooled connection. Exceptions from the job are caught: returns false and sets
    // `error` to a secret-free description (see describeDbError).
    [[nodiscard]] bool run(const Job& job, std::string& error);

    // Opens one connection now (startup reachability check). Never throws.
    [[nodiscard]] bool warmUp(std::string& error);

    // Closes idle connections; leased ones close when returned. Later run() calls fail.
    void close();

    struct Stats
    {
        std::size_t open = 0;
        std::size_t idle = 0;
        std::uint64_t connectsOk = 0;
        std::uint64_t connectsFailed = 0;
        std::uint64_t retries = 0; // jobs re-run after a lost connection
    };
    [[nodiscard]] Stats stats() const;

    // Backend PIDs of the idle connections (tests use this to terminate exactly our sessions).
    [[nodiscard]] std::vector<int> idleBackendPids() const;

    [[nodiscard]] const DbSettings& settings() const noexcept { return m_settings; }

private:
    struct Slot
    {
        ConnectionPtr conn;
        core::MonoTime lastUsed{};
    };

    [[nodiscard]] bool acquire(Slot& out, std::string& error);
    void release(Slot slot, bool broken);
    [[nodiscard]] ConnectionPtr connectNew(std::string& error);

    const DbSettings m_settings;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Slot> m_idle;
    std::size_t m_open = 0; // idle + leased + being opened
    bool m_closed = false;

    // Reconnect backoff (guarded by m_mutex).
    core::MonoTime m_nextConnectAt{};
    std::chrono::milliseconds m_backoff{0};
    bool m_down = false; // logged "unavailable", waiting to log "restored"

    Stats m_stats; // counters only (guarded by m_mutex)
};

} // namespace vo::server::db
