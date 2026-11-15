// Server: TCP listener that creates a Session per accepted connection, enforces the connection cap
// (net.max_connections -> HELLO_ACK ServerFull) and shuts everything down cleanly.
//
// Threading: construct, start() and shutdown() on the io_context thread (or before run()); the
// io_context must be run by a single thread. The account service must outlive the io_context's
// last handler (stop it after io.run() returns). The world must outlive the server (sessions post
// into it until they close); stop it after the sessions are gone and before the account service.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/net/ServerContext.h"
#include "server/world/World.h"
#include "shared/net/Crypto.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vo::server::net {

class Server
{
public:
    Server(asio::io_context& io, ServerSettings settings, vo::net::ServerIdentity identity,
           auth::IAccountService& accounts, world::World& world);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts accepting. False (with `error`) if the address/port cannot be used.
    bool start(std::string& error);

    // Stops accepting, sends ERROR(ServerShutdown) to every encrypted session and closes all
    // connections gracefully (each in-world session hands its player to the world for the final
    // despawn + position save). The io_context runs out of work once they are gone.
    void shutdown();

    // Actual listening port (useful with listenPort = 0).
    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] std::size_t sessionCount() const noexcept { return m_ctx->sessions.size(); }
    [[nodiscard]] const ServerContext& context() const noexcept { return *m_ctx; }

private:
    void doAccept();

    asio::io_context& m_io;
    asio::ip::tcp::acceptor m_acceptor;
    std::shared_ptr<ServerContext> m_ctx;
    std::uint16_t m_port = 0;
    bool m_accepting = false;
};

} // namespace vo::server::net
