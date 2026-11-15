#include "server/net/Session.h"

#include "shared/core/Log.h"
#include "shared/net/Messages.h"

#include <algorithm>

namespace vo::server::net {

namespace vn = vo::net;
using vn::Opcode;

namespace {

constexpr std::size_t kMaxDeferredRequests = 16;

// TODO(M6): fixed spawn and speeds until the world/zone module exists; use the character's saved
// position and the zone's movement rules instead.
constexpr std::uint16_t kSpawnZoneId = 1;
constexpr float kSpawnX = 256.0f;
constexpr float kSpawnY = 0.0f;
constexpr float kSpawnZ = 256.0f;
constexpr float kSpawnYaw = 0.0f;
constexpr float kWalkSpeed = 2.0f;
constexpr float kRunSpeed = 5.5f;

constexpr bool is(std::uint16_t raw, Opcode op) noexcept
{
    return raw == static_cast<std::uint16_t>(op);
}

vn::CharSummary toSummary(const auth::CharacterInfo& c)
{
    vn::CharSummary s;
    s.charId = c.id;
    s.name = c.name;
    s.race = c.race;
    s.charClass = c.charClass;
    s.face = c.face;
    s.hair = c.hair;
    s.level = c.level;
    s.zoneId = c.position.zoneId;
    return s;
}

} // namespace

std::string_view sessionStateName(Session::State s) noexcept
{
    switch (s)
    {
    case Session::State::Handshake: return "Handshake";
    case Session::State::Auth: return "Auth";
    case Session::State::CharSelect: return "CharSelect";
    case Session::State::InWorld: return "InWorld";
    case Session::State::Closed: return "Closed";
    }
    return "?";
}

bool isOpcodeAllowed(Session::State state, std::uint16_t op) noexcept
{
    switch (state)
    {
    case Session::State::Handshake: return is(op, Opcode::Hello);
    case Session::State::Auth: return is(op, Opcode::LoginReq) || is(op, Opcode::Ping);
    case Session::State::CharSelect:
        return is(op, Opcode::CharListReq) || is(op, Opcode::CharCreateReq) || is(op, Opcode::CharDeleteReq) ||
               is(op, Opcode::EnterWorldReq) || is(op, Opcode::Logout) || is(op, Opcode::Ping);
    case Session::State::InWorld:
        return is(op, Opcode::MoveReq) || is(op, Opcode::LeaveWorldReq) || is(op, Opcode::Logout) ||
               is(op, Opcode::Ping);
    case Session::State::Closed: return false;
    }
    return false;
}

std::shared_ptr<Session> Session::create(std::shared_ptr<ServerContext> ctx, std::shared_ptr<vn::Connection> conn,
                                         std::uint64_t id, bool overCapacity)
{
    return std::make_shared<Session>(PrivateTag{}, std::move(ctx), std::move(conn), id, overCapacity);
}

Session::Session(PrivateTag, std::shared_ptr<ServerContext> ctx, std::shared_ptr<vn::Connection> conn,
                 std::uint64_t id, bool overCapacity)
    : m_ctx(std::move(ctx)), m_conn(std::move(conn)), m_executor(m_conn->executor()), m_tickTimer(m_executor),
      m_id(id), m_overCapacity(overCapacity), m_address(m_conn->remoteAddress())
{
    m_acceptedAt = core::monoNow();
    m_lastReceiveAt = m_acceptedAt;
}

Session::~Session() = default;

void Session::start()
{
    VO_LOG_DEBUG("session {}: accepted from {}{}", m_id, m_address, m_overCapacity ? " (over capacity)" : "");
    auto self = shared_from_this();
    m_conn->start([self](std::uint16_t opcode, std::span<const std::uint8_t> payload) { self->onMessage(opcode, payload); },
                  [self](vn::CloseReason reason) { self->onClosed(reason); });
    scheduleTick();
}

void Session::setState(State s)
{
    if (m_state == s)
        return;
    VO_LOG_TRACE("session {}: {} -> {}", m_id, sessionStateName(m_state), sessionStateName(s));
    m_state = s;
}

// ---- Timers ----

void Session::scheduleTick()
{
    const auto& st = m_ctx->settings;
    auto interval = std::min({st.handshakeTimeout, st.loginTimeout, st.idleTimeout}) / 4;
    interval = std::clamp(interval, std::chrono::milliseconds(10), std::chrono::milliseconds(1000));
    m_tickTimer.expires_after(interval);
    m_tickTimer.async_wait([weak = weak_from_this()](const std::error_code& ec) {
        if (ec)
            return;
        if (auto self = weak.lock())
            self->onTick();
    });
}

void Session::onTick()
{
    if (m_state == State::Closed)
        return;
    const auto now = core::monoNow();
    const auto& st = m_ctx->settings;
    if (m_state == State::Handshake)
    {
        if (now - m_acceptedAt >= st.handshakeTimeout)
        {
            closeSilently("handshake timeout");
            return;
        }
    }
    else
    {
        if (m_state == State::Auth && !m_busy && now - m_authStartedAt >= st.loginTimeout)
        {
            VO_LOG_INFO("session {}: login timeout", m_id);
            kick(vn::ErrorCode::Timeout, "login timeout");
            return;
        }
        if (!m_busy && now - m_lastReceiveAt >= st.idleTimeout)
        {
            VO_LOG_INFO("session {}: idle timeout", m_id);
            kick(vn::ErrorCode::Timeout, "idle timeout");
            return;
        }
    }
    scheduleTick();
}

// ---- Closing ----

void Session::kick(vn::ErrorCode code, std::string_view debugMessage)
{
    if (m_state == State::Closed)
        return;
    const bool encrypted = m_conn->isEncrypted();
    setState(State::Closed);
    m_deferred.clear();
    if (encrypted)
    {
        vn::Error err;
        err.errorCode = code;
        err.debugMessage = std::string(debugMessage.substr(0, 255));
        (void)m_conn->sendMessage(err);
    }
    m_conn->close();
}

void Session::protocolViolation(std::string_view what)
{
    VO_LOG_INFO("session {}: protocol violation from {}: {}", m_id, m_address, what);
    kick(vn::ErrorCode::ProtocolViolation, what);
}

void Session::closeSilently(std::string_view why)
{
    if (m_state == State::Closed)
        return;
    VO_LOG_DEBUG("session {}: closing ({})", m_id, why);
    setState(State::Closed);
    m_deferred.clear();
    m_conn->abort();
}

void Session::savePositionIfInWorld(std::function<void(bool)> done)
{
    if (m_characterId == 0 || m_positionSaved)
    {
        if (done)
            done(true);
        return;
    }
    m_positionSaved = true;
    m_ctx->accounts.saveCharacterPosition(m_accountId, m_characterId, m_position, std::move(done));
}

void Session::onClosed(vn::CloseReason reason)
{
    VO_LOG_DEBUG("session {}: connection closed ({})", m_id, vn::closeReasonName(reason));
    setState(State::Closed);
    m_tickTimer.cancel();
    m_deferred.clear();
    savePositionIfInWorld(nullptr);

    auto& ctx = *m_ctx;
    ctx.sessions.erase(m_id);
    if (m_overCapacity)
        --ctx.overflowSessions;
    else
        --ctx.activeSessions;
    if (m_accountId != 0)
    {
        const auto it = ctx.online.find(m_accountId);
        if (it != ctx.online.end())
        {
            const auto current = it->second.lock();
            if (!current || current.get() == this)
                ctx.online.erase(it);
        }
    }
}

// ---- Dispatch ----

void Session::onMessage(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (m_state == State::Closed)
        return;
    m_lastReceiveAt = core::monoNow();
    VO_LOG_TRACE("session {}: <- {} ({} bytes)", m_id, vn::opcodeName(static_cast<Opcode>(opcode)), payload.size());

    if (m_busy && !is(opcode, Opcode::Ping))
    {
        if (m_deferred.size() >= kMaxDeferredRequests)
        {
            kick(vn::ErrorCode::Flood, "too many pending requests");
            return;
        }
        m_deferred.push_back({opcode, std::vector<std::uint8_t>(payload.begin(), payload.end())});
        return;
    }
    dispatch(opcode, payload);
}

void Session::drainDeferred()
{
    while (!m_busy && m_state != State::Closed && !m_deferred.empty())
    {
        Deferred d = std::move(m_deferred.front());
        m_deferred.pop_front();
        dispatch(d.opcode, d.payload);
    }
}

void Session::dispatch(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    if (m_state == State::Handshake)
    {
        handleHello(opcode, payload);
        return;
    }
    if (!isOpcodeAllowed(m_state, opcode))
    {
        VO_LOG_INFO("session {}: opcode 0x{:04X} ({}) not valid in state {}", m_id, opcode,
                    vn::opcodeName(static_cast<Opcode>(opcode)), sessionStateName(m_state));
        kick(vn::ErrorCode::BadState, "opcode not valid in this state");
        return;
    }
    switch (static_cast<Opcode>(opcode))
    {
    case Opcode::Ping: handlePing(payload); break;
    case Opcode::LoginReq: handleLogin(payload); break;
    case Opcode::CharListReq: handleCharList(payload); break;
    case Opcode::CharCreateReq: handleCharCreate(payload); break;
    case Opcode::CharDeleteReq: handleCharDelete(payload); break;
    case Opcode::EnterWorldReq: handleEnterWorld(payload); break;
    case Opcode::LeaveWorldReq: handleLeaveWorld(payload); break;
    case Opcode::Logout: handleLogout(payload); break;
    case Opcode::MoveReq: handleMove(payload); break;
    default: kick(vn::ErrorCode::BadState, "opcode not valid in this state"); break;
    }
}

// ---- Handshake ----

void Session::handleHello(std::uint16_t opcode, std::span<const std::uint8_t> payload)
{
    // Nothing but HELLO/HELLO_ACK may be sent in plaintext, so failures here close without ERROR.
    vn::Hello hello;
    if (!is(opcode, Opcode::Hello) || !vn::decodeMessage(payload, hello))
    {
        closeSilently("expected a well-formed HELLO");
        return;
    }
    if (hello.magic != vn::kMagic)
    {
        closeSilently("bad magic");
        return;
    }

    vn::HelloAck ack;
    ack.serverProtocolVersion = vn::kProtocolVersion;
    if (hello.protocolVersion != vn::kProtocolVersion)
        ack.result = vn::HelloResult::VersionMismatch;
    else if (m_overCapacity)
        ack.result = vn::HelloResult::ServerFull;
    else if (m_ctx->settings.maintenance)
        ack.result = vn::HelloResult::Maintenance;

    if (ack.result != vn::HelloResult::Ok)
    {
        VO_LOG_INFO("session {}: rejecting handshake from {} (result {}, client protocol {})", m_id, m_address,
                    static_cast<int>(ack.result), hello.protocolVersion);
        (void)m_conn->sendPlainMessage(ack); // key and signature stay zero-filled
        setState(State::Closed);
        m_conn->close();
        return;
    }

    vn::KxKeyPair kx = vn::generateKxKeyPair();
    vn::SessionKeys keys;
    const vn::KxPublicKey serverPk = kx.publicKey;
    if (!vn::deriveServerSessionKeys(kx, hello.clientKxPk, keys)) // wipes kx.secretKey
    {
        closeSilently("unacceptable client key");
        return;
    }
    ack.serverKxPk = serverPk;
    ack.signature = m_ctx->identity.signHandshake(hello.clientKxPk, serverPk, vn::kProtocolVersion);
    if (!m_conn->sendPlainMessage(ack))
    {
        closeSilently("cannot send HELLO_ACK");
        return;
    }
    m_conn->enableEncryption(std::move(keys));
    m_authStartedAt = core::monoNow();
    setState(State::Auth);
    VO_LOG_DEBUG("session {}: handshake complete (client build {})", m_id, hello.clientBuild);
}

// ---- Common ----

void Session::handlePing(std::span<const std::uint8_t> payload)
{
    vn::Ping ping;
    if (!vn::decodeMessage(payload, ping))
    {
        protocolViolation("malformed PING");
        return;
    }
    vn::Pong pong;
    pong.clientTimeMs = ping.clientTimeMs;
    pong.serverTimeMs = m_ctx->serverTimeMs();
    (void)m_conn->sendMessage(pong);
}

void Session::handleLogout(std::span<const std::uint8_t> payload)
{
    vn::Logout logout;
    if (!vn::decodeMessage(payload, logout))
    {
        protocolViolation("malformed LOGOUT");
        return;
    }
    VO_LOG_DEBUG("session {}: logout", m_id);
    m_busy = true;
    savePositionIfInWorld(onExecutor<bool>([](Session& s, bool) {
        s.m_busy = false;
        if (s.m_state == State::Closed)
            return;
        s.setState(State::Closed);
        s.m_deferred.clear();
        s.m_conn->close();
    }));
}

// ---- Login ----

void Session::handleLogin(std::span<const std::uint8_t> payload)
{
    vn::LoginReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed LOGIN_REQ");
        return;
    }
    if (m_ctx->rateLimiter.isBlocked(m_address))
    {
        VO_LOG_INFO("session {}: login rate limit hit for {}", m_id, m_address);
        vn::secureZero(req.password.data(), req.password.size());
        (void)m_conn->sendMessage(vn::LoginRes{vn::LoginResult::TooManyAttempts, 0});
        return;
    }
    m_busy = true;
    m_ctx->accounts.login(std::move(req.username), std::move(req.password),
                          onExecutor<auth::LoginOutcome>(
                              [](Session& s, auth::LoginOutcome outcome) { s.onLoginDone(outcome); }));
}

