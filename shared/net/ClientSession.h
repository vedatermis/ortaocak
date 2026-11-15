// ClientSession: client side of the connection + handshake (docs/PROTOCOL.md §3), shared by the game
// client and the bot client so neither duplicates protocol logic.
//
//   asio::io_context io;
//   vo::net::ClientSession session(io);
//   session.setMessageHandler([](std::uint16_t op, std::span<const std::uint8_t> payload) { ... });
//   session.setClosedHandler([](vo::net::CloseReason r) { ... });
//   session.connect(settings, [](vo::net::ConnectResult r, const std::string& detail) {
//       if (r == vo::net::ConnectResult::Ok) { /* encrypted; send LOGIN_REQ */ }
//       else if (r == vo::net::ConnectResult::VerificationFailed) { /* "sunucu doğrulanamadı" */ }
//   });
//   // every frame (game client):
//   session.poll();                       // == io.poll(), restarting the io_context when idle
//   session.sendMessage(vo::net::Ping{now});
//
// Steps: resolve -> TCP connect (TCP_NODELAY) -> HELLO (ephemeral kx key) -> HELLO_ACK -> verify the
// Ed25519 signature against the pinned server key -> derive session keys, wipe the ephemeral
// secret -> encrypted mode -> connect callback(Ok). Nothing blocks; everything is driven by the
// io_context.
//
// THREADING CONTRACT: not thread-safe. Construct it, call its members and drive the io_context from
// one thread; every callback (connect, message, closed) is invoked from inside io_context
// run()/poll() on that thread. Exactly one connect callback per connect(). After a successful connect
// the closed handler runs once when the connection ends. No callback runs after the ClientSession is
// destroyed or after close() returns (close() suppresses the closed handler).
#pragma once

#include "shared/net/CloseReason.h"
#include "shared/net/Crypto.h"
#include "shared/net/PacketWriter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace asio {
class io_context;
} // namespace asio

namespace vo::net {

enum class ConnectResult : std::uint8_t
{
    Ok,
    ResolveFailed,
    ConnectFailed,
    Timeout,            // no HELLO_ACK within ClientSettings::handshakeTimeout
    VersionMismatch,    // HELLO_ACK result 1
    ServerFull,         // HELLO_ACK result 2
    Maintenance,        // HELLO_ACK result 3
    VerificationFailed, // bad signature (or server protocol version) -> "sunucu doğrulanamadı"
    ProtocolError,      // unexpected/malformed HELLO_ACK
    Disconnected,       // connection lost during the handshake
    Cancelled,          // close() called before the handshake finished
};
[[nodiscard]] std::string_view connectResultName(ConnectResult r) noexcept;

struct ClientSettings
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 15001;
    SignPublicKey serverPublicKey{}; // pinned (config/client.toml: server_public_key)
    std::uint32_t clientBuild = 0;
    std::uint16_t protocolVersion = 0; // 0 = kProtocolVersion (override for tests only)
    std::chrono::milliseconds handshakeTimeout{10000}; // covers resolve + connect + HELLO_ACK
};

class ClientSession
{
public:
    enum class State : std::uint8_t
    {
        Idle,        // not connected yet
        Connecting,  // resolve / TCP connect
        Handshaking, // HELLO sent, waiting for HELLO_ACK
        Ready,       // encrypted session established
        Closed,      // finished (after failure, close() or disconnect)
    };

    using ConnectHandler = std::function<void(ConnectResult result, const std::string& detail)>;
    using MessageHandler = std::function<void(std::uint16_t opcode, std::span<const std::uint8_t> payload)>;
    using ClosedHandler = std::function<void(CloseReason reason)>;

    explicit ClientSession(asio::io_context& io);
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Messages received in Ready state (raw opcode; decode with decodeMessage<T>()).
    void setMessageHandler(MessageHandler handler);
    void setClosedHandler(ClosedHandler handler);

    // Starts connecting. Only valid in Idle state (a ClientSession is single-use; create a new one to
    // reconnect). Returns false if not Idle.
    bool connect(const ClientSettings& settings, ConnectHandler onDone);

    // Encrypted send; false unless Ready (or if the frame is too large / the queue overflowed).
    bool send(std::uint16_t opcode, std::span<const std::uint8_t> payload);
    template <class T>
    bool sendMessage(const T& msg)
    {
        PacketWriter w;
        msg.write(w);
        return w.ok() && send(static_cast<std::uint16_t>(T::kOpcode), w.data());
    }

    // Graceful close (flushes queued frames). No closed handler is invoked for a local close; a
    // pending connect completes with Cancelled.
    void close();

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool isReady() const noexcept { return state() == State::Ready; }

    // Runs ready handlers without blocking (io_context::poll), restarting the io_context first if
    // it had run out of work. Returns the number of handlers run. For game-loop integration.
    std::size_t poll();

    // Testing hook: queue bytes verbatim on the connection (e.g. a corrupted frame). False unless
    // the TCP connection exists.
    bool sendRawForTesting(std::vector<std::uint8_t> bytes);

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace vo::net
