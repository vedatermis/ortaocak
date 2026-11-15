#include "server/db/WorkerPool.h"

#include "server/db/Db.h"
#include "shared/core/Log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace vo::server::db {

WorkerPool::WorkerPool(std::size_t threads, std::string name)
    : m_name(std::move(name)), m_threadCount(std::max<std::size_t>(threads, 1))
{
    m_threads.reserve(m_threadCount);
    for (std::size_t i = 0; i < m_threadCount; ++i)
        m_threads.emplace_back([this] { loop(); });
}

WorkerPool::~WorkerPool()
{
    stop();
}

bool WorkerPool::submit(std::function<void()> job)
{
    {
        const std::scoped_lock lock(m_mutex);
        if (m_stopping)
            return false;
        m_queue.push_back(std::move(job));
    }
    m_cv.notify_one();
    return true;
}

void WorkerPool::stop()
{
    const std::scoped_lock joinLock(m_joinMutex);
    {
        const std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }
    m_cv.notify_all();
    for (auto& t : m_threads)
    {
        if (t.joinable())
            t.join();
    }
}

std::size_t WorkerPool::pending() const
{
    const std::scoped_lock lock(m_mutex);
    return m_queue.size();
}

void WorkerPool::loop()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty())
                return; // stopping and drained
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        try
        {
            job();
        }
        catch (const std::exception& e) // jobs must not throw; keep the worker alive if one does
        {
            VO_LOG_ERROR("{}: job threw: {}", m_name, describeDbError(e));
        }
    }
}

} // namespace vo::server::db