void Session::onLoginDone(const auth::LoginOutcome& outcome)
{
    m_busy = false;
    if (m_state != State::Auth)
        return; // closed meanwhile

    if (outcome.result == vn::LoginResult::InvalidCredentials)
        m_ctx->rateLimiter.recordFailure(m_address);

    vn::LoginRes res{outcome.result, 0};
    if (outcome.result == vn::LoginResult::Ok)
    {
        res.accountId = outcome.accountId;
        auto& online = m_ctx->online;
        if (const auto it = online.find(outcome.accountId); it != online.end())
        {
            if (auto old = it->second.lock(); old && old.get() != this)
            {
                VO_LOG_INFO("session {}: account {} logged in again, kicking session {}", m_id, outcome.accountId,
                            old->id());
                old->kick(vn::ErrorCode::KickedDuplicateLogin, "logged in from another connection");
            }
        }
        online[outcome.accountId] = weak_from_this();
        m_accountId = outcome.accountId;
        setState(State::CharSelect);
        VO_LOG_INFO("session {}: account {} logged in from {}", m_id, m_accountId, m_address);
    }
    (void)m_conn->sendMessage(res);
    drainDeferred();
}

// ---- Characters ----

void Session::handleCharList(std::span<const std::uint8_t> payload)
{
    vn::CharListReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed CHAR_LIST_REQ");
        return;
    }
    m_busy = true;
    m_ctx->accounts.listCharacters(
        m_accountId, onExecutor<auth::CharacterListOutcome>([](Session& s, auth::CharacterListOutcome out) {
            s.m_busy = false;
            if (s.m_state != State::CharSelect)
                return;
            if (!out.ok)
            {
                // CHAR_LIST_RES has no result field, so a storage failure can only be reported as ERROR.
                VO_LOG_ERROR("session {}: character list failed for account {}", s.m_id, s.m_accountId);
                s.kick(vn::ErrorCode::InternalError, "character list unavailable");
                return;
            }
            vn::CharListRes res;
            for (const auto& c : out.characters)
                res.characters.push_back(toSummary(c));
            (void)s.m_conn->sendMessage(res);
            s.drainDeferred();
        }));
}

