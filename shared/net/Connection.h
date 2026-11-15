// Connection: one framed (and, after the handshake, encrypted) TCP stream on asio, shared by client
// and server.
//
// - Async read loop through FrameDecoder; complete frames are decrypted (FrameCipher) and delivered
//   as (opcode, payload) to the message handler.
// - Serialized async write queue; more than kMaxOutboundQueueBytes queued -> immediate close
//   (CloseReason::OutboundOverflow).
// - close() is graceful: queued frames are flushed, the send side is shut down and the connection
//   lingers (discarding input) until the peer closes or the flush timeout expires. abort() closes
//   immediately. Either way the closed handler runs exactly once, after which both handlers are
//   released (so handlers may safely capture an owner that holds the Connection).
// - Bad frame lengths and AEAD failures close immediately without any error packet (Â§2.2).
//
// THREADING CONTRACT: a Connection is not thread-safe. All member functions must be called on the
// thread that runs the socket's io_context (i.e. from inside a handler, or via asio::post onto
// executor()), and that io_context must be driven by a single thread (run(), poll() or run_one()
// loops). Both handlers are always invoked from that io_context; the closed handler is posted, so
// it never runs re-entrantly inside close()/abort()/send().
#pragma once

#include "shared/net/CloseReason.h"
#include "shared/net/Crypto.h"
#include "shared/net/FrameCodec.h"
#include "shared/net/PacketWriter.h"

#include <asio/any_io_executor.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vo::net {

class Connection : public std::enable_shared_from_this<Connection>
{
public:
    // `opcode` is the raw u16 from the wire (may be unknown); `payload` is valid only during the call.
    using MessageHandler = std::function<void(std::uint16_t opcode, std::span<const std::uint8_t> payload)>;
    using ClosedHandler = std::function<void(CloseReason reason)>;

    // Takes ownership of a connected socket and enables TCP_NODELAY.
    [[nodiscard]] static std::shared_ptr<Connection> create(asio::ip::tcp::socket socket);

    struct PrivateTag
    {
        explicit PrivateTag() = default;
    };
    Connection(PrivateTag, asio::ip::tcp::socket socket); // use create()
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Starts the read loop. Call once.
    void start(MessageHandler onMessage, ClosedHandler onClosed);

    // Plaintext frame (handshake only). False if closing/closed or the frame is too large.
    bool sendPlain(std::uint16_t opcode, std::span<const std::uint8_t> payload);
    // Encrypted frame. False if encryption is not enabled, closing/closed, or too large.
    bool send(std::uint16_t opcode, std::span<const std::uint8_t> payload);

    template <class T>
    bool sendMessage(const T& msg)
    {
        PacketWriter w;
        msg.write(w);
        return w.ok() && send(static_cast<std::uint16_t>(T::kOpcode), w.data());
    }
    template <class T>
    bool sendPlainMessage(const T& msg)
    {
        PacketWriter w;
        msg.write(w);
        return w.ok() && sendPlain(static_cast<std::uint16_t>(T::kOpcode), w.data());
    }

    // Switches both directions to encrypted frames: every later send() is encrypted and every later
    // received frame (including bytes already buffered) is decoded as an encrypted frame.
    void enableEncryption(SessionKeys keys);
    [[nodiscard]] bool isEncrypted() const noexcept { return m_cipher.has_value(); }

    void close();
    void abort();

    // True until close()/abort() or a failure.
    [[nodiscard]] bool isOpen() const noexcept { return !m_closing && !m_closed; }
    [[nodiscard]] const std::string& remoteAddress() const noexcept { return m_remoteAddress; }
    [[nodiscard]] asio::any_io_executor executor() { return m_socket.get_executor(); }
    [[nodiscard]] std::size_t queuedBytes() const noexcept { return m_queuedBytes; }

    // Maximum time close() waits for the queue to flush and the peer to close. Default 2 s.
    void setCloseTimeout(std::chrono::milliseconds timeout) noexcept { m_closeTimeout = timeout; }

    // Testing hook: queues bytes verbatim (no framing, no encryption).
    bool sendRawForTesting(std::vector<std::uint8_t> bytes);

private:
    void doRead();
    void onRead(const std::error_code& ec, std::size_t bytes);
    bool enqueue(std::vector<std::uint8_t> frame);
    void doWrite();
    void onWrite(const std::error_code& ec);
    void finishGracefulClose();
    void terminate(CloseReason reason);

    asio::ip::tcp::socket m_socket;
    asio::steady_timer m_closeTimer;
    std::string m_remoteAddress;

    MessageHandler m_onMessage;
    ClosedHandler m_onClosed;

    std::array<std::uint8_t, 4096> m_readBuffer{};
    FrameDecoder m_decoder;
    FrameDecoder::Frame m_frame;
    std::vector<std::uint8_t> m_plain;
    std::optional<FrameCipher> m_cipher;

    std::deque<std::vector<std::uint8_t>> m_writeQueue;
    std::size_t m_queuedBytes = 0;
    bool m_writing = false;

    std::chrono::milliseconds m_closeTimeout{2000};
    bool m_started = false;
    bool m_closing = false;   // close() requested, or closed
    bool m_sentFin = false;   // graceful close: send side shut down, lingering for peer EOF
    bool m_closed = false;    // socket closed, closed handler posted
};

} // namespace vo::net
