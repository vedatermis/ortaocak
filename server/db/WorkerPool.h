// WorkerPool: fixed set of threads executing DB jobs (and the Argon2 hashing that goes with them)
// off the network and world threads. FIFO queue; stop() drains every queued job, then joins.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vo::server::db {

class WorkerPool
{
public:
    WorkerPool(std::size_t threads, std::string name);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Queues a job. Returns false (job not queued) once stop() has begun. Jobs must not throw.
    [[nodiscard]] bool submit(std::function<void()> job);

    // Runs everything already queued, then joins the threads. Idempotent.
    void stop();

    [[nodiscard]] std::size_t threadCount() const noexcept { return m_threadCount; }
    [[nodiscard]] std::size_t pending() const;

private:
    void loop();

    const std::string m_name;
    const std::size_t m_threadCount;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    bool m_stopping = false;
    std::vector<std::thread> m_threads;
    std::mutex m_joinMutex; // serialises concurrent stop() calls
};

} // namespace vo::server::db
