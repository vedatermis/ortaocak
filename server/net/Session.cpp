#include "server/net/Session.h"

#include "server/world/MoveValidator.h"
#include "shared/core/Log.h"
#include "shared/net/Messages.h"

#include <asio/post.hpp>

#include <algorithm>

namespace vo::server::net {

namespace vn = vo::net;
using vn::Opcode;

namespace {

constexpr std::size_t kMaxDeferredRequests = 16;
// Outbound backlog worth a (rate-limited) warning; the hard cap (256 KB) closes the connection.
constexpr std::size_t kQueueWarnBytes = 128 * 1024;

constexpr bool is(std::uint16_t raw, Opcode op) noexcept
{
    return raw == static_cast<std::uint16_t>(op);
}

// The world's handle to a session: batches are posted onto the session's executor, never run on
// the world thread.
class SessionLink final : public world::IPlayerLink
{
public:
    SessionLink(std::weak_ptr<Session> session, asio::any_io_executor executor)
        : m_session(std::move(session)), m_executor(std::move(executor))
    {
    }

    void deliver(world::WorldBatch batch) override
    {
        asio::post(m_executor, [weak = m_session, batch = std::move(batch)]() mutable {
            if (auto s = weak.lock())
                s->onWorldBatch(std::move(batch));
        });
    }

private:
    std::weak_ptr<Session> m_session;
    asio::any_io_executor m_executor;
};

vn::ItemEntry toEntry(game::ItemPlace place, const game::ItemStack& s)
{
    vn::ItemEntry e;
    e.container = place.container;
    e.slot = place.slot;
    if (!s.empty())
    {
        e.itemId = s.id;
        e.templateId = s.templateId;
        e.quantity = s.quantity;
    }
    return e;
}

// Logs every stored item whose template this server does not know, once per item and process.
void logUnknownItems(ServerContext& ctx, auth::CharacterId character, const std::vector<game::StoredItem>& items)
{
    for (const auto& item : items)
    {
        if (ctx.items->find(item.templateId))
            continue;
        bool first = false;
        {
            const std::scoped_lock lock(ctx.unknownItemsMutex);
            first = ctx.loggedUnknownItems.insert(item.id).second;
        }
        if (first)
            VO_LOG_WARN("items: item {} of character {} has unknown template {} at ({}, {}); kept but hidden", item.id,
                        character, item.templateId, item.place.container, item.place.slot);
    }
}

} // namespace

vn::CharSummary Session::toSummary(const auth::CharacterInfo& c) const
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
    const game::ItemCatalog& items = *m_ctx->items;
    if (c.starterKitGranted)
    {
        const auto shown = [&](std::uint16_t id) -> std::uint16_t { return items.find(id) ? id : 0; };
        s.mainHandTemplate = shown(c.mainHandTemplate);
        s.offHandTemplate = shown(c.offHandTemplate);
    }
    else
    {
        // §1.4: listed with the kit it will receive on its first entry.
        const auto kit = game::starterKitVisible(items, c.charClass);
        s.mainHandTemplate = kit.mainHand;
        s.offHandTemplate = kit.offHand;
    }
    return s;
}

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
        return is(op, Opcode::MoveReq) || is(op, Opcode::ItemMoveReq) || is(op, Opcode::LeaveWorldReq) ||
               is(op, Opcode::Logout) || is(op, Opcode::Ping);
    case Session::State::Closed: return false;
    }
    return false;
}

std::shared_ptr<Session> Session::create(std::shared_ptr<ServerContext> ctx, std::shared_ptr<vo::net::Connection> conn,
                                         std::uint64_t id, bool overCapacity)
{
    return std::make_shared<Session>(PrivateTag{}, std::move(ctx), std::move(conn), id, overCapacity);
}

