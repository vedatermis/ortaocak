// ServerConnection: the server's side of one framed (after the handshake encrypted) TCP stream.
//
// Same wire format, close semantics and limits as vo::net::Connection (shared/net/Connection.h; it
// reuses FrameDecoder / encode*Frame / FrameCipher), with one difference that matters at scale:
// outbound frames are *coalesced*. Frames sent while a write is in flight, or between beginBatch()
// and endBatch(), are appended to one buffer and go out in a single async_write. vo::net::Connection
// writes one frame per async_write and waits for its completion before the next, which caps a
// connection at roughly one frame per completion round trip (~1000 frames/s measured on loopback)
// regardless of CPU — not enough for a player in a crowded area (docs/SERVER_WORLD.md §9).
// TODO: fold the coalescing into shared/net/Connection (owner: shared/net) and drop this class.
//
// - More than kMaxOutboundQueueBytes queued (in flight + pending) -> immediate close
//   (CloseReason::OutboundOverflow).
// - close() is graceful (flush, shut down the send side, linger until the peer closes or the close
//   timeout); abort() closes at once. The closed handler runs exactly once, posted, after which both
//   handlers are released.
//
// THREADING CONTRACT: as vo::net::Connection: not thread-safe; call it only on the thread running
// the socket's io_context, which must be driven by a single thread.
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

namespace vo::server::net {

class ServerConnection : public std::enable_shared_from_this<ServerConnection>
{
public:
    using MessageHandler = std::function<void(std::uint16_t opcode, std::span<const std::uint8_t> payload)>;
    using ClosedHandler = std::function<void(vo::net::CloseReason reason)>;

    // Takes ownership of a connected socket and enables TCP_NODELAY.
    [[nodiscard]] static std::shared_ptr<ServerConnection> create(asio::ip::tcp::socket socket);

    struct PrivateTag
    {
        explicit PrivateTag() = default;
    };
    ServerConnection(PrivateTag, asio::ip::tcp::socket socket); // use create()
    ~ServerConnection();
    ServerConnection(const ServerConnection&) = delete;
    ServerConnection& operator=(const ServerConnection&) = delete;

    void start(MessageHandler onMessage, ClosedHandler onClosed);

    bool sendPlain(std::uint16_t opcode, std::span<const std::uint8_t> payload);
    bool send(std::uint16_t opcode, std::span<const std::uint8_t> payload);

    template <class T>
    bool sendMessage(const T& msg)
    {
        vo::net::PacketWriter w;
        msg.write(w);
        return w.ok() && send(static_cast<std::uint16_t>(T::kOpcode), w.data());
    }
    template <class T>
    bool sendPlainMessage(const T& msg)
    {
        vo::net::PacketWriter w;
        msg.write(w);
        return w.ok() && sendPlain(static_cast<std::uint16_t>(T::kOpcode), w.data());
    }

    // Frames sent between these two calls are written together (endBatch() starts the write).
    void beginBatch() noexcept { ++m_batchDepth; }
    void endBatch();

    void enableEncryption(vo::net::SessionKeys keys);
    [[nodiscard]] bool isEncrypted() const noexcept { return m_cipher.has_value(); }

    void close();
    void abort();

    [[nodiscard]] bool isOpen() const noexcept { return !m_closing && !m_closed; }
    [[nodiscard]] const std::string& remoteAddress() const noexcept { return m_remoteAddress; }
    [[nodiscard]] asio::any_io_executor executor() { return m_socket.get_executor(); }
    [[nodiscard]] std::size_t queuedBytes() const noexcept { return m_inflight.size() + m_pending.size(); }
    // async_write calls issued so far (statistics/tests).
    [[nodiscard]] std::uint64_t writeCount() const noexcept { return m_writes; }

    void setCloseTimeout(std::chrono::milliseconds timeout) noexcept { m_closeTimeout = timeout; }

private:
    void doRead();
    void onRead(const std::error_code& ec, std::size_t bytes);
    bool afterAppend(std::size_t before);
    void maybeWrite();
    void onWrite(const std::error_code& ec);
    void finishGracefulClose();
    void terminate(vo::net::CloseReason reason);

    asio::ip::tcp::socket m_socket;
    asio::steady_timer m_closeTimer;
    std::string m_remoteAddress;

    MessageHandler m_onMessage;
    ClosedHandler m_onClosed;

    std::array<std::uint8_t, 16384> m_readBuffer{};
    vo::net::FrameDecoder m_decoder;
    vo::net::FrameDecoder::Frame m_frame;
    std::vector<std::uint8_t> m_plain;
    std::optional<vo::net::FrameCipher> m_cipher;

    std::vector<std::uint8_t> m_pending;  // frames appended since the current write started
    std::vector<std::uint8_t> m_inflight; // the buffer the current async_write owns
    bool m_writing = false;
    int m_batchDepth = 0;
    std::uint64_t m_writes = 0;

    std::chrono::milliseconds m_closeTimeout{2000};
    bool m_started = false;
    bool m_closing = false;
    bool m_sentFin = false;
    bool m_closed = false;
};

} // namespace vo::server::net
