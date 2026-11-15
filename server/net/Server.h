// Server: TCP listener that creates a Session per accepted connection, enforces the connection cap
// (net.max_connections -> HELLO_ACK ServerFull) and shuts everything down cleanly.
//
// Threading: the listener runs on the io_context passed in (construct, start() and shutdown() on
// its thread, or before run()). Sessions run on `settings.ioThreads` network threads owned by the
// Server, each driving its own io_context (Connection requires a single-threaded io_context); new
// connections are assigned round-robin. The account service must outlive the network threads'
// last handler (stop it after stopNetwork()). The world must outlive the server (sessions post into
// it until they close); stop it after the sessions are gone and before the account service.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/net/ServerContext.h"
#include "server/world/World.h"
#include "shared/net/Crypto.h"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vo::server::net {

class Server
{
public:
    Server(asio::io_context& io, ServerSettings settings, vo::net::ServerIdentity identity,
           auth::IAccountService& accounts, world::World& world);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Starts the network threads, binds and starts accepting. False (with `error`) if the
    // address/port cannot be used.
    bool start(std::string& error);

    // Stops accepting, sends ERROR(ServerShutdown) to every encrypted session and closes all
    // connections gracefully (each in-world session hands its player to the world for the final
    // despawn + position save).
    void shutdown();

    // True once every session is gone (waits at most `wait`). Thread-safe.
    [[nodiscard]] bool waitForSessions(std::chrono::milliseconds wait) const;
    // Waits (bounded) until every session is gone, then stops and joins the network threads; handlers
    // still queued are dropped. Idempotent; also run by the destructor. Call it after the account
    // service is stopped (its callbacks post onto these io_contexts), never from a network thread.
    void stopNetwork(std::chrono::milliseconds wait = std::chrono::milliseconds(5000));

    // Actual listening port (useful with listenPort = 0).
    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    // Thread-safe.
    [[nodiscard]] std::size_t sessionCount() const;
    [[nodiscard]] const ServerContext& context() const noexcept { return *m_ctx; }

private:
    struct NetThread
    {
        asio::io_context io{1};
        asio::executor_work_guard<asio::io_context::executor_type> guard{asio::make_work_guard(io)};
        std::thread thread;
    };

    void doAccept();

    asio::io_context& m_io;
    asio::ip::tcp::acceptor m_acceptor;
    std::shared_ptr<ServerContext> m_ctx;
    std::vector<std::unique_ptr<NetThread>> m_net;
    std::size_t m_nextNet = 0;
    std::uint16_t m_port = 0;
    bool m_accepting = false;
};

} // namespace vo::server::net
