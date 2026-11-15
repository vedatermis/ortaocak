#include "shared/net/ClientSession.h"

#include "shared/core/Log.h"
#include "shared/net/Connection.h"
#include "shared/net/Messages.h"
#include "shared/net/Opcodes.h"

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include <utility>

namespace vo::net {

std::string_view connectResultName(ConnectResult r) noexcept
{
    switch (r)
    {
    case ConnectResult::Ok: return "ok";
    case ConnectResult::ResolveFailed: return "resolve failed";
    case ConnectResult::ConnectFailed: return "connect failed";
    case ConnectResult::Timeout: return "handshake timeout";
    case ConnectResult::VersionMismatch: return "protocol version mismatch";
    case ConnectResult::ServerFull: return "server full";
    case ConnectResult::Maintenance: return "server in maintenance";
    case ConnectResult::VerificationFailed: return "server verification failed";
    case ConnectResult::ProtocolError: return "protocol error";
    case ConnectResult::Disconnected: return "disconnected during handshake";
    case ConnectResult::Cancelled: return "cancelled";
    }
    return "unknown";
}

struct ClientSession::Impl : std::enable_shared_from_this<Impl>
{
    explicit Impl(asio::io_context& ioContext) : io(ioContext), resolver(ioContext), socket(ioContext), timer(ioContext) {}

    asio::io_context& io;
    asio::ip::tcp::resolver resolver;
    asio::ip::tcp::socket socket; // until connected; then owned by `conn`
    asio::steady_timer timer;
    std::shared_ptr<Connection> conn;

    State state = State::Idle;
    ClientSettings settings;
    std::uint16_t version = kProtocolVersion;
    KxKeyPair kx;

    ConnectHandler onConnect;
    MessageHandler onMessage;
    ClosedHandler onClosed;

    void start()
    {
        state = State::Connecting;
        timer.expires_after(settings.handshakeTimeout);
        timer.async_wait([weak = weak_from_this()](const std::error_code& ec) {
            auto self = weak.lock();
            if (!self || ec)
                return;
            if (self->state == State::Connecting || self->state == State::Handshaking)
                self->finishConnect(ConnectResult::Timeout, "no HELLO_ACK within the handshake timeout");
        });

        resolver.async_resolve(settings.host, std::to_string(settings.port),
                               [weak = weak_from_this()](const std::error_code& ec,
                                                         asio::ip::tcp::resolver::results_type results) {
                                   auto self = weak.lock();
                                   if (!self || self->state != State::Connecting)
                                       return;
                                   if (ec)
                                   {
                                       self->finishConnect(ConnectResult::ResolveFailed, ec.message());
                                       return;
                                   }
                                   self->onResolved(results);
                               });
    }

    void onResolved(const asio::ip::tcp::resolver::results_type& results)
    {
        asio::async_connect(socket, results,
                            [weak = weak_from_this()](const std::error_code& ec, const asio::ip::tcp::endpoint&) {
                                auto self = weak.lock();
                                if (!self || self->state != State::Connecting)
                                    return;
                                if (ec)
                                {
                                    self->finishConnect(ConnectResult::ConnectFailed, ec.message());
                                    return;
                                }
                                self->onConnected();
                            });
    }

    void onConnected()
    {
        conn = Connection::create(std::move(socket));
        std::weak_ptr<Impl> weak = weak_from_this();
        conn->start(
            [weak](std::uint16_t opcode, std::span<const std::uint8_t> payload) {
                if (auto self = weak.lock())
                    self->onFrame(opcode, payload);
            },
            [weak](CloseReason reason) {
                if (auto self = weak.lock())
                    self->onConnectionClosed(reason);
            });

        kx = generateKxKeyPair();
        Hello hello;
        hello.protocolVersion = version;
        hello.clientBuild = settings.clientBuild;
        hello.clientKxPk = kx.publicKey;
        state = State::Handshaking;
        if (!conn->sendPlainMessage(hello))
            finishConnect(ConnectResult::ProtocolError, "cannot send HELLO");
    }

