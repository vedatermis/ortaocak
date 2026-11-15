#include "client/net/GameClient.h"

#include "shared/core/Log.h"
#include "shared/net/Messages.h"

#include <asio/io_context.hpp>

#include <thread>

namespace vo::client::net {

namespace vn = vo::net;

std::string_view gameClientStateName(GameClient::State state) noexcept
{
    switch (state)
    {
    case GameClient::State::Disconnected: return "disconnected";
    case GameClient::State::Connecting: return "connecting";
    case GameClient::State::Connected: return "connected";
    }
    return "?";
}

GameClient::GameClient(Settings settings)
    : m_settings(std::move(settings)), m_io(std::make_unique<asio::io_context>())
{
}

GameClient::~GameClient()
{
    disconnect();
    m_retired.clear();
    m_session.reset();
    m_io.reset();
}

Millis GameClient::now()
{
    return std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now().time_since_epoch());
}

bool GameClient::connect()
{
    if (m_state != State::Disconnected)
        return false;
    m_account.reset();
    m_events.clear();
    m_requests.clear();
    m_lastServerError.reset();
    const std::uint64_t generation = ++m_generation;

    if (!m_settings.serverKey)
    {
        VO_LOG_ERROR("net: no valid server_public_key configured; cannot verify the server");
        NetEvent e;
        e.type = NetEventType::ConnectFailed;
        e.connectFailure = ConnectFailure::MissingServerKey;
        e.detail = "missing server key";
        m_events.push_back(std::move(e));
        return true;
    }

    m_session = std::make_unique<vn::ClientSession>(*m_io);
    m_session->setMessageHandler([this, generation](std::uint16_t opcode, std::span<const std::uint8_t> payload)
                                 { onMessage(generation, opcode, payload); });
    m_session->setClosedHandler(
        [this, generation](vn::CloseReason reason)
        {
            if (generation != m_generation)
                return;
            DisconnectInfo info;
            info.cause = DisconnectCause::Closed;
            info.closeReason = reason;
            info.serverError = m_lastServerError;
            failConnected(info);
        });

    vn::ClientSettings cs;
    cs.host = m_settings.host;
    cs.port = m_settings.port;
    cs.serverPublicKey = *m_settings.serverKey;
    cs.clientBuild = m_settings.clientBuild;
    cs.protocolVersion = m_settings.protocolVersion;
    cs.handshakeTimeout = m_settings.handshakeTimeout;

    VO_LOG_INFO("net: connecting to {}:{}", m_settings.host, m_settings.port);
    m_state = State::Connecting;
    m_session->connect(cs,
                       [this, generation](vn::ConnectResult result, const std::string& detail)
                       {
                           if (generation != m_generation)
                               return;
                           NetEvent e;
                           if (result == vn::ConnectResult::Ok)
                           {
                               VO_LOG_INFO("net: connected, session encrypted");
                               m_state = State::Connected;
                               m_ping.reset(now());
                               e.type = NetEventType::Connected;
                           }
                           else
                           {
                               VO_LOG_WARN("net: connect failed: {} ({})", vn::connectResultName(result), detail);
                               m_state = State::Disconnected;
                               e.type = NetEventType::ConnectFailed;
                               e.connectResult = result;
                               e.detail = detail;
                               ++m_generation;
                               dropSession();
                           }
                           m_events.push_back(std::move(e));
                       });
    return true;
}

void GameClient::dropSession()
{
    if (m_session)
    {
        m_session->close();
        m_retired.push_back(std::move(m_session));
    }
}

void GameClient::disconnect()
{
    ++m_generation; // callbacks of the old session (e.g. Cancelled) are ignored from here on
    if (m_state != State::Disconnected)
        VO_LOG_INFO("net: disconnecting (local)");
    dropSession();
    m_state = State::Disconnected;
    m_events.clear();
    m_requests.clear();
}

void GameClient::logout()
{
    if (m_state == State::Connected && m_account.accountId != 0) // LOGOUT is only valid once logged in
    {
        VO_LOG_INFO("net: logout");
        (void)send(vn::Logout{});
    }
    disconnect();
}