Session::Session(PrivateTag, std::shared_ptr<ServerContext> ctx, std::shared_ptr<vo::net::Connection> conn,
                 std::uint64_t id, bool overCapacity)
    : m_ctx(std::move(ctx)), m_conn(std::move(conn)), m_executor(m_conn->executor()), m_tickTimer(m_executor),
      m_id(id), m_overCapacity(overCapacity), m_address(m_conn->remoteAddress())
{
    m_acceptedAt = core::monoNow();
    m_lastReceiveAt = m_acceptedAt;
    m_lastBusyAt = m_acceptedAt;
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
        // Time spent waiting for the server itself (a queued login, a world/DB request) is not idle.
        if (m_busy)
            m_lastBusyAt = now;
        if (!m_busy && now - std::max(m_lastReceiveAt, m_lastBusyAt) >= st.idleTimeout)
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

void Session::kickAsync(vn::ErrorCode code, std::string_view debugMessage)
{
    asio::post(m_executor, [self = shared_from_this(), code, msg = std::string(debugMessage)] { self->kick(code, msg); });
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

bool Session::isStaleMove(core::MonoTime now) const noexcept
{
    if (m_state == State::InWorld && m_leavePending)
        return true; // LEAVE_WORLD_REQ / LOGOUT received, the player is already out of the world
    return m_state == State::CharSelect && m_hasLeftWorld && now - m_leftWorldAt < kLeaveGrace;
}

void Session::leaveWorld(std::function<void(bool)> done)
{
    m_worldJoined = false;
    m_enterPending = false;
    m_worldGeneration = 0; // later batches (and item completions) of this world session are dropped
    m_itemQueue.clear();   // not decided yet: dropped with the world session (a move in flight still commits)
    m_ctx->world.post(world::LeaveCommand{m_id, std::move(done)});
}

void Session::onClosed(vn::CloseReason reason)
{
    VO_LOG_DEBUG("session {}: connection closed ({})", m_id, vn::closeReasonName(reason));
    setState(State::Closed);
    m_tickTimer.cancel();
    m_deferred.clear();
    if (m_worldJoined)
        leaveWorld(nullptr); // the world despawns the player and saves its position

    auto& ctx = *m_ctx;
    const std::scoped_lock lock(ctx.registryMutex);
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

    if (is(opcode, Opcode::MoveReq) && isStaleMove(m_lastReceiveAt))
    {
        // Still counted against the 30/s cap, so the grace window cannot be used for flooding.
        if (!m_moveLimiter.allow(m_lastReceiveAt))
        {
            kick(vn::ErrorCode::Flood, "too many MOVE_REQ");
            return;
        }
        if (m_staleMovesDropped++ == 0)
            VO_LOG_DEBUG("session {}: dropping MOVE_REQ sent before leaving the world", m_id);
        return;
    }
    if (m_busy && !is(opcode, Opcode::Ping))
    {
        if (m_deferred.size() >= kMaxDeferredRequests)
        {
            kick(vn::ErrorCode::Flood, "too many pending requests");
            return;
        }
        m_deferred.push_back({opcode, std::vector<std::uint8_t>(payload.begin(), payload.end()), m_lastReceiveAt});
        return;
    }
    dispatch(opcode, payload, m_lastReceiveAt);
}

void Session::drainDeferred()
{
    while (!m_busy && m_state != State::Closed && !m_deferred.empty())
    {
        Deferred d = std::move(m_deferred.front());
        m_deferred.pop_front();
        dispatch(d.opcode, d.payload, d.receivedAt);
    }
}

void Session::dispatch(std::uint16_t opcode, std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
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
    case Opcode::MoveReq: handleMove(payload, receivedAt); break;
    case Opcode::ItemMoveReq: handleItemMove(payload, receivedAt); break;
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
    if (!m_worldJoined)
    {
        setState(State::Closed);
        m_deferred.clear();
        m_conn->close();
        return;
    }
    // In the world: despawn and save first, then close.
    m_busy = true;
    m_leavePending = true;
    leaveWorld(onExecutor<bool>([](Session& s, bool saved) {
        s.m_busy = false;
        if (!saved)
            VO_LOG_WARN("session {}: saving the position of character {} at logout failed", s.m_id, s.m_characterId);
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
    m_ctx->accounts.login(std::move(req.username), std::move(req.password), m_address,
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
        std::shared_ptr<Session> old;
        {
            const std::scoped_lock lock(m_ctx->registryMutex);
            auto& online = m_ctx->online;
            if (const auto it = online.find(outcome.accountId); it != online.end())
            {
                old = it->second.lock();
                if (old.get() == this)
                    old.reset();
            }
            online[outcome.accountId] = weak_from_this();
        }
        if (old)
        {
            VO_LOG_INFO("session {}: account {} logged in again, kicking session {}", m_id, outcome.accountId,
                        old->id());
            old->kickAsync(vn::ErrorCode::KickedDuplicateLogin, "logged in from another connection");
        }
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
                res.characters.push_back(s.toSummary(c));
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
                res.character = s.toSummary(out.character);
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

// ---- World ----

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
            if (s.m_state != State::CharSelect)
            {
                s.m_busy = false;
                return;
            }
            if (out.result != vn::EnterWorldResult::Ok)
            {
                s.m_busy = false;
                vn::EnterWorldRes res;
                res.result = out.result;
                (void)s.m_conn->sendMessage(res);
                s.drainDeferred();
                return;
            }
            s.onCharacterLoaded(std::move(out.character));
        }));
}

void Session::onCharacterLoaded(auth::CharacterInfo character)
{
    std::optional<game::StarterKit> kit;
    if (const game::StarterKit* k = m_ctx->items->kitFor(character.charClass))
        kit = *k;
    const auth::CharacterId id = character.id;
    m_ctx->accounts.loadCharacterItems(
        m_accountId, id, std::move(kit),
        onExecutor<auth::LoadItemsOutcome>([character = std::move(character)](Session& s, auth::LoadItemsOutcome out) {
            s.onItemsLoaded(character, std::move(out));
        }));
}

void Session::onItemsLoaded(auth::CharacterInfo character, auth::LoadItemsOutcome items)
{
    if (m_state != State::CharSelect)
    {
        m_busy = false;
        return;
    }
    game::Inventory inventory;
    std::string error;
    bool ok = items.ok;
    if (ok && !game::Inventory::fromItems(items.items, inventory, error))
    {
        VO_LOG_ERROR("session {}: items of character {} are inconsistent: {}", m_id, character.id, error);
        ok = false;
    }
    if (!ok)
    {
        if (items.ok == false)
            VO_LOG_ERROR("session {}: loading the items of character {} failed", m_id, character.id);
        m_busy = false;
        vn::EnterWorldRes res;
        res.result = vn::EnterWorldResult::ServerError;
        (void)m_conn->sendMessage(res);
        drainDeferred();
        return;
    }
    logUnknownItems(*m_ctx, character.id, items.items);
    if (items.kitGranted)
        VO_LOG_INFO("session {}: granted the starter kit to character {} ({} item(s))", m_id, character.id,
                    inventory.count());

    // The world places the character and answers with this session's first batch
    // (onWorldBatch -> ENTER_WORLD_RES + INVENTORY + ENTITY_SPAWNs). Stay busy until then.
    m_inventory = inventory;
    m_itemUser = {character.charClass, character.level};
    m_visible = game::visibleEquipment(*m_ctx->items, m_inventory);
    m_itemQueue.clear();
    character.starterKitGranted = true;
    character.mainHandTemplate = m_visible.mainHand;
    character.offHandTemplate = m_visible.offHand;

    m_characterId = character.id;
    m_worldGeneration = ++m_generationCounter;
    m_worldJoined = true;
    m_enterPending = true;
    m_hasMoveSeq = false;
    world::EnterCommand cmd;
    cmd.session = m_id;
    cmd.generation = m_worldGeneration;
    cmd.link = std::make_shared<SessionLink>(weak_from_this(), m_executor);
    cmd.account = m_accountId;
    cmd.character = std::move(character);
    m_ctx->world.post(std::move(cmd));
}

void Session::sendInventory()
{
    vn::Inventory inv;
    inv.bagSize = static_cast<std::uint8_t>(game::kBagSlots);
    for (const auto& item : m_inventory.items()) // equipment first, by slot
    {
        if (!m_ctx->items->find(item.templateId))
            continue; // hidden (logged at load)
        inv.items.push_back({item.place.container, item.place.slot, item.id, item.templateId, item.quantity});
    }
    (void)m_conn->sendMessage(inv);
}

// ---- Items ----

void Session::handleItemMove(std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    if (!m_itemLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} ITEM_MOVE_REQ per second from {}", m_id,
                    world::ItemMoveRateLimiter::kLimit, m_address);
        kick(vn::ErrorCode::Flood, "too many ITEM_MOVE_REQ");
        return;
    }
    vn::ItemMoveReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed ITEM_MOVE_REQ");
        return;
    }
    if (m_itemQueue.size() >= kMaxQueuedItemMoves)
    {
        // Only reachable while the database is very slow; the client rolls back.
        (void)m_conn->sendMessage(vn::ItemMoveRes{req.seq, vn::ItemMoveResult::ServerError});
        return;
    }
    m_itemQueue.push_back(req);
    processItemMoves();
}

