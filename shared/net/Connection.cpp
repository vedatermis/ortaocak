#include "shared/net/Connection.h"

#include "shared/core/Log.h"
#include "shared/net/Opcodes.h"

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/write.hpp>

#include <utility>

namespace vo::net {

std::shared_ptr<Connection> Connection::create(asio::ip::tcp::socket socket)
{
    return std::make_shared<Connection>(PrivateTag{}, std::move(socket));
}

Connection::Connection(PrivateTag, asio::ip::tcp::socket socket)
    : m_socket(std::move(socket)), m_closeTimer(m_socket.get_executor())
{
    std::error_code ec;
    m_socket.set_option(asio::ip::tcp::no_delay(true), ec);
    const auto ep = m_socket.remote_endpoint(ec);
    m_remoteAddress = ec ? std::string("?") : ep.address().to_string();
}

Connection::~Connection()
{
    std::error_code ec;
    m_socket.close(ec);
}

void Connection::start(MessageHandler onMessage, ClosedHandler onClosed)
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

// ---- Reading ----

void Connection::doRead()
{
    m_socket.async_read_some(asio::buffer(m_readBuffer),
                             [self = shared_from_this()](const std::error_code& ec, std::size_t bytes) {
                                 self->onRead(ec, bytes);
                             });
}

void Connection::onRead(const std::error_code& ec, std::size_t bytes)
{
    if (m_closed)
        return;
    if (ec)
    {
        if (m_closing)
            terminate(CloseReason::LocalClose); // peer finished while we were lingering
        else if (ec == asio::error::eof)
            terminate(CloseReason::PeerClosed);
        else
            terminate(CloseReason::NetworkError);
        return;
    }
    if (m_closing)
    {
        doRead(); // discard input until the peer closes or the close timer fires
        return;
    }

    m_decoder.feed(std::span<const std::uint8_t>(m_readBuffer.data(), bytes));
    // Replies produced while handling this chunk (PONG, results, ...) leave in one write.
    beginBatch();
    while (!m_closing && !m_closed)
    {
        m_decoder.setMode(m_cipher ? FrameMode::Encrypted : FrameMode::Plain);
        const auto status = m_decoder.next(m_frame);
        if (status == FrameDecoder::Status::NeedMore)
            break;
        if (status == FrameDecoder::Status::Error)
        {
            terminate(CloseReason::BadFrame);
            break;
        }
        std::span<const std::uint8_t> body = m_frame.body;
        if (m_cipher)
        {
            if (!m_cipher->open(m_frame.body, m_plain))
            {
                terminate(CloseReason::DecryptFailed);
                break;
            }
            body = m_plain;
        }
        std::uint16_t opcode = 0;
        std::span<const std::uint8_t> payload;
        if (!splitFrameBody(body, opcode, payload))
        {
            terminate(CloseReason::BadFrame);
            break;
        }
        ++m_framesReceived;
        if (m_onMessage)
            m_onMessage(opcode, payload);
    }
    endBatch();
    if (!m_closed)
        doRead();
}

// ---- Writing (coalesced) ----

bool Connection::sendPlain(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (!isOpen())
        return false;
    const std::size_t before = m_pending.size();
    if (!encodePlainFrame(opcode, payload, m_pending))
    {
        m_pending.resize(before);
        VO_LOG_ERROR("net: plaintext frame for opcode 0x{:04X} too large ({} bytes)", opcode, payload.size());
        return false;
    }
    return afterAppend(before);
}

bool Connection::send(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (!isOpen() || !m_cipher)
        return false;
    const std::size_t before = m_pending.size();
    // Seals into m_pending; the tx counter advances only if this succeeds.
    if (!encodeEncryptedFrame(*m_cipher, opcode, payload, m_pending))
    {
        m_pending.resize(before);
        VO_LOG_ERROR("net: encrypted frame for opcode 0x{:04X} too large ({} bytes)", opcode, payload.size());
        return false;
    }
    return afterAppend(before);
}

bool Connection::sendRawForTesting(std::vector<std::uint8_t> bytes)
{
    if (!isOpen())
        return false;
    const std::size_t before = m_pending.size();
    m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
    return afterAppend(before);
}

bool Connection::afterAppend(std::size_t before)
{
    if (queuedBytes() > kMaxOutboundQueueBytes)
    {
        m_pending.resize(before);
        VO_LOG_WARN("net: outbound queue to {} exceeded {} bytes, closing", m_remoteAddress, kMaxOutboundQueueBytes);
        terminate(CloseReason::OutboundOverflow);
        return false;
    }
    ++m_framesSent;
    maybeWrite();
    return true;
}

void Connection::endBatch()
{
    if (m_batchDepth > 0)
        --m_batchDepth;
    maybeWrite();
}

void Connection::maybeWrite()
{
    if (m_writing || m_closed || m_batchDepth > 0 || m_pending.empty())
        return;
    // m_inflight is idle here (no write references it) and was cleared by the last completion.
    m_inflight.swap(m_pending);
    m_writing = true;
    ++m_writes;
    asio::async_write(m_socket, asio::buffer(m_inflight),
                      [self = shared_from_this()](const std::error_code& ec, std::size_t) { self->onWrite(ec); });
}

void Connection::onWrite(const std::error_code& ec)
{
    m_writing = false;
    // From here on no operation references m_inflight.
    m_inflight.clear(); // keeps its capacity for the next swap
    if (m_closed)
        return;
    if (ec)
    {
        terminate(CloseReason::NetworkError);
        return;
    }
    if (!m_pending.empty())
        maybeWrite();
    else if (m_closing)
        finishGracefulClose();
}

void Connection::enableEncryption(SessionKeys keys)
{
    m_cipher.emplace(std::move(keys));
}

// ---- Closing ----

void Connection::close()
{
    if (m_closing || m_closed)
        return;
    m_closing = true;
    m_batchDepth = 0; // flush whatever is pending
    m_closeTimer.expires_after(m_closeTimeout);
    m_closeTimer.async_wait([self = shared_from_this()](const std::error_code& ec) {
        if (!ec)
            self->terminate(CloseReason::LocalClose);
    });
    if (!m_started)
    {
        terminate(CloseReason::LocalClose);
        return;
    }
    maybeWrite();
    if (!m_writing && m_pending.empty())
        finishGracefulClose();
}

void Connection::finishGracefulClose()
{
    if (m_sentFin || m_closed)
        return;
    m_sentFin = true;
    std::error_code ec;
    m_socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    if (ec)
        terminate(CloseReason::LocalClose);
    // Otherwise the read loop keeps running and terminates on the peer's EOF (or the close timer).
}

void Connection::abort()
{
    terminate(CloseReason::LocalClose);
}

void Connection::terminate(CloseReason reason)
{
    if (m_closed)
        return;
    m_closed = true;
    m_closing = true;
    std::error_code ec;
    m_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    m_socket.close(ec);
    m_closeTimer.cancel();
    // An in-flight async_write still references m_inflight until its handler runs: leave it alone
    // (onWrite clears it). Pending frames are never written now.
    m_pending.clear();
    m_pending.shrink_to_fit();
    if (!m_writing)
    {
        m_inflight.clear();
        m_inflight.shrink_to_fit();
    }
    VO_LOG_TRACE("net: connection {} closed ({})", m_remoteAddress, closeReasonName(reason));
    asio::post(m_socket.get_executor(), [self = shared_from_this(), reason]() {
        auto onClosed = std::move(self->m_onClosed);
        self->m_onClosed = nullptr;
        self->m_onMessage = nullptr;
        if (onClosed)
            onClosed(reason);
    });
}

} // namespace vo::net