void Session::handleCharCreate(std::span<const std::uint8_t> payload)
{
    vn::CharCreateReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed CHAR_CREATE_REQ");
        return;
    }
    auth::CreateCharacterRequest create;
    create.name = std::move(req.name);
    create.race = req.race;
    create.charClass = req.charClass;
    create.face = req.face;
    create.hair = req.hair;
    m_busy = true;
    m_ctx->accounts.createCharacter(
        m_accountId, std::move(create),
        onExecutor<auth::CreateCharacterOutcome>([](Session& s, auth::CreateCharacterOutcome out) {
            s.m_busy = false;
            if (s.m_state != State::CharSelect)
                return;
            vn::CharCreateRes res;
            res.result = out.result;
            if (out.result == vn::CharCreateResult::Ok)
            {
                res.character = toSummary(out.character);
                VO_LOG_INFO("session {}: account {} created character {} '{}'", s.m_id, s.m_accountId,
                            out.character.id, out.character.name);
            }
            (void)s.m_conn->sendMessage(res);
            s.drainDeferred();
        }));
}

void Session::handleCharDelete(std::span<const std::uint8_t> payload)
{
    vn::CharDeleteReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed CHAR_DELETE_REQ");
        return;
    }
    const auto charId = req.charId;
    m_busy = true;
    m_ctx->accounts.deleteCharacter(
        m_accountId, charId,
        onExecutor<auth::DeleteCharacterOutcome>([charId](Session& s, auth::DeleteCharacterOutcome out) {
            s.m_busy = false;
            if (s.m_state != State::CharSelect)
                return;
            if (out.result == vn::CharDeleteResult::Ok)
                VO_LOG_INFO("session {}: account {} deleted character {}", s.m_id, s.m_accountId, charId);
            (void)s.m_conn->sendMessage(vn::CharDeleteRes{out.result, charId});
            s.drainDeferred();
        }));
}