void Session::processItemMoves()
{
    while (!m_itemInFlight && !m_itemQueue.empty() && m_state == State::InWorld && !m_leavePending)
    {
        const vn::ItemMoveReq req = m_itemQueue.front();
        m_itemQueue.pop_front();
        const game::ItemPlace from{req.fromContainer, req.fromSlot};
        const game::ItemPlace to{req.toContainer, req.toSlot};
        game::ItemMovePlan plan = game::planItemMove(*m_ctx->items, m_inventory, m_itemUser, from, to);
        if (plan.result != vn::ItemMoveResult::Ok)
        {
            VO_LOG_TRACE("session {}: item move seq {} ({},{})->({},{}) refused: {}", m_id, req.seq, from.container,
                         from.slot, to.container, to.slot, vn::itemMoveResultName(plan.result));
            (void)m_conn->sendMessage(vn::ItemMoveRes{req.seq, plan.result});
            continue;
        }
        auto relocations = plan.relocations(m_inventory);
        m_itemInFlight = true;
        const std::uint64_t generation = m_worldGeneration;
        const std::uint32_t seq = req.seq;
        m_ctx->accounts.applyItemMove(m_accountId, m_characterId, std::move(relocations),
                                      onExecutor<bool>([plan = std::move(plan), seq, generation](Session& s, bool ok) {
                                          s.onItemMoveDone(plan, seq, generation, ok);
                                      }));
    }
}

