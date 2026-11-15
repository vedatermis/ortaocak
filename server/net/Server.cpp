#include "server/net/Server.h"

#include "server/net/Session.h"
#include "shared/core/Log.h"
#include "shared/net/Connection.h"

#include <asio/ip/address.hpp>

#include <utility>
#include <vector>

namespace vo::server::net {

Server::Server(asio::io_context& io, ServerSettings settings, vo::net::ServerIdentity identity,
               auth::IAccountService& accounts)
    : m_io(io), m_acceptor(io),
      m_ctx(std::make_shared<ServerContext>(std::move(settings), std::move(identity), accounts))
{
}

Server::~Server()
{
    m_ctx->shuttingDown = true;
    std::error_code ec;
    m_acceptor.close(ec);
}

bool Server::start(std::string& error)
{
    const auto& st = m_ctx->settings;
    std::error_code ec;
    const auto address = asio::ip::make_address(st.listenAddress, ec);
    if (ec)
    {
        error = "invalid listen address '" + st.listenAddress + "': " + ec.message();
        return false;
    }
    const asio::ip::tcp::endpoint endpoint(address, st.listenPort);
    m_acceptor.open(endpoint.protocol(), ec);
    if (!ec)
    {
        // No SO_REUSEADDR: on Windows it would let a second server bind the same port.
        m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(false), ec);
    }
    if (!ec)
        m_acceptor.bind(endpoint, ec);
    if (!ec)
        m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec)
    {
        error = "cannot listen on " + st.listenAddress + ":" + std::to_string(st.listenPort) + ": " + ec.message();
        std::error_code ignored;
        m_acceptor.close(ignored);
        return false;
    }
    m_port = m_acceptor.local_endpoint(ec).port();
    m_accepting = true;
    VO_LOG_INFO("listening on {}:{} (max {} connections)", st.listenAddress, m_port, st.maxConnections);
    doAccept();
    return true;
}

void Server::doAccept()
{
    m_acceptor.async_accept([this, ctx = m_ctx](const std::error_code& ec, asio::ip::tcp::socket socket) {
        if (ctx->shuttingDown || !m_accepting) // shuttingDown is also set by ~Server: check it first
            return;
        if (ec)
        {
            if (ec != asio::error::operation_aborted)
                VO_LOG_WARN("accept failed: {}", ec.message());
            if (m_acceptor.is_open())
                doAccept();
            return;
        }

        const bool overCapacity = ctx->activeSessions >= ctx->settings.maxConnections;
        if (overCapacity && ctx->overflowSessions >= ctx->settings.overflowConnections)
        {
            std::error_code ignored;
            socket.close(ignored); // far over capacity: do not even answer
        }
        else
        {
            auto conn = vo::net::Connection::create(std::move(socket));
            const std::uint64_t id = ctx->nextSessionId++;
            auto session = Session::create(ctx, std::move(conn), id, overCapacity);
            if (overCapacity)
                ++ctx->overflowSessions;
            else
                ++ctx->activeSessions;
            ctx->sessions[id] = session;
            session->start();
        }
        doAccept();
    });
}

void Server::shutdown()
{
    if (m_ctx->shuttingDown)
        return;
    m_ctx->shuttingDown = true;
    m_accepting = false;
    std::error_code ec;
    m_acceptor.close(ec);

    std::vector<std::shared_ptr<Session>> sessions;
    for (const auto& [id, weak] : m_ctx->sessions)
    {
        if (auto s = weak.lock())
            sessions.push_back(std::move(s));
    }
    VO_LOG_INFO("shutting down: closing {} session(s)", sessions.size());
    for (const auto& s : sessions)
        s->kick(vo::net::ErrorCode::ServerShutdown, "server shutting down");
}

} // namespace vo::server::net