    void onFrame(std::uint16_t opcode, std::span<const std::uint8_t> payload)
    {
        if (state == State::Ready)
        {
            if (onMessage)
                onMessage(opcode, payload);
            return;
        }
        if (state != State::Handshaking)
            return;

        HelloAck ack;
        if (opcode != static_cast<std::uint16_t>(Opcode::HelloAck) || !decodeMessage(payload, ack))
        {
            finishConnect(ConnectResult::ProtocolError, "expected a well-formed HELLO_ACK");
            return;
        }
        switch (ack.result)
        {
        case HelloResult::Ok: break;
        case HelloResult::VersionMismatch:
            finishConnect(ConnectResult::VersionMismatch,
                          "server speaks protocol version " + std::to_string(ack.serverProtocolVersion));
            return;
        case HelloResult::ServerFull: finishConnect(ConnectResult::ServerFull, "server full"); return;
        case HelloResult::Maintenance: finishConnect(ConnectResult::Maintenance, "server in maintenance"); return;
        }
        if (ack.serverProtocolVersion != version ||
            !verifyHandshakeSignature(settings.serverPublicKey, kx.publicKey, ack.serverKxPk, ack.serverProtocolVersion,
                                      ack.signature))
        {
            finishConnect(ConnectResult::VerificationFailed, "HELLO_ACK signature does not match the pinned server key");
            return;
        }
        SessionKeys keys;
        if (!deriveClientSessionKeys(kx, ack.serverKxPk, keys))
        {
            finishConnect(ConnectResult::ProtocolError, "invalid server key exchange key");
            return;
        }
        conn->enableEncryption(std::move(keys));
        finishConnect(ConnectResult::Ok, {});
    }

    void onConnectionClosed(CloseReason reason)
    {
        if (state == State::Connecting || state == State::Handshaking)
        {
            finishConnect(ConnectResult::Disconnected, std::string(closeReasonName(reason)));
            return;
        }
        if (state == State::Ready)
        {
            state = State::Closed;
            conn.reset();
            auto handler = onClosed;
            if (handler)
                handler(reason);
        }
    }

    void finishConnect(ConnectResult result, const std::string& detail)
    {
        timer.cancel();
        kx.secretKey.wipe();
        if (result == ConnectResult::Ok)
        {
            state = State::Ready;
        }
        else
        {
            state = State::Closed;
            teardown();
        }
        auto handler = std::move(onConnect);
        onConnect = nullptr;
        if (handler)
            handler(result, detail);
    }

    void teardown()
    {
        timer.cancel();
        resolver.cancel();
        std::error_code ec;
        socket.close(ec);
        if (conn)
        {
            conn->abort();
            conn.reset();
        }
    }
};

ClientSession::ClientSession(asio::io_context& io) : m_impl(std::make_shared<Impl>(io)) {}

ClientSession::~ClientSession()
{
    m_impl->onConnect = nullptr;
    m_impl->onMessage = nullptr;
    m_impl->onClosed = nullptr;
    m_impl->state = State::Closed;
    m_impl->teardown();
}

void ClientSession::setMessageHandler(MessageHandler handler)
{
    m_impl->onMessage = std::move(handler);
}

void ClientSession::setClosedHandler(ClosedHandler handler)
{
    m_impl->onClosed = std::move(handler);
}

bool ClientSession::connect(const ClientSettings& settings, ConnectHandler onDone)
{
    if (m_impl->state != State::Idle)
        return false;
    m_impl->settings = settings;
    m_impl->version = settings.protocolVersion != 0 ? settings.protocolVersion : kProtocolVersion;
    m_impl->onConnect = std::move(onDone);
    m_impl->start();
    return true;
}

bool ClientSession::send(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (m_impl->state != State::Ready || !m_impl->conn)
        return false;
    return m_impl->conn->send(opcode, payload);
}

void ClientSession::close()
{
    switch (m_impl->state)
    {
    case State::Connecting:
    case State::Handshaking: m_impl->finishConnect(ConnectResult::Cancelled, "closed by the client"); break;
    case State::Ready:
        m_impl->state = State::Closed;
        if (m_impl->conn)
        {
            m_impl->conn->close();
            m_impl->conn.reset();
        }
        break;
    case State::Idle: m_impl->state = State::Closed; break;
    case State::Closed: break;
    }
}

ClientSession::State ClientSession::state() const noexcept
{
    return m_impl->state;
}

std::size_t ClientSession::poll()
{
    if (m_impl->io.stopped())
        m_impl->io.restart();
    return m_impl->io.poll();
}

bool ClientSession::sendRawForTesting(std::vector<std::uint8_t> bytes)
{
    if (!m_impl->conn)
        return false;
    return m_impl->conn->sendRawForTesting(std::move(bytes));
}

} // namespace vo::net
