// GameClient: the game client's connection to the server, shared by all scenes.
//
// Owns the asio io_context and the current vo::net::ClientSession (single-use: a new one per
// connect()). Everything runs on the main thread: call poll() once per frame; it drives the socket,
// sends a PING every 5 s (tracking the RTT), enforces request timeouts and turns what happened into
// NetEvents that the active scene drains with nextEvent():
//
//   Connected      handshake done, encrypted session ready (send LOGIN_REQ now)
//   ConnectFailed  connect() did not succeed (connectResult / connectFailure)
//   Message        a server message (opcode + payload; decode with vo::net::decodeMessage). PONG and
//                  ERROR are consumed here: ERROR is remembered and explains the following close.
//   Disconnected   an established session ended (DisconnectInfo; Turkish text via NetText.h)
//
// disconnect()/logout() are local decisions: they produce no event and drop queued events.
// request() sends a message and expects `responseOpcode` within the request timeout; if it does not
// arrive the connection is dropped with DisconnectCause::RequestTimeout.
#pragma once

#include "client/net/AccountState.h"
#include "client/net/NetText.h"
#include "client/net/RequestTracker.h"
#include "shared/net/ClientSession.h"
#include "shared/net/Crypto.h"
#include "shared/net/Opcodes.h"
#include "shared/net/PacketWriter.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace asio {
class io_context;
}

namespace vo::client::net {

enum class NetEventType : std::uint8_t
{
    Connected,
    ConnectFailed,
    Message,
    Disconnected,
};

struct NetEvent
{
    NetEventType type = NetEventType::Message;
    // ConnectFailed
    ConnectFailure connectFailure = ConnectFailure::Session;
    vo::net::ConnectResult connectResult = vo::net::ConnectResult::Ok;
    std::string detail; // English diagnostic for the log
    // Message
    std::uint16_t opcode = 0;
    std::vector<std::uint8_t> payload;
    // Disconnected
    DisconnectInfo disconnect;

    [[nodiscard]] bool is(vo::net::Opcode op) const noexcept
    {
        return type == NetEventType::Message && opcode == static_cast<std::uint16_t>(op);
    }
};

class GameClient
{
public:
    struct Settings
    {
        std::string host = "127.0.0.1";
        std::uint16_t port = vo::net::kDefaultPort;
        std::optional<vo::net::SignPublicKey> serverKey; // nullopt: config has no valid key
        Millis handshakeTimeout{10000};
        Millis requestTimeout{15000};
        std::uint32_t clientBuild = 0;
        std::uint16_t protocolVersion = 0; // 0 = kProtocolVersion (testing override otherwise)
    };

    enum class State : std::uint8_t
    {
        Disconnected,
        Connecting, // resolve / TCP / handshake
        Connected,  // encrypted session ready
    };

    explicit GameClient(Settings settings);
    ~GameClient();
    GameClient(const GameClient&) = delete;
    GameClient& operator=(const GameClient&) = delete;

    // Starts connecting (only from Disconnected; returns false otherwise). The result arrives as a
    // Connected or ConnectFailed event. Resets account().
    bool connect();
    // Local close (also cancels a pending connect). No event; queued events are dropped.
    void disconnect();
    // Sends LOGOUT and closes gracefully (the queued frames are still flushed). No event.
    void logout();

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] bool isConnected() const noexcept { return m_state == State::Connected; }

    template <class T>
    bool send(const T& msg)
    {
        vo::net::PacketWriter w;
        msg.write(w);
        return w.ok() && sendRaw(static_cast<std::uint16_t>(T::kOpcode), w.data());
    }
    // send() + expect `responseOpcode` within the request timeout.
    template <class T>
    bool request(const T& msg, vo::net::Opcode responseOpcode)
    {
        if (!send(msg))
            return false;
        m_requests.add(static_cast<std::uint16_t>(responseOpcode), now(), m_settings.requestTimeout);
        return true;
    }
    [[nodiscard]] bool hasPendingRequests() const noexcept { return !m_requests.empty(); }

    // Once per frame (also while minimised, so pings keep the session alive).
    void poll();
    // Drains one event; nullopt when the queue is empty.
    [[nodiscard]] std::optional<NetEvent> nextEvent();
    [[nodiscard]] bool hasEvents() const noexcept { return !m_events.empty(); }

    [[nodiscard]] std::optional<double> rttMs() const noexcept { return m_ping.smoothedRttMs(); }
    [[nodiscard]] AccountState& account() noexcept { return m_account; }
    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }

    // Polls the io_context for up to `budget` so a graceful close can flush (used at shutdown).
    void flush(Millis budget);

private:
    [[nodiscard]] static Millis now();
    bool sendRaw(std::uint16_t opcode, std::span<const std::uint8_t> payload);
    void onMessage(std::uint64_t generation, std::uint16_t opcode, std::span<const std::uint8_t> payload);
    void dropSession(); // current session -> m_retired (destroyed at the next poll)
    void failConnected(DisconnectInfo info);

    Settings m_settings;
    std::unique_ptr<asio::io_context> m_io;
    std::unique_ptr<vo::net::ClientSession> m_session;
    std::vector<std::unique_ptr<vo::net::ClientSession>> m_retired;
    State m_state = State::Disconnected;
    std::uint64_t m_generation = 0; // ignores callbacks of sessions we already gave up on
    std::deque<NetEvent> m_events;
    RequestTracker m_requests;
    PingTracker m_ping;
    std::optional<std::uint16_t> m_lastServerError;
    AccountState m_account;
};

[[nodiscard]] std::string_view gameClientStateName(GameClient::State state) noexcept;

} // namespace vo::client::net