void Session::onItemMoveDone(const game::ItemMovePlan& plan, std::uint32_t seq, std::uint64_t generation, bool ok)
{
    m_itemInFlight = false;
    if (m_state != State::InWorld || generation != m_worldGeneration)
    {
        // Left the world meanwhile: the next entry reloads the items (after this commit, as the
        // account service runs one account's requests in order).
        processItemMoves();
        return;
    }
    if (!ok)
    {
        VO_LOG_WARN("session {}: item move seq {} of character {} was not stored", m_id, seq, m_characterId);
        (void)m_conn->sendMessage(vn::ItemMoveRes{seq, vn::ItemMoveResult::ServerError});
        processItemMoves();
        return;
    }
    game::applyItemMove(m_inventory, plan);
    vn::ItemUpdate update;
    for (const auto& c : plan.changes)
        update.items.push_back(toEntry(c.place, c.content));
    (void)m_conn->sendMessage(update);
    (void)m_conn->sendMessage(vn::ItemMoveRes{seq, vn::ItemMoveResult::Ok});
    const game::VisibleEquipment visible = game::visibleEquipment(*m_ctx->items, m_inventory);
    if (visible != m_visible)
    {
        m_visible = visible;
        m_ctx->world.post(world::EquipCommand{m_id, visible.mainHand, visible.offHand});
    }
    processItemMoves();
}

