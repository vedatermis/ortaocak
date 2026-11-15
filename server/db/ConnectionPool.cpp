#include "server/db/ConnectionPool.h"

#include "shared/core/Assert.h"
#include "shared/core/Log.h"

#include <pqxx/pqxx>

#include <algorithm>
#include <format>
#include <utility>

namespace vo::server::db {

namespace {

constexpr std::chrono::milliseconds kMinBackoff{100};
constexpr std::chrono::milliseconds kMaxBackoff{5000};

} // namespace

ConnectionPool::ConnectionPool(DbSettings settings) : m_settings(std::move(settings)) {}

ConnectionPool::~ConnectionPool()
{
    close();
}

void ConnectionPool::close()
{
    std::vector<Slot> idle;
    {
        const std::scoped_lock lock(m_mutex);
        m_closed = true;
        idle.swap(m_idle);
        m_open -= idle.size();
    }
    m_cv.notify_all();
    idle.clear(); // closes the connections outside the lock
}

ConnectionPtr ConnectionPool::connectNew(std::string& error)
{
    {
        const std::scoped_lock lock(m_mutex);
        const auto now = core::monoNow();
        if (now < m_nextConnectAt)
        {
            const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(m_nextConnectAt - now);
            error = std::format("database unavailable (next reconnect attempt in {} ms)", wait.count());
            return nullptr;
        }
    }
    std::string connectError;
    auto conn = openConnection(m_settings, m_settings.connectionString, connectError);
    const std::scoped_lock lock(m_mutex);
    if (!conn)
    {
        ++m_stats.connectsFailed;
        m_backoff = std::clamp(m_backoff * 2, kMinBackoff, kMaxBackoff);
        m_nextConnectAt = core::monoNow() + m_backoff;
        if (!m_down)
        {
            m_down = true;
            VO_LOG_WARN("db: cannot connect to {}: {} (retrying with backoff)",
                        describeConnectionString(m_settings.connectionString), connectError);
        }
        error = "cannot connect to the database: " + connectError;
        return nullptr;
    }
    ++m_stats.connectsOk;
    m_backoff = std::chrono::milliseconds(0);
    m_nextConnectAt = {};
    if (m_down)
    {
        m_down = false;
        VO_LOG_INFO("db: connection to {} restored", describeConnectionString(m_settings.connectionString));
    }
    return conn;
}

bool ConnectionPool::acquire(Slot& out, std::string& error)
{
    if (isThreadNoDb())
    {
        VO_ASSERT_MSG(false, "database access from a thread marked markThreadNoDb()");
        VO_LOG_ERROR("db: pool access from the network thread refused (programming error)");
        error = "database access from the network thread";
        return false;
    }
    {
        std::unique_lock lock(m_mutex);
        const bool ready = m_cv.wait_for(lock, m_settings.acquireTimeout, [this] {
            return m_closed || !m_idle.empty() || m_open < m_settings.poolSize;
        });
        if (m_closed)
        {
            error = "database pool closed";
            return false;
        }
        if (!ready)
        {
            error = std::format("no free database connection within {} ms", m_settings.acquireTimeout.count());
            return false;
        }
        if (!m_idle.empty())
        {
            out = std::move(m_idle.back()); // LIFO: keeps the warm connections busy
            m_idle.pop_back();
        }
        else
        {
            ++m_open; // reserve the slot, connect outside the lock
        }
    }

    if (out.conn)
    {
        const bool stale = core::monoNow() - out.lastUsed >= m_settings.healthCheckIdle;
        if (out.conn->is_open() && (!stale || pingConnection(*out.conn)))
            return true;
        VO_LOG_DEBUG("db: pooled connection failed its health check, reconnecting");
        out.conn.reset();
    }
    out.conn = connectNew(error);
    if (!out.conn)
    {
        {
            const std::scoped_lock lock(m_mutex);
            --m_open;
        }
        m_cv.notify_one();
        return false;
    }
    return true;
}

void ConnectionPool::release(Slot slot, bool broken)
{
    {
        const std::scoped_lock lock(m_mutex);
        if (broken || m_closed || !slot.conn || !slot.conn->is_open())
        {
            --m_open;
        }
        else
        {
            slot.lastUsed = core::monoNow();
            m_idle.push_back(std::move(slot));
            slot = {};
        }
    }
    m_cv.notify_one();
    // A dropped connection (if any) is closed here, outside the lock.
}

bool ConnectionPool::run(const Job& job, std::string& error)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        Slot slot;
        if (!acquire(slot, error))
            return false;
        try
        {
            job(*slot.conn);
            release(std::move(slot), false);
            return true;
        }
        catch (const pqxx::in_doubt_error& e)
        {
            // The commit may or may not have happened: never retry.
            error = describeDbError(e);
            release(std::move(slot), true);
            return false;
        }
        catch (const std::exception& e)
        {
            error = describeDbError(e);
            const bool lost = isConnectionError(e, slot.conn.get());
            release(std::move(slot), lost);
            if (!lost)
                return false;
            {
                // Whatever killed this connection (server restart, admin termination, network) has
                // most likely killed the idle ones too: health-check each before its next use.
                const std::scoped_lock lock(m_mutex);
                for (auto& idle : m_idle)
                    idle.lastUsed = {};
                if (attempt == 0)
                    ++m_stats.retries;
            }
            if (attempt == 0)
                VO_LOG_WARN("db: connection lost during a job ({}); retrying on a new connection", error);
        }
    }
    return false;
}

bool ConnectionPool::warmUp(std::string& error)
{
    return run([](pqxx::connection&) {}, error);
}

ConnectionPool::Stats ConnectionPool::stats() const
{
    const std::scoped_lock lock(m_mutex);
    Stats s = m_stats;
    s.open = m_open;
    s.idle = m_idle.size();
    return s;
}

std::vector<int> ConnectionPool::idleBackendPids() const
{
    const std::scoped_lock lock(m_mutex);
    std::vector<int> pids;
    for (const auto& s : m_idle)
    {
        if (s.conn && s.conn->is_open())
            pids.push_back(s.conn->backendpid());
    }
    return pids;
}

} // namespace vo::server::db