void Session::handleEnterWorld(std::span<const std::uint8_t> payload)
{
    vn::EnterWorldReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed ENTER_WORLD_REQ");
        return;
    }
    m_busy = true;
    m_ctx->accounts.loadCharacterForWorld(
        m_accountId, req.charId,
        onExecutor<auth::LoadCharacterOutcome>([](Session& s, auth::LoadCharacterOutcome out) {
            s.m_busy = false;
            if (s.m_state != State::CharSelect)
                return;
            vn::EnterWorldRes res;
            res.result = out.result;
            if (out.result == vn::EnterWorldResult::Ok)
            {
                // TODO(M6): register the entity in the world simulation, use the saved position and
                // follow up with ENTITY_SPAWN for everyone in range. For now: fixed spawn point.
                s.m_characterId = out.character.id;
                s.m_entityId = s.m_ctx->nextEntityId++;
                s.m_position = {kSpawnZoneId, kSpawnX, kSpawnY, kSpawnZ, kSpawnYaw};
                s.m_positionSaved = false;
                res.entityId = s.m_entityId;
                res.zoneId = kSpawnZoneId;
                res.x = kSpawnX;
                res.y = kSpawnY;
                res.z = kSpawnZ;
                res.yaw = kSpawnYaw;
                res.walkSpeed = kWalkSpeed;
                res.runSpeed = kRunSpeed;
                res.serverTimeMs = s.m_ctx->serverTimeMs();
                s.setState(State::InWorld);
                VO_LOG_INFO("session {}: character {} entered the world as entity {}", s.m_id, s.m_characterId,
                            s.m_entityId);
            }
            (void)s.m_conn->sendMessage(res);
            s.drainDeferred();
        }));
}

void Session::handleLeaveWorld(std::span<const std::uint8_t> payload)
{
    vn::LeaveWorldReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed LEAVE_WORLD_REQ");
        return;
    }
    m_busy = true;
    savePositionIfInWorld(onExecutor<bool>([](Session& s, bool saved) {
        s.m_busy = false;
        if (s.m_state != State::InWorld)
            return;
        if (!saved)
            VO_LOG_WARN("session {}: saving position of character {} failed", s.m_id, s.m_characterId);
        s.m_characterId = 0;
        s.m_entityId = 0;
        s.setState(State::CharSelect);
        (void)s.m_conn->sendMessage(vn::LeaveWorldRes{vn::kLeaveWorldOk});
        s.drainDeferred();
    }));
}

void Session::handleMove(std::span<const std::uint8_t> payload)
{
    vn::MoveReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed MOVE_REQ");
        return;
    }
    // TODO(M6): validate and apply the move (speed, bounds, terrain, 30/s flood cap), relay
    // ENTITY_MOVE to the area of interest, answer POSITION_CORRECT on rejection. Accepted and
    // ignored for now.
}

} // namespace vo::server::net