void GameClient::failConnected(DisconnectInfo info)
{
    const UserMessage msg = disconnectMessage(info);
    VO_LOG_WARN("net: connection lost ({}{}): {}", vn::closeReasonName(info.closeReason),
                info.serverError ? ", ERROR " + std::string(vn::errorCodeName(
                                                    static_cast<vn::ErrorCode>(*info.serverError)))
                                 : std::string(),
                msg.text);
    ++m_generation;
    dropSession();
    m_state = State::Disconnected;
    m_requests.clear();
    NetEvent e;
    e.type = NetEventType::Disconnected;
    e.disconnect = info;
    m_events.push_back(std::move(e));
}

bool GameClient::sendRaw(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (m_state != State::Connected || !m_session)
        return false;
    if (!m_session->send(opcode, payload))
    {
        VO_LOG_WARN("net: cannot send {}", vn::opcodeName(static_cast<vn::Opcode>(opcode)));
        return false;
    }
    VO_LOG_TRACE("net: -> {} ({} bytes)", vn::opcodeName(static_cast<vn::Opcode>(opcode)), payload.size());
    return true;
}

void GameClient::onMessage(std::uint64_t generation, std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (generation != m_generation)
        return;
    VO_LOG_TRACE("net: <- {} ({} bytes)", vn::opcodeName(static_cast<vn::Opcode>(opcode)), payload.size());
    if (opcode == static_cast<std::uint16_t>(vn::Opcode::Pong))
    {
        vn::Pong pong;
        if (vn::decodeMessage(payload, pong))
        {
            if (const auto rtt = m_ping.onPong(pong.clientTimeMs, now()))
                VO_LOG_DEBUG("net: rtt {:.0f} ms", *rtt);
        }
        return;
    }
    if (opcode == static_cast<std::uint16_t>(vn::Opcode::Error))
    {
        vn::Error err;
        if (vn::decodeMessage(payload, err))
        {
            m_lastServerError = static_cast<std::uint16_t>(err.errorCode);
            VO_LOG_WARN("net: server ERROR {} ('{}')", vn::errorCodeName(err.errorCode), err.debugMessage);
        }
        return;
    }
    m_requests.complete(opcode);
    NetEvent e;
    e.type = NetEventType::Message;
    e.opcode = opcode;
    e.payload.assign(payload.begin(), payload.end());
    m_events.push_back(std::move(e));
}

void GameClient::injectMessage(std::uint16_t opcode, std::vector<std::uint8_t> payload)
{
    if (m_state != State::Connected)
        return;
    VO_LOG_TRACE("net: <- {} ({} bytes, injected)", vn::opcodeName(static_cast<vn::Opcode>(opcode)), payload.size());
    NetEvent e;
    e.type = NetEventType::Message;
    e.opcode = opcode;
    e.payload = std::move(payload);
    m_events.push_back(std::move(e));
}

void GameClient::poll()
{
    m_retired.clear(); // never inside a session callback: safe to destroy here
    if (m_io->stopped())
        m_io->restart();
    m_io->poll();

    if (m_state != State::Connected)
        return;
    const Millis t = now();
    if (const auto op = m_requests.expired(t))
    {
        VO_LOG_WARN("net: no {} within {} ms", vn::opcodeName(static_cast<vn::Opcode>(*op)),
                    m_settings.requestTimeout.count());
        DisconnectInfo info;
        info.cause = DisconnectCause::RequestTimeout;
        info.closeReason = vn::CloseReason::LocalClose;
        failConnected(info);
        return;
    }
    if (m_ping.timedOut(t))
    {
        DisconnectInfo info;
        info.cause = DisconnectCause::PingTimeout;
        info.closeReason = vn::CloseReason::LocalClose;
        failConnected(info);
        return;
    }
    if (const auto stamp = m_ping.pingDue(t))
        (void)send(vn::Ping{*stamp});
}

std::optional<NetEvent> GameClient::nextEvent()
{
    if (m_events.empty())
        return std::nullopt;
    NetEvent e = std::move(m_events.front());
    m_events.pop_front();
    return e;
}

void GameClient::flush(Millis budget)
{
    const Millis deadline = now() + budget;
    m_retired.clear();
    if (m_io->stopped())
        m_io->restart();
    while (now() < deadline)
    {
        m_io->poll();
        if (m_io->stopped())
            break; // no outstanding work: every graceful close has finished
        std::this_thread::sleep_for(Millis(2));
    }
}

} // namespace vo::client::net