void Session::onWorldBatch(world::WorldBatch batch)
{
    if (m_state == State::Closed || batch.generation == 0 || batch.generation != m_worldGeneration)
        return; // stale: from a world session that has been left meanwhile
    const bool completesEnter = batch.entered.has_value();
    // The whole tick for this player leaves in one socket write.
    m_conn->beginBatch();
    struct EndBatch
    {
        vo::net::Connection& c;
        ~EndBatch() { c.endBatch(); }
    } endBatch{*m_conn};
    if (completesEnter)
    {
        if (!m_enterPending || m_state != State::CharSelect)
            return;
        m_enterPending = false;
        const world::EnteredInfo& e = *batch.entered;
        m_entityId = e.entityId;
        vn::EnterWorldRes res;
        res.result = vn::EnterWorldResult::Ok;
        res.entityId = e.entityId;
        res.zoneId = e.zoneId;
        res.x = e.x;
        res.y = e.y;
        res.z = e.z;
        res.yaw = e.yaw;
        res.walkSpeed = e.walkSpeed;
        res.runSpeed = e.runSpeed;
        res.serverTimeMs = e.serverTimeMs;
        setState(State::InWorld);
        (void)m_conn->sendMessage(res);
        sendInventory(); // PROTOCOL_ITEMS.md §2: right after ENTER_WORLD_RES, before any ENTITY_SPAWN
        VO_LOG_INFO("session {}: character {} entered zone {} as entity {} at ({:.1f}, {:.1f}, {:.1f})", m_id,
                    m_characterId, e.zoneId, e.entityId, e.x, e.y, e.z);
    }
    if (m_state != State::InWorld)
        return;
    for (const auto& item : batch.items)
    {
        if (!m_conn->send(item.opcode, batch.payload(item)))
            break; // closing (e.g. the 256 KB outbound cap was exceeded)
    }
    if (m_conn->queuedBytes() > kQueueWarnBytes)
    {
        const auto now = core::monoNow();
        if (now - m_lastQueueWarn > std::chrono::seconds(5))
        {
            m_lastQueueWarn = now;
            VO_LOG_WARN("session {}: {} KB queued for {} (slow reader)", m_id, m_conn->queuedBytes() / 1024, m_address);
        }
    }
    if (completesEnter)
    {
        m_busy = false;
        drainDeferred();
    }
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
    m_leavePending = true;
    m_staleMovesDropped = 0;
    leaveWorld(onExecutor<bool>([](Session& s, bool saved) {
        s.m_busy = false;
        s.m_leavePending = false;
        if (s.m_state != State::InWorld)
            return;
        if (!saved)
            VO_LOG_WARN("session {}: saving the position of character {} failed", s.m_id, s.m_characterId);
        VO_LOG_DEBUG("session {}: character {} left the world", s.m_id, s.m_characterId);
        s.m_characterId = 0;
        s.m_entityId = 0;
        s.m_hasLeftWorld = true;
        s.m_leftWorldAt = core::monoNow();
        s.setState(State::CharSelect);
        (void)s.m_conn->sendMessage(vn::LeaveWorldRes{vn::kLeaveWorldOk});
        s.drainDeferred();
    }));
}

void Session::handleMove(std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    if (!m_moveLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} MOVE_REQ per second from {}", m_id, world::MoveRateLimiter::kLimit,
                    m_address);
        kick(vn::ErrorCode::Flood, "too many MOVE_REQ");
        return;
    }
    vn::MoveReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed MOVE_REQ");
        return;
    }
    std::string_view why;
    if (!world::isPlausibleMove(req, why))
    {
        protocolViolation(why);
        return;
    }
    if (m_hasMoveSeq && req.seq <= m_lastMoveSeq)
    {
        protocolViolation("MOVE_REQ seq not increasing");
        return;
    }
    m_hasMoveSeq = true;
    m_lastMoveSeq = req.seq;
    m_ctx->world.post(world::MoveCommand{m_id, req, receivedAt});
}

} // namespace vo::server::net
