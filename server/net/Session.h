// Session: one client connection on the server and its state machine (docs/PROTOCOL.md §4):
//   Handshake -> Auth -> CharSelect <-> InWorld
// Handles the handshake, timeouts (handshake 5 s, login 30 s, idle 30 s), PING, opcode/state
// validation (ERROR(BadState) + close), malformed payloads (ERROR(ProtocolViolation) + close) and the
// login/character requests against IAccountService.
//
// Threading: all members run on the server io_context thread. Account-service callbacks may arrive on
// any thread; they are posted back onto the connection's executor before touching the session.
// While a service request is pending, further requests are queued (max 16, then ERROR(Flood)) and
// processed in order afterwards; PING is answered immediately.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/net/ServerContext.h"
#include "shared/net/Connection.h"
#include "shared/net/Opcodes.h"

#include <asio/any_io_executor.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vo::server::net {

class Session : public std::enable_shared_from_this<Session>
{
public:
    enum class State : std::uint8_t
    {
        Handshake,
        Auth,
        CharSelect,
        InWorld,
        Closed,
    };

    struct PrivateTag
    {
        explicit PrivateTag() = default;
    };

    // `overCapacity`: answer HELLO with ServerFull.
    [[nodiscard]] static std::shared_ptr<Session> create(std::shared_ptr<ServerContext> ctx,
                                                         std::shared_ptr<vo::net::Connection> conn, std::uint64_t id,
                                                         bool overCapacity);
    Session(PrivateTag, std::shared_ptr<ServerContext> ctx, std::shared_ptr<vo::net::Connection> conn,
            std::uint64_t id, bool overCapacity);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start();

    // Sends ERROR(code) if the session is encrypted, then closes gracefully. Before the handshake
    // completes there is no way to send ERROR, so the connection is just closed.
    void kick(vo::net::ErrorCode code, std::string_view debugMessage);

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }
    [[nodiscard]] bool overCapacity() const noexcept { return m_overCapacity; }

private:
    struct Deferred
    {
        std::uint16_t opcode = 0;
        std::vector<std::uint8_t> payload;
    };

    void onMessage(std::uint16_t opcode, std::span<const std::uint8_t> payload);
    void dispatch(std::uint16_t opcode, std::span<const std::uint8_t> payload);
    void drainDeferred();
    void onClosed(vo::net::CloseReason reason);
    void scheduleTick();
    void onTick();

    void handleHello(std::uint16_t opcode, std::span<const std::uint8_t> payload);
    void handlePing(std::span<const std::uint8_t> payload);
    void handleLogin(std::span<const std::uint8_t> payload);
    void handleCharList(std::span<const std::uint8_t> payload);
    void handleCharCreate(std::span<const std::uint8_t> payload);
    void handleCharDelete(std::span<const std::uint8_t> payload);
    void handleEnterWorld(std::span<const std::uint8_t> payload);
    void handleLeaveWorld(std::span<const std::uint8_t> payload);
    void handleLogout(std::span<const std::uint8_t> payload);
    void handleMove(std::span<const std::uint8_t> payload);

    void onLoginDone(const auth::LoginOutcome& outcome);

    void protocolViolation(std::string_view what);
    void closeSilently(std::string_view why);
    void setState(State s);
    void savePositionIfInWorld(std::function<void(bool)> done);

    // Wraps a member continuation so it runs on this session's executor, only if the session is
    // still alive. Safe to invoke from any thread.
    template <class R, class F>
    std::function<void(R)> onExecutor(F f)
    {
        return [weak = weak_from_this(), ex = m_executor, f](R result) {
            asio::post(ex, [weak, f, result = std::move(result)]() mutable {
                if (auto self = weak.lock())
                    f(*self, std::move(result));
            });
        };
    }

    std::shared_ptr<ServerContext> m_ctx;
    std::shared_ptr<vo::net::Connection> m_conn;
    asio::any_io_executor m_executor;
    asio::steady_timer m_tickTimer;
    std::uint64_t m_id;
    bool m_overCapacity;
    std::string m_address;

    State m_state = State::Handshake;
    bool m_busy = false; // an account-service request is in flight
    std::deque<Deferred> m_deferred;

    core::MonoTime m_acceptedAt;
    core::MonoTime m_authStartedAt;
    core::MonoTime m_lastReceiveAt;

    auth::AccountId m_accountId = 0;
    auth::CharacterId m_characterId = 0;
    std::uint32_t m_entityId = 0;
    auth::CharacterPosition m_position;
    bool m_positionSaved = true;
};

[[nodiscard]] std::string_view sessionStateName(Session::State s) noexcept;
// Whether `opcode` is a valid client opcode in `state` (§4 table).
[[nodiscard]] bool isOpcodeAllowed(Session::State state, std::uint16_t opcode) noexcept;

} // namespace vo::server::net
