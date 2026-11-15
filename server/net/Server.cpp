#include "server/net/Server.h"

#include "server/db/Db.h"
#include "server/net/Session.h"
#include "shared/core/Log.h"
#include "server/net/ServerConnection.h"

#include <asio/ip/address.hpp>
#include <asio/post.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace vo::server::net {

Server::Server(asio::io_context& io, ServerSettings settings, vo::net::ServerIdentity identity,
               auth::IAccountService& accounts, world::World& world)
    : m_io(io), m_acceptor(io),
      m_ctx(std::make_shared<ServerContext>(std::move(settings), std::move(identity), accounts, world))
{
}

Server::~Server()
{
    m_ctx->shuttingDown = true;
    std::error_code ec;
    m_acceptor.close(ec);
    stopNetwork(std::chrono::milliseconds(0));
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

    const std::size_t threads = std::clamp<std::size_t>(st.ioThreads, 1, 64);
    for (std::size_t i = 0; i < threads; ++i)
    {
        auto t = std::make_unique<NetThread>();
        NetThread* raw = t.get();
        raw->thread = std::thread([raw] {
            db::markThreadNoDb(); // network threads never block on the database
            raw->io.run();
        });
        m_net.push_back(std::move(t));
    }
    m_accepting = true;
    VO_LOG_INFO("listening on {}:{} (max {} connections, {} network thread(s))", st.listenAddress, m_port,
                st.maxConnections, threads);
    doAccept();
    return true;
}

void Server::doAccept()
{
    // The socket is created on the next network thread's io_context; the session lives there.
    NetThread& target = *m_net[m_nextNet];
    m_nextNet = (m_nextNet + 1) % m_net.size();
    m_acceptor.async_accept(target.io, [this, ctx = m_ctx](const std::error_code& ec, asio::ip::tcp::socket socket) {
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

        std::shared_ptr<Session> session;
        {
            const std::scoped_lock lock(ctx->registryMutex);
            const bool overCapacity = ctx->activeSessions >= ctx->settings.maxConnections;
            if (!overCapacity || ctx->overflowSessions < ctx->settings.overflowConnections)
            {
                auto conn = ServerConnection::create(std::move(socket));
                const std::uint64_t id = ctx->nextSessionId++;
                session = Session::create(ctx, std::move(conn), id, overCapacity);
                if (overCapacity)
                    ++ctx->overflowSessions;
                else
                    ++ctx->activeSessions;
                ctx->sessions[id] = session;
            }
        }
        if (session)
            asio::post(session->executor(), [session] { session->start(); });
        else
        {
            std::error_code ignored;
            socket.close(ignored); // far over capacity: do not even answer
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
    {
        const std::scoped_lock lock(m_ctx->registryMutex);
        for (const auto& [id, weak] : m_ctx->sessions)
        {
            if (auto s = weak.lock())
                sessions.push_back(std::move(s));
        }
    }
    VO_LOG_INFO("shutting down: closing {} session(s)", sessions.size());
    for (const auto& s : sessions)
        s->kickAsync(vo::net::ErrorCode::ServerShutdown, "server shutting down");
}

std::size_t Server::sessionCount() const
{
    const std::scoped_lock lock(m_ctx->registryMutex);
    return m_ctx->sessions.size();
}

bool Server::waitForSessions(std::chrono::milliseconds wait) const
{
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (sessionCount() > 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return sessionCount() == 0;
}

void Server::stopNetwork(std::chrono::milliseconds wait)
{
    if (m_net.empty())
        return;
    (void)waitForSessions(wait);
    if (const auto left = sessionCount(); left > 0)
        VO_LOG_WARN("network: {} session(s) still open, stopping the network threads anyway", left);
    for (auto& t : m_net)
    {
        t->guard.reset();
        t->io.stop();
    }
    for (auto& t : m_net)
    {
        if (t->thread.joinable())
            t->thread.join();
    }
    m_net.clear(); // destroys the io_contexts with whatever handlers were still queued
}

} // namespace vo::server::net
