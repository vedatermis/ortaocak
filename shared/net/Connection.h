// Connection: one framed (and, after the handshake, encrypted) TCP stream on asio, shared by client
// and server (server Session, client ClientSession, botclient).
//
// - Async read loop through FrameDecoder; complete frames are decrypted (FrameCipher) and delivered
//   as (opcode, payload) to the message handler.
// - Coalesced writes: every send() encodes (and encrypts) its frame at once, in call order, into
//   one pending buffer; one async_write is in flight at a time and carries *everything* queued when
//   it starts. Frames sent while a write is in flight join the next one; frames sent between
//   beginBatch() and endBatch() (or while the message handler runs inside one read) go out together
//   when the batch ends. Writing one frame per async_write and waiting for each completion capped a
//   connection at ~1000 frames/s on loopback (docs/SERVER_WORLD.md §9).
// - Frame counters: the per-direction AEAD nonce counter advances when a frame is sealed, i.e. in
//   send() order, and frames are written in exactly that order, so the peer's counter always
//   matches. A frame that fails to encode does not advance the counter.
// - Outbound cap: more than kMaxOutboundQueueBytes in flight + pending -> immediate close
//   (CloseReason::OutboundOverflow).
// - Buffer lifetime: the buffer an async_write references (m_inflight) is never touched — not
//   cleared, resized or swapped — until that write's completion handler has run, also when the
//   connection is aborted or closed by the peer meanwhile (the handler keeps the Connection alive).
// - close() is graceful: queued frames are flushed, the send side is shut down and the connection
//   lingers (discarding input) until the peer closes or the flush timeout expires. abort() closes
//   immediately. Either way the closed handler runs exactly once, after which both handlers are
//   released (so handlers may safely capture an owner that holds the Connection).
// - Bad frame lengths and AEAD failures close immediately without any error packet (PROTOCOL §2.2).
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

    // Frames sent between these two calls are written together; endBatch() starts the write.
    // Nestable. The read loop wraps every chunk it dispatches in a batch, so all replies produced
    // while handling one read leave in one write.
    void beginBatch() noexcept { ++m_batchDepth; }
    void endBatch();

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
    // Bytes in flight + pending (what the outbound cap applies to).
    [[nodiscard]] std::size_t queuedBytes() const noexcept { return m_inflight.size() + m_pending.size(); }
    // Statistics / tests: async_write calls issued, frames queued, frames received.
    [[nodiscard]] std::uint64_t writeCount() const noexcept { return m_writes; }
    [[nodiscard]] std::uint64_t framesSent() const noexcept { return m_framesSent; }
    [[nodiscard]] std::uint64_t framesReceived() const noexcept { return m_framesReceived; }

    // Maximum time close() waits for the queue to flush and the peer to close. Default 2 s.
    void setCloseTimeout(std::chrono::milliseconds timeout) noexcept { m_closeTimeout = timeout; }

    // Testing hook: queues bytes verbatim (no framing, no encryption).
    bool sendRawForTesting(std::vector<std::uint8_t> bytes);

private:
    void doRead();
    void onRead(const std::error_code& ec, std::size_t bytes);
    bool afterAppend(std::size_t before);
    void maybeWrite();
    void onWrite(const std::error_code& ec);
    void finishGracefulClose();
    void terminate(CloseReason reason);

    asio::ip::tcp::socket m_socket;
    asio::steady_timer m_closeTimer;
    std::string m_remoteAddress;

    MessageHandler m_onMessage;
    ClosedHandler m_onClosed;

    std::array<std::uint8_t, 16384> m_readBuffer{};
    FrameDecoder m_decoder;
    FrameDecoder::Frame m_frame;
    std::vector<std::uint8_t> m_plain;
    std::optional<FrameCipher> m_cipher;

    std::vector<std::uint8_t> m_pending;  // frames appended since the current write started
    std::vector<std::uint8_t> m_inflight; // owned by the current async_write until its handler ran
    bool m_writing = false;
    int m_batchDepth = 0;
    std::uint64_t m_writes = 0;
    std::uint64_t m_framesSent = 0;
    std::uint64_t m_framesReceived = 0;

    std::chrono::milliseconds m_closeTimeout{2000};
    bool m_started = false;
    bool m_closing = false;   // close() requested, or closed
    bool m_sentFin = false;   // graceful close: send side shut down, lingering for peer EOF
    bool m_closed = false;    // socket closed, closed handler posted
};

} // namespace vo::net
