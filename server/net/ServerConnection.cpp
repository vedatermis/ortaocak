#include "server/net/ServerConnection.h"

#include "shared/core/Log.h"
#include "shared/net/Opcodes.h"

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/write.hpp>

#include <utility>

namespace vo::server::net {

namespace vn = vo::net;

std::shared_ptr<ServerConnection> ServerConnection::create(asio::ip::tcp::socket socket)
{
    return std::make_shared<ServerConnection>(PrivateTag{}, std::move(socket));
}

ServerConnection::ServerConnection(PrivateTag, asio::ip::tcp::socket socket)
    : m_socket(std::move(socket)), m_closeTimer(m_socket.get_executor())
{
    std::error_code ec;
    m_socket.set_option(asio::ip::tcp::no_delay(true), ec);
    const auto ep = m_socket.remote_endpoint(ec);
    m_remoteAddress = ec ? std::string("?") : ep.address().to_string();
}

ServerConnection::~ServerConnection()
{
    std::error_code ec;
    m_socket.close(ec);
}

void ServerConnection::start(MessageHandler onMessage, ClosedHandler onClosed)
{
    if (m_started)
        return;
    m_started = true;
    m_onMessage = std::move(onMessage);
    m_onClosed = std::move(onClosed);
    if (m_closed)
        return;
    doRead();
}

// ---- Reading (same as vo::net::Connection) ----

void ServerConnection::doRead()
{
    m_socket.async_read_some(asio::buffer(m_readBuffer),
                             [self = shared_from_this()](const std::error_code& ec, std::size_t bytes) {
                                 self->onRead(ec, bytes);
                             });
}

void ServerConnection::onRead(const std::error_code& ec, std::size_t bytes)
{
    if (m_closed)
        return;
    if (ec)
    {
        if (m_closing)
            terminate(vn::CloseReason::LocalClose);
        else if (ec == asio::error::eof)
            terminate(vn::CloseReason::PeerClosed);
        else
            terminate(vn::CloseReason::NetworkError);
        return;
    }
    if (m_closing)
    {
        doRead(); // discard input until the peer closes or the close timer fires
        return;
    }
    m_decoder.feed(std::span<const std::uint8_t>(m_readBuffer.data(), bytes));
    // Replies produced while handling this chunk (PONG, results) leave in one write.
    beginBatch();
    while (!m_closing && !m_closed)
    {
        m_decoder.setMode(m_cipher ? vn::FrameMode::Encrypted : vn::FrameMode::Plain);
        const auto status = m_decoder.next(m_frame);
        if (status == vn::FrameDecoder::Status::NeedMore)
            break;
        if (status == vn::FrameDecoder::Status::Error)
        {
            terminate(vn::CloseReason::BadFrame);
            break;
        }
        std::span<const std::uint8_t> body = m_frame.body;
        if (m_cipher)
        {
            if (!m_cipher->open(m_frame.body, m_plain))
            {
                terminate(vn::CloseReason::DecryptFailed);
                break;
            }
            body = m_plain;
        }
        std::uint16_t opcode = 0;
        std::span<const std::uint8_t> payload;
        if (!vn::splitFrameBody(body, opcode, payload))
        {
            terminate(vn::CloseReason::BadFrame);
            break;
        }
        if (m_onMessage)
            m_onMessage(opcode, payload);
    }
    endBatch();
    if (!m_closed)
        doRead();
}

// ---- Writing (coalesced) ----

bool ServerConnection::sendPlain(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (!isOpen())
        return false;
    const std::size_t before = m_pending.size();
    if (!vn::encodePlainFrame(opcode, payload, m_pending))
    {
        m_pending.resize(before);
        VO_LOG_ERROR("net: plaintext frame for opcode 0x{:04X} too large ({} bytes)", opcode, payload.size());
        return false;
    }
    return afterAppend(before);
}

bool ServerConnection::send(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (!isOpen() || !m_cipher)
        return false;
    const std::size_t before = m_pending.size();
    if (!vn::encodeEncryptedFrame(*m_cipher, opcode, payload, m_pending))
    {
        m_pending.resize(before);
        VO_LOG_ERROR("net: encrypted frame for opcode 0x{:04X} too large ({} bytes)", opcode, payload.size());
        return false;
    }
    return afterAppend(before);
}

bool ServerConnection::afterAppend(std::size_t before)
{
    if (queuedBytes() > vn::kMaxOutboundQueueBytes)
    {
        m_pending.resize(before);
        VO_LOG_WARN("net: outbound queue to {} exceeded {} bytes, closing", m_remoteAddress,
                    vn::kMaxOutboundQueueBytes);
        terminate(vn::CloseReason::OutboundOverflow);
        return false;
    }
    maybeWrite();
    return true;
}

void ServerConnection::endBatch()
{
    if (m_batchDepth > 0)
        --m_batchDepth;
    maybeWrite();
}

void ServerConnection::maybeWrite()
{
    if (m_writing || m_closed || m_batchDepth > 0 || m_pending.empty())
        return;
    m_inflight.swap(m_pending); // m_pending is now empty (it was the old, cleared m_inflight)
    m_writing = true;
    ++m_writes;
    asio::async_write(m_socket, asio::buffer(m_inflight),
                      [self = shared_from_this()](const std::error_code& ec, std::size_t) { self->onWrite(ec); });
}

void ServerConnection::onWrite(const std::error_code& ec)
{
    m_writing = false;
    if (m_closed)
        return;
    if (ec)
    {
        terminate(vn::CloseReason::NetworkError);
        return;
    }
    m_inflight.clear(); // keeps its capacity for the next swap
    if (!m_pending.empty())
        maybeWrite();
    else if (m_closing)
        finishGracefulClose();
}

void ServerConnection::enableEncryption(vn::SessionKeys keys)
{
    m_cipher.emplace(std::move(keys));
}

// ---- Closing (same as vo::net::Connection) ----

void ServerConnection::close()
{
    if (m_closing || m_closed)
        return;
    m_closing = true;
    m_batchDepth = 0; // flush whatever is pending
    m_closeTimer.expires_after(m_closeTimeout);
    m_closeTimer.async_wait([self = shared_from_this()](const std::error_code& ec) {
        if (!ec)
            self->terminate(vn::CloseReason::LocalClose);
    });
    if (!m_started)
    {
        terminate(vn::CloseReason::LocalClose);
        return;
    }
    maybeWrite();
    if (!m_writing && m_pending.empty())
        finishGracefulClose();
}

void ServerConnection::finishGracefulClose()
{
    if (m_sentFin || m_closed)
        return;
    m_sentFin = true;
    std::error_code ec;
    m_socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    if (ec)
        terminate(vn::CloseReason::LocalClose);
}

void ServerConnection::abort()
{
    terminate(vn::CloseReason::LocalClose);
}

void ServerConnection::terminate(vn::CloseReason reason)
{
    if (m_closed)
        return;
    m_closed = true;
    m_closing = true;
    std::error_code ec;
    m_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    m_socket.close(ec);
    m_closeTimer.cancel();
    // An in-flight async_write still references m_inflight until its handler runs: keep it.
    m_pending.clear();
    VO_LOG_TRACE("net: connection {} closed ({})", m_remoteAddress, vn::closeReasonName(reason));
    asio::post(m_socket.get_executor(), [self = shared_from_this(), reason]() {
        auto onClosed = std::move(self->m_onClosed);
        self->m_onClosed = nullptr;
        self->m_onMessage = nullptr;
        if (onClosed)
            onClosed(reason);
    });
}

} // namespace vo::server::net
