#include "server/net/Session.h"

#include "server/world/MoveValidator.h"
#include "shared/core/Log.h"
#include "shared/game/Shop.h"
#include "shared/game/Upgrade.h"
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

// PROTOCOL_COMBAT.md §3: the client's combat requests (valid only in InWorld).
constexpr bool isCombatRequest(std::uint16_t op) noexcept
{
    return is(op, Opcode::TargetReq) || is(op, Opcode::AttackReq) || is(op, Opcode::AttackStopReq) ||
           is(op, Opcode::SkillReq) || is(op, Opcode::RespawnReq) || is(op, Opcode::StanceReq);
}

// PROTOCOL_LOOT.md §4: the client's loot requests (valid only in InWorld).
constexpr bool isLootRequest(std::uint16_t op) noexcept
{
    return is(op, Opcode::LootOpenReq) || is(op, Opcode::LootTakeReq) || is(op, Opcode::LootCloseReq);
}

// PROTOCOL_SHOP.md §3: the client's shop requests (valid only in InWorld).
constexpr bool isShopRequest(std::uint16_t op) noexcept
{
    return is(op, Opcode::ShopOpenReq) || is(op, Opcode::ShopCloseReq) || is(op, Opcode::ShopBuyReq) ||
           is(op, Opcode::ShopSellReq);
}

// PROTOCOL_UPGRADE.md §4: UPGRADE_REQ (valid only in InWorld; dropped like a shop request while
// leaving or changing zone).
constexpr bool isUpgradeRequest(std::uint16_t op) noexcept
{
    return is(op, Opcode::UpgradeReq);
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
        e.upgrade = s.upgrade;
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
        for (std::size_t i = 0; i < s.armorTemplates.size(); ++i) // M17
            s.armorTemplates[i] = shown(c.armorTemplates[i]);
    }
    else
    {
        // §1.4: listed with the kit it will receive on its first entry.
        const auto kit = game::starterKitVisible(items, c.charClass);
        s.mainHandTemplate = kit.mainHand;
        s.offHandTemplate = kit.offHand;
        s.armorTemplates = kit.armor;
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
               is(op, Opcode::Logout) || is(op, Opcode::Ping) || is(op, Opcode::TargetReq) ||
               is(op, Opcode::AttackReq) || is(op, Opcode::AttackStopReq) || is(op, Opcode::SkillReq) ||
               is(op, Opcode::RespawnReq) || is(op, Opcode::StanceReq) || isLootRequest(op) ||
               is(op, Opcode::StatAllocateReq) || is(op, Opcode::SkillLearnReq) || is(op, Opcode::SkillBarSetReq) ||
               is(op, Opcode::ZoneLoadedReq) || isShopRequest(op) || is(op, Opcode::ItemUseReq) ||
               isUpgradeRequest(op);
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
      m_id(id), m_overCapacity(overCapacity), m_address(m_conn->remoteAddress()), m_upgradeRng(std::random_device{}())
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
    m_statQueue.clear();   // the same for stat allocations
    m_skillQueue.clear();  // and for skill learns / bar changes
    m_zoneLoading = false;
    m_zoneAwaitLoaded = false;
    // Loot grants dropped here (or still in a batch on its way) go back to their containers; the
    // one being written stays reserved until its settle (PROTOCOL_LOOT.md §3.4).
    world::LeaveCommand leave{m_id, std::move(done), {}};
    if (m_lootInFlight != 0)
        leave.lootInFlight.push_back(m_lootInFlight);
    m_ctx->world.post(std::move(leave));
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
    if ((isCombatRequest(opcode) || isLootRequest(opcode) || is(opcode, Opcode::StatAllocateReq) ||
         is(opcode, Opcode::SkillLearnReq) || is(opcode, Opcode::SkillBarSetReq) || is(opcode, Opcode::ZoneLoadedReq) ||
         isShopRequest(opcode) || is(opcode, Opcode::ItemUseReq) || isUpgradeRequest(opcode)) &&
        isStaleMove(m_lastReceiveAt))
    {
        // Like MOVE_REQ (§5.4 "Leaving"): a combat request the client sent before it knew it left.
        VO_LOG_DEBUG("session {}: dropping {} sent before leaving the world", m_id,
                     vn::opcodeName(static_cast<Opcode>(opcode)));
        return;
    }
    if (m_state == State::InWorld && m_zoneLoading &&
        (is(opcode, Opcode::MoveReq) || isCombatRequest(opcode) || isLootRequest(opcode) || isShopRequest(opcode) ||
         is(opcode, Opcode::ItemUseReq) || isUpgradeRequest(opcode)))
    {
        // PROTOCOL_ZONES.md §3.4: sent before the client knew it changed zone (or while loading).
        // Dropped silently, but still counted against the flood caps.
        const auto op = static_cast<Opcode>(opcode);
        const bool counted = op == Opcode::AttackReq || op == Opcode::SkillReq || op == Opcode::TargetReq ||
                             op == Opcode::StanceReq;
        if ((is(opcode, Opcode::MoveReq) && !m_moveLimiter.allow(m_lastReceiveAt)) ||
            (counted && !m_combatLimiter.allow(m_lastReceiveAt)) ||
            (isLootRequest(opcode) && !m_lootLimiter.allow(m_lastReceiveAt)) ||
            (isShopRequest(opcode) && !m_shopLimiter.allow(m_lastReceiveAt)) ||
            (is(opcode, Opcode::ItemUseReq) && !m_useLimiter.allow(m_lastReceiveAt)) ||
            (isUpgradeRequest(opcode) && !m_upgradeLimiter.allow(m_lastReceiveAt)))
        {
            kick(vn::ErrorCode::Flood, "too many requests while changing zone");
            return;
        }
        if (m_zoneDropped++ == 0)
            VO_LOG_DEBUG("session {}: dropping {} while changing zone", m_id, vn::opcodeName(op));
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
    case Opcode::TargetReq:
    case Opcode::AttackReq:
    case Opcode::AttackStopReq:
    case Opcode::SkillReq:
    case Opcode::RespawnReq:
    case Opcode::StanceReq: handleCombat(opcode, payload, receivedAt); break;
    case Opcode::LootOpenReq:
    case Opcode::LootTakeReq:
    case Opcode::LootCloseReq: handleLoot(opcode, payload, receivedAt); break;
    case Opcode::StatAllocateReq: handleStatAllocate(payload, receivedAt); break;
    case Opcode::SkillLearnReq:
    case Opcode::SkillBarSetReq: handleSkillRequest(opcode, payload, receivedAt); break;
    case Opcode::ZoneLoadedReq: handleZoneLoaded(payload); break;
    case Opcode::ShopOpenReq:
    case Opcode::ShopCloseReq:
    case Opcode::ShopBuyReq:
    case Opcode::ShopSellReq: handleShop(opcode, payload, receivedAt); break;
    case Opcode::ItemUseReq: handleItemUse(payload, receivedAt); break;
    case Opcode::UpgradeReq: handleUpgrade(payload, receivedAt); break;
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
    // Testing (world.testing_enter_coins, M16): top the balance up before the entry, stored like a gain.
    const std::uint32_t floor = std::min(m_ctx->settings.testingEnterCoins, vn::kMaxCoins);
    if (floor > 0 && character.coins < floor)
    {
        auth::ItemWrite write;
        write.coinsAdded = floor - character.coins;
        const auth::CharacterId id = character.id;
        m_ctx->accounts.applyItemWrite(
            m_accountId, id, std::move(write),
            onExecutor<auth::ItemWriteOutcome>([character](Session& s, auth::ItemWriteOutcome out) {
                if (s.m_state != State::CharSelect)
                {
                    s.m_busy = false;
                    return;
                }
                auth::CharacterInfo c = character;
                if (out.ok)
                {
                    VO_LOG_INFO("session {}: character {} enters with {} Akçe (world.testing_enter_coins)", s.m_id, c.id,
                                out.coins);
                    c.coins = out.coins;
                }
                s.loadItemsFor(std::move(c));
            }));
        return;
    }
    loadItemsFor(std::move(character));
}

void Session::loadItemsFor(auth::CharacterInfo character)
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
    m_statPoints = character.statPoints;
    m_statQueue.clear();
    {
        // PROTOCOL_SKILLS.md §3.5: the stored ranks and bar (slots that are not learned shown empty).
        const game::SkillCatalog& catalog = game::skills();
        m_skillRanks = game::normalizeSkillRanks(catalog, character.charClass, character.skillRanks);
        m_skillBar = game::sanitizeSkillBar(catalog, character.charClass, m_skillRanks,
                                            character.skillBar.value_or(game::defaultSkillBar(catalog, character.charClass)),
                                            m_ctx->items.get()); // PROTOCOL_SHOP.md §4.1: item slots too
        m_skillQueue.clear();
    }
    m_itemUser = {character.charClass, std::max<std::uint16_t>(character.level, 1),
                  game::baseAttributes(character.charClass, m_statPoints)};
    m_visible = game::visibleEquipment(*m_ctx->items, m_inventory);
    m_equipBonus = game::equipmentBonus(*m_ctx->items, m_inventory);
    m_dead = false; // a character that left dead enters alive at its race spawn (§3.5)
    m_itemQueue.clear();
    m_coins = std::min(character.coins, vn::kMaxCoins);
    character.starterKitGranted = true;
    character.mainHandTemplate = m_visible.mainHand;
    character.offHandTemplate = m_visible.offHand;
    character.armorTemplates = m_visible.armor; // M17

    m_characterId = character.id;
    m_worldGeneration = ++m_generationCounter;
    m_worldJoined = true;
    m_enterPending = true;
    m_hasMoveSeq = false;
    m_zoneLoading = false;
    m_zoneAwaitLoaded = false;
    world::EnterCommand cmd;
    cmd.session = m_id;
    cmd.generation = m_worldGeneration;
    cmd.link = std::make_shared<SessionLink>(weak_from_this(), m_executor);
    cmd.account = m_accountId;
    cmd.character = std::move(character);
    cmd.equip = m_equipBonus;
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
        inv.items.push_back(
            {item.place.container, item.place.slot, item.id, item.templateId, item.quantity, item.upgrade});
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
    const auto queuedMoves = static_cast<std::size_t>(std::count_if(
        m_itemQueue.begin(), m_itemQueue.end(), [](const ItemOp& op) { return std::holds_alternative<vn::ItemMoveReq>(op); }));
    if (queuedMoves >= kMaxQueuedItemMoves)
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
        ItemOp op = std::move(m_itemQueue.front());
        m_itemQueue.pop_front();
        if (const auto* grant = std::get_if<world::LootGrant>(&op))
        {
            (void)startLootTake(*grant); // sets m_itemInFlight if it started a write
            continue;
        }
        if (const auto* grant = std::get_if<world::ShopGrant>(&op))
        {
            if (grant->kind == world::ShopGrant::Kind::Upgrade)
                (void)startUpgrade(*grant); // PROTOCOL_UPGRADE.md §2.2
            else
                (void)startShopGrant(*grant); // PROTOCOL_SHOP.md §2.2
            continue;
        }
        if (const auto* grant = std::get_if<world::ItemUseGrant>(&op))
        {
            (void)startItemUse(*grant); // PROTOCOL_SHOP.md §2.3
            continue;
        }
        const vn::ItemMoveReq req = std::get<vn::ItemMoveReq>(op);
        const game::ItemPlace from{req.fromContainer, req.fromSlot};
        const game::ItemPlace to{req.toContainer, req.toSlot};
        if (m_dead)
        {
            // PROTOCOL_COMBAT.md §1.1: no item moves while dead (allowed in combat).
            (void)m_conn->sendMessage(vn::ItemMoveRes{req.seq, vn::ItemMoveResult::SlotBlocked});
            continue;
        }
        game::ItemMovePlan plan = game::planItemMove(*m_ctx->items, m_inventory, m_itemUser, from, to);
        if (plan.result != vn::ItemMoveResult::Ok)
        {
            VO_LOG_TRACE("session {}: item move seq {} ({},{})->({},{}) refused: {}", m_id, req.seq, from.container,
                         from.slot, to.container, to.slot, vn::itemMoveResultName(plan.result));
            (void)m_conn->sendMessage(vn::ItemMoveRes{req.seq, plan.result});
            continue;
        }
        m_itemInFlight = true;
        const std::uint64_t generation = m_worldGeneration;
        const std::uint32_t seq = req.seq;
        if (plan.isMerge())
        {
            // PROTOCOL_LOOT.md §1.2: two quantity changes (or a change and a delete), one transaction.
            auth::ItemWrite write;
            write.quantities = plan.merge;
            m_ctx->accounts.applyItemWrite(
                m_accountId, m_characterId, std::move(write),
                onExecutor<auth::ItemWriteOutcome>(
                    [plan = std::move(plan), seq, generation](Session& s, auth::ItemWriteOutcome out) {
                        s.onItemMoveDone(plan, seq, generation, out.ok);
                    }));
            continue;
        }
        auto relocations = plan.relocations(m_inventory);
        m_ctx->accounts.applyItemMove(m_accountId, m_characterId, std::move(relocations),
                                      onExecutor<bool>([plan = std::move(plan), seq, generation](Session& s, bool ok) {
                                          s.onItemMoveDone(plan, seq, generation, ok);
                                      }));
    }
}

bool Session::startLootTake(const world::LootGrant& grant)
{
    // §3.4 step 3: coins up to the cap, items into stacks first, then free bag slots.
    const std::uint32_t coins = std::min(grant.coins, vn::kMaxCoins - std::min(m_coins, vn::kMaxCoins));
    std::vector<game::ItemAddRequest> requests;
    requests.reserve(grant.items.size());
    for (const auto& i : grant.items)
        requests.push_back({i.templateId, i.quantity});
    game::ItemAddPlan plan = game::planItemAdd(*m_ctx->items, m_inventory, requests);

    // What goes back to the container: on success the units that did not fit, on failure everything.
    world::LootSettleCommand onSuccess{grant.reservation, grant.coins - coins, {}};
    for (std::size_t i = 0; i < grant.items.size(); ++i)
    {
        const std::uint32_t placed = i < plan.placed.size() ? plan.placed[i] : 0;
        if (placed < grant.items[i].quantity)
            onSuccess.itemsReturned.push_back(
                {grant.items[i].index, grant.items[i].templateId,
                 static_cast<std::uint16_t>(grant.items[i].quantity - placed)});
    }
    const world::LootSettleCommand onFailure{grant.reservation, grant.coins, grant.items};
    const vn::LootResult result = !onSuccess.itemsReturned.empty() ? vn::LootResult::BagFull
                                  : coins < grant.coins           ? vn::LootResult::PurseFull
                                                                  : vn::LootResult::Ok;
    if (coins == 0 && plan.empty())
    {
        // Nothing fits: no write, everything goes straight back.
        m_ctx->world.post(onFailure);
        (void)m_conn->sendMessage(vn::LootTakeRes{grant.seq, grant.loot, result});
        return false;
    }

    auth::ItemWrite write;
    write.quantities = plan.grown;
    write.inserts = plan.created;
    write.coinsAdded = coins;
    m_itemInFlight = true;
    m_lootInFlight = grant.reservation;
    const std::uint64_t generation = m_worldGeneration;
    // The world learns the outcome straight from the database thread, so the reservation ends even
    // if this session is gone by then (World::post is thread-safe; the account service is stopped
    // before the world is destroyed). The client is answered on the session's executor.
    auto toSession = onExecutor<auth::ItemWriteOutcome>(
        [plan = std::move(plan), grant, coins, result, generation](Session& s, auth::ItemWriteOutcome out) {
            s.m_itemInFlight = false;
            if (s.m_lootInFlight == grant.reservation)
                s.m_lootInFlight = 0;
            if (s.m_state != State::InWorld || generation != s.m_worldGeneration)
            {
                // Left the world meanwhile: the next entry loads the committed items and balance.
                s.processItemMoves();
                return;
            }
            if (!out.ok)
            {
                VO_LOG_WARN("session {}: loot take seq {} of character {} was not stored", s.m_id, grant.seq,
                            s.m_characterId);
                (void)s.m_conn->sendMessage(vn::LootTakeRes{grant.seq, grant.loot, vn::LootResult::ServerError});
                s.processItemMoves();
                return;
            }
            const auto changes = plan.changes(s.m_inventory, out.newIds);
            game::applyItemAdd(s.m_inventory, plan, out.newIds);
            s.m_coins = out.coins;
            s.sendItemChanges(changes);
            if (coins > 0)
                (void)s.m_conn->sendMessage(vn::Coins{s.m_coins, coins});
            (void)s.m_conn->sendMessage(vn::LootTakeRes{grant.seq, grant.loot, result});
            s.processItemMoves();
        });
    m_ctx->accounts.applyItemWrite(
        m_accountId, m_characterId, std::move(write),
        [ctx = m_ctx, onSuccess = std::move(onSuccess), onFailure, toSession = std::move(toSession)](
            auth::ItemWriteOutcome out) mutable {
            ctx->world.post(out.ok ? std::move(onSuccess) : std::move(onFailure));
            toSession(std::move(out));
        });
    return true;
}

void Session::sendItemChanges(const std::vector<game::ItemSlotChange>& changes)
{
    vn::ItemUpdate update;
    for (const auto& c : changes)
    {
        update.items.push_back(toEntry(c.place, c.content));
        if (update.items.size() == vn::kMaxItemUpdateEntries)
        {
            (void)m_conn->sendMessage(update);
            update.items.clear();
        }
    }
    if (!update.items.empty())
        (void)m_conn->sendMessage(update);
}

void Session::handleLoot(std::uint16_t opcode, std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    using K = world::LootCommand::Kind;
    const auto op = static_cast<Opcode>(opcode);
    // §5: LOOT_OPEN_REQ / LOOT_TAKE_REQ / LOOT_CLOSE_REQ share one cap of 10 per second.
    if (!m_lootLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} loot requests per second from {}", m_id, world::LootRateLimiter::kLimit,
                    m_address);
        kick(vn::ErrorCode::Flood, "too many loot requests");
        return;
    }
    world::LootCommand cmd;
    cmd.session = m_id;
    bool ok = false;
    switch (op)
    {
    case Opcode::LootOpenReq:
    {
        vn::LootOpenReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Open;
        cmd.loot = m.lootId;
        break;
    }
    case Opcode::LootTakeReq:
    {
        vn::LootTakeReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Take;
        cmd.loot = m.lootId;
        cmd.seq = m.seq;
        cmd.what = m.what;
        break;
    }
    case Opcode::LootCloseReq:
    {
        vn::LootCloseReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Close;
        cmd.loot = m.lootId;
        break;
    }
    default: break;
    }
    if (!ok)
    {
        protocolViolation(std::string("malformed ") + std::string(vn::opcodeName(op)));
        return;
    }
    if (m_leavePending)
        return; // the player is already on its way out of the world
    m_ctx->world.post(std::move(cmd));
}

// ---- Merchants and consumables (PROTOCOL_SHOP.md §2.2, §2.3) ----

void Session::handleShop(std::uint16_t opcode, std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    using K = world::ShopCommand::Kind;
    const auto op = static_cast<Opcode>(opcode);
    // §6: SHOP_OPEN / CLOSE / BUY / SELL_REQ share one cap of 10 per second.
    if (!m_shopLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} shop requests per second from {}", m_id, world::ShopRateLimiter::kLimit,
                    m_address);
        kick(vn::ErrorCode::Flood, "too many shop requests");
        return;
    }
    world::ShopCommand cmd;
    cmd.session = m_id;
    bool ok = false;
    switch (op)
    {
    case Opcode::ShopOpenReq:
    {
        vn::ShopOpenReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Open;
        cmd.npc = m.npcId;
        break;
    }
    case Opcode::ShopCloseReq:
    {
        vn::ShopCloseReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Close;
        cmd.npc = m.npcId;
        break;
    }
    case Opcode::ShopBuyReq:
    {
        vn::ShopBuyReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Buy;
        cmd.seq = m.seq;
        cmd.npc = m.npcId;
        cmd.templateId = m.templateId;
        cmd.quantity = m.quantity;
        break;
    }
    case Opcode::ShopSellReq:
    {
        vn::ShopSellReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Sell;
        cmd.seq = m.seq;
        cmd.npc = m.npcId;
        cmd.bagSlot = m.bagSlot;
        cmd.quantity = m.quantity;
        break;
    }
    default: break;
    }
    if (!ok)
    {
        protocolViolation(std::string("malformed ") + std::string(vn::opcodeName(op)));
        return;
    }
    if (m_leavePending)
        return; // the player is already on its way out of the world
    m_ctx->world.post(std::move(cmd));
}

void Session::handleItemUse(std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    if (!m_useLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} ITEM_USE_REQ per second from {}", m_id, world::ItemUseRateLimiter::kLimit,
                    m_address);
        kick(vn::ErrorCode::Flood, "too many ITEM_USE_REQ");
        return;
    }
    vn::ItemUseReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed ITEM_USE_REQ");
        return;
    }
    if (m_leavePending)
        return;
    // §2.3 checks 1-2 against the committed bag; the world checks 3-6 and starts the cooldown.
    const vn::ItemUseResult check = game::checkItemUseStack(*m_ctx->items, m_inventory, req.templateId, req.bagSlot);
    if (check != vn::ItemUseResult::Ok)
    {
        (void)m_conn->sendMessage(vn::ItemUseRes{req.seq, check, req.templateId});
        return;
    }
    const game::ItemTemplate* t = m_ctx->items->find(req.templateId);
    world::ItemUseCommand cmd;
    cmd.session = m_id;
    cmd.kind = world::ItemUseCommand::Kind::Use;
    cmd.seq = req.seq;
    cmd.templateId = req.templateId;
    cmd.bagSlot = req.bagSlot;
    cmd.restoreHp = t->restoreHp;
    cmd.restoreMp = t->restoreMp;
    m_ctx->world.post(cmd);
}

bool Session::startShopGrant(const world::ShopGrant& grant)
{
    using R = vn::ShopResult;
    const std::uint64_t generation = m_worldGeneration;
    if (grant.kind == world::ShopGrant::Kind::Buy)
    {
        game::ShopBuyPlan plan =
            game::planShopBuy(*m_ctx->items, m_inventory, m_coins, grant.templateId, grant.quantity, grant.unitPrice);
        if (plan.result != R::Ok)
        {
            VO_LOG_TRACE("session {}: buy seq {} of {} x {} refused: {}", m_id, grant.seq, grant.quantity,
                         grant.templateId, vn::shopResultName(plan.result));
            (void)m_conn->sendMessage(vn::ShopBuyRes{grant.seq, plan.result, grant.templateId, grant.quantity, 0});
            return false;
        }
        auth::ItemWrite write;
        write.quantities = plan.add.grown;
        write.inserts = plan.add.created;
        write.coinsRemoved = plan.cost;
        m_itemInFlight = true;
        applyShopWrite(
            kTestingFailBuy, grant.templateId, std::move(write),
            onExecutor<auth::ItemWriteOutcome>(
                [plan = std::move(plan), grant, generation](Session& s, auth::ItemWriteOutcome out) {
                    s.m_itemInFlight = false;
                    if (s.m_state != State::InWorld || generation != s.m_worldGeneration)
                    {
                        s.processItemMoves(); // left meanwhile: the next entry loads the committed state
                        return;
                    }
                    if (!out.ok)
                    {
                        VO_LOG_WARN("session {}: buy seq {} of character {} was not stored", s.m_id, grant.seq,
                                    s.m_characterId);
                        (void)s.m_conn->sendMessage(
                            vn::ShopBuyRes{grant.seq, R::ServerError, grant.templateId, grant.quantity, 0});
                        s.processItemMoves();
                        return;
                    }
                    const auto changes = plan.add.changes(s.m_inventory, out.newIds);
                    game::applyItemAdd(s.m_inventory, plan.add, out.newIds);
                    s.m_coins = out.coins;
                    s.sendItemChanges(changes);
                    (void)s.m_conn->sendMessage(vn::Coins{s.m_coins, 0});
                    (void)s.m_conn->sendMessage(
                        vn::ShopBuyRes{grant.seq, R::Ok, grant.templateId, grant.quantity, plan.cost});
                    s.processItemMoves();
                }));
        return true;
    }
    game::ShopSellPlan plan = game::planShopSell(*m_ctx->items, m_inventory, m_coins, grant.bagSlot, grant.quantity);
    if (plan.result != R::Ok)
    {
        VO_LOG_TRACE("session {}: sell seq {} of slot {} refused: {}", m_id, grant.seq, grant.bagSlot,
                     vn::shopResultName(plan.result));
        (void)m_conn->sendMessage(vn::ShopSellRes{grant.seq, plan.result, plan.templateId, grant.quantity, 0});
        return false;
    }
    auth::ItemWrite write;
    write.quantities = {plan.change};
    write.coinsAdded = plan.gained;
    m_itemInFlight = true;
    applyShopWrite(
        kTestingFailSell, plan.templateId, std::move(write),
        onExecutor<auth::ItemWriteOutcome>([plan, grant, generation](Session& s, auth::ItemWriteOutcome out) {
            s.m_itemInFlight = false;
            if (s.m_state != State::InWorld || generation != s.m_worldGeneration)
            {
                s.processItemMoves();
                return;
            }
            if (!out.ok)
            {
                VO_LOG_WARN("session {}: sell seq {} of character {} was not stored", s.m_id, grant.seq,
                            s.m_characterId);
                (void)s.m_conn->sendMessage(vn::ShopSellRes{grant.seq, R::ServerError, plan.templateId, grant.quantity, 0});
                s.processItemMoves();
                return;
            }
            game::ItemStack& stack = s.m_inventory.at(plan.change.place);
            if (plan.change.after == 0)
                stack = {};
            else
                stack.quantity = plan.change.after;
            s.m_coins = out.coins;
            s.sendItemChanges({{plan.change.place, stack}});
            (void)s.m_conn->sendMessage(vn::Coins{s.m_coins, plan.gained});
            (void)s.m_conn->sendMessage(
                vn::ShopSellRes{grant.seq, R::Ok, plan.templateId, grant.quantity, plan.gained});
            s.processItemMoves();
        }));
    return true;
}

bool Session::startItemUse(const world::ItemUseGrant& grant)
{
    // §2.3 check 7: the stack may have gone (sold, moved, used) while the use waited.
    const auto slot = game::findUseStack(m_inventory, grant.templateId, grant.bagSlot);
    if (!slot)
    {
        (void)m_conn->sendMessage(vn::ItemUseRes{grant.seq, vn::ItemUseResult::NoItem, grant.templateId});
        return false;
    }
    const game::ItemPlace place = game::ItemPlace::bag(*slot);
    const game::ItemStack stack = m_inventory.at(place);
    const game::ItemQuantityChange change{stack.id, place, stack.quantity,
                                          static_cast<std::uint16_t>(stack.quantity - 1)};
    auth::ItemWrite write;
    write.quantities = {change};
    m_itemInFlight = true;
    const std::uint64_t generation = m_worldGeneration;
    applyShopWrite(
        kTestingFailUse, grant.templateId, std::move(write),
        onExecutor<auth::ItemWriteOutcome>([change, grant, generation](Session& s, auth::ItemWriteOutcome out) {
            s.m_itemInFlight = false;
            if (s.m_state != State::InWorld || generation != s.m_worldGeneration)
            {
                s.processItemMoves(); // consumed, but the player is gone: nothing to restore
                return;
            }
            if (!out.ok)
            {
                VO_LOG_WARN("session {}: use seq {} of character {} was not stored", s.m_id, grant.seq, s.m_characterId);
                (void)s.m_conn->sendMessage(vn::ItemUseRes{grant.seq, vn::ItemUseResult::ServerError, grant.templateId});
                s.processItemMoves();
                return;
            }
            game::ItemStack& st = s.m_inventory.at(change.place);
            if (change.after == 0)
                st = {};
            else
                st.quantity = change.after;
            s.sendItemChanges({{change.place, st}});
            (void)s.m_conn->sendMessage(vn::ItemUseRes{grant.seq, vn::ItemUseResult::Ok, grant.templateId});
            // Now the world restores (PROTOCOL_SHOP.md §2.3: only after the commit).
            world::ItemUseCommand apply;
            apply.session = s.m_id;
            apply.kind = world::ItemUseCommand::Kind::Apply;
            apply.seq = grant.seq;
            apply.templateId = grant.templateId;
            apply.bagSlot = change.place.slot;
            apply.restoreHp = grant.restoreHp;
            apply.restoreMp = grant.restoreMp;
            s.m_ctx->world.post(apply);
            s.processItemMoves();
        }));
    return true;
}

void Session::applyShopWrite(std::uint8_t kind, std::uint16_t templateId, auth::ItemWrite write,
                             std::function<void(auth::ItemWriteOutcome)> done)
{
    const std::uint16_t failing = m_ctx->settings.testingFailFirstItemWrite;
    if (failing != 0 && templateId == failing && (m_testingFailedWrites & kind) == 0)
    {
        m_testingFailedWrites |= kind;
        VO_LOG_INFO("session {}: write of item {} failed on purpose (world.testing_fail_first_item_write)", m_id,
                    templateId);
        done(auth::ItemWriteOutcome{}); // `done` posts to this session's executor: completes later, as a real write
        return;
    }
    m_ctx->accounts.applyItemWrite(m_accountId, m_characterId, std::move(write), std::move(done));
}

// ---- Item upgrades (PROTOCOL_UPGRADE.md §2.2) ----

void Session::handleUpgrade(std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    if (!m_upgradeLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} UPGRADE_REQ per second from {}", m_id, world::UpgradeRateLimiter::kLimit,
                    m_address);
        kick(vn::ErrorCode::Flood, "too many UPGRADE_REQ");
        return;
    }
    vn::UpgradeReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed UPGRADE_REQ");
        return;
    }
    if (m_leavePending)
        return; // the player is already on its way out of the world
    // §2.2 checks 1-4 (open smith, alive, range) in the world; the rest here, against the committed state.
    world::ShopCommand cmd;
    cmd.session = m_id;
    cmd.kind = world::ShopCommand::Kind::Upgrade;
    cmd.seq = req.seq;
    cmd.npc = req.npcId;
    cmd.bagSlot = req.bagSlot;
    cmd.itemId = req.itemId;
    cmd.fromLevel = req.fromLevel;
    cmd.stoneSlot = req.stoneSlot;
    m_ctx->world.post(std::move(cmd));
}

bool Session::startUpgrade(const world::ShopGrant& grant)
{
    using U = vn::UpgradeResult;
    game::UpgradePlan plan = game::planUpgrade(*m_ctx->items, m_inventory, m_coins, grant.bagSlot, grant.itemId,
                                               grant.fromLevel, grant.stoneSlot);
    if (plan.result != U::Success)
    {
        VO_LOG_DEBUG("session {}: upgrade seq {} of bag slot {} refused: {}", m_id, grant.seq, grant.bagSlot,
                     vn::upgradeResultName(plan.result));
        (void)m_conn->sendMessage(vn::UpgradeRes{grant.seq, plan.result, grant.bagSlot, plan.templateId, plan.level, 0, 0});
        return false;
    }
    // §2.3: the server's roll (0..999), unless the testing hook forces the outcome: targets below its
    // threshold succeed (roll 0), the others fail (roll 999 - a target that cannot fail still succeeds).
    const std::uint8_t failFrom = m_ctx->settings.testingUpgradeFailFrom;
    const auto roll = failFrom == 0 ? static_cast<std::uint16_t>(std::uniform_int_distribution<int>(0, 999)(m_upgradeRng))
                      : plan.target < failFrom ? std::uint16_t{0}
                                               : std::uint16_t{999};
    const U outcome = game::upgradeOutcome(plan, roll);
    const game::ItemStack itemAfter = game::itemAfterUpgrade(plan, outcome);
    const game::ItemStack stoneAfter = game::stoneAfterUpgrade(plan);

    auth::ItemWrite write;
    write.quantities = {{plan.stone.id, plan.stonePlace, plan.stone.quantity, stoneAfter.quantity}};
    write.coinsRemoved = plan.fee;
    if (outcome == U::Success)
        write.upgrades = {{plan.item.id, plan.level, plan.target, false}};
    else if (outcome == U::Destroyed)
        write.upgrades = {{plan.item.id, plan.level, 0, true}};
    m_itemInFlight = true;
    const std::uint64_t generation = m_worldGeneration;
    const game::ItemTemplate* t = m_ctx->items->find(plan.templateId);
    const std::string name = t ? t->name : std::string("?");
    applyShopWrite(
        kTestingFailUpgrade, plan.templateId, std::move(write),
        onExecutor<auth::ItemWriteOutcome>([plan, grant, outcome, roll, itemAfter, stoneAfter, name,
                                            generation](Session& s, auth::ItemWriteOutcome out) {
            s.m_itemInFlight = false;
            if (s.m_state != State::InWorld || generation != s.m_worldGeneration)
            {
                s.processItemMoves(); // left meanwhile: the next entry loads the committed state
                return;
            }
            if (!out.ok)
            {
                VO_LOG_WARN("session {}: upgrade seq {} of item {} of character {} was not stored", s.m_id, grant.seq,
                            plan.item.id, s.m_characterId);
                (void)s.m_conn->sendMessage(
                    vn::UpgradeRes{grant.seq, U::ServerError, grant.bagSlot, plan.templateId, plan.level, 0, 0});
                s.processItemMoves();
                return;
            }
            // One line per attempt (PROTOCOL_UPGRADE.md §2.6).
            VO_LOG_INFO("upgrade: character {} item {} '{}' (+{} -> +{}): {} (chance {} permille, roll {}, fee {}, "
                        "stone {}), balance {}",
                        s.m_characterId, plan.item.id, name, plan.level, plan.target, vn::upgradeResultName(outcome),
                        plan.chancePermille, roll, plan.fee, plan.stone.templateId, out.coins);
            s.m_inventory.at(plan.itemPlace) = itemAfter;
            s.m_inventory.at(plan.stonePlace) = stoneAfter;
            s.m_coins = out.coins;
            std::vector<game::ItemSlotChange> changes;
            if (outcome != U::Failed)
                changes.push_back({plan.itemPlace, itemAfter});
            changes.push_back({plan.stonePlace, stoneAfter});
            s.sendItemChanges(changes);
            (void)s.m_conn->sendMessage(vn::Coins{s.m_coins, 0});
            const std::uint8_t level = outcome == U::Success ? plan.target : plan.level;
            (void)s.m_conn->sendMessage(vn::UpgradeRes{grant.seq, outcome, grant.bagSlot, plan.templateId, level,
                                                       plan.stone.templateId, plan.fee});
            s.processItemMoves();
        }));
    return true;
}
// ---- Stats (PROTOCOL_STATS.md §3.3) ----

void Session::handleStatAllocate(std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    if (!m_statLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} STAT_ALLOCATE_REQ per second from {}", m_id,
                    world::StatRateLimiter::kLimit, m_address);
        kick(vn::ErrorCode::Flood, "too many STAT_ALLOCATE_REQ");
        return;
    }
    vn::StatAllocateReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed STAT_ALLOCATE_REQ");
        return;
    }
    if (m_leavePending)
        return; // the player is already on its way out of the world
    if (m_statInFlight && m_statQueue.size() >= vn::kMaxQueuedStatAllocations)
    {
        // Only reachable while the database is very slow.
        (void)m_conn->sendMessage(vn::StatAllocateRes{req.seq, vn::StatAllocateResult::ServerError,
                                                      game::unspentPoints(m_itemUser.level, m_statPoints)});
        return;
    }
    m_statQueue.push_back(req);
    processStatAllocations();
}

void Session::processStatAllocations()
{
    while (!m_statInFlight && !m_statQueue.empty() && m_state == State::InWorld && !m_leavePending)
    {
        const vn::StatAllocateReq req = m_statQueue.front();
        m_statQueue.pop_front();
        const std::uint16_t level = m_itemUser.level;
        const vn::StatAllocateResult check =
            game::checkAllocation(m_itemUser.charClass, level, m_statPoints, req.points);
        if (check != vn::StatAllocateResult::Ok)
        {
            VO_LOG_TRACE("session {}: stat allocation seq {} refused: {}", m_id, req.seq,
                         vn::statAllocateResultName(check));
            (void)m_conn->sendMessage(
                vn::StatAllocateRes{req.seq, check, game::unspentPoints(level, m_statPoints)});
            continue;
        }
        m_statInFlight = true;
        const std::uint64_t generation = m_worldGeneration;
        const std::uint32_t seq = req.seq;
        auth::StatAllocation write;
        write.points = req.points;
        write.budget = game::pointsForLevel(level);
        write.classStart = game::classStartAttributes(m_itemUser.charClass);
        m_ctx->accounts.allocateStats(
            m_accountId, m_characterId, write,
            onExecutor<auth::StatAllocationOutcome>([seq, generation](Session& s, auth::StatAllocationOutcome out) {
                s.m_statInFlight = false;
                if (s.m_state != State::InWorld || generation != s.m_worldGeneration)
                {
                    // Left the world meanwhile: the next entry loads the stored allocation.
                    s.processStatAllocations();
                    return;
                }
                if (!out.ok)
                {
                    VO_LOG_WARN("session {}: stat allocation seq {} of character {} was not stored", s.m_id, seq,
                                s.m_characterId);
                    (void)s.m_conn->sendMessage(vn::StatAllocateRes{
                        seq, vn::StatAllocateResult::ServerError, game::unspentPoints(s.m_itemUser.level, s.m_statPoints)});
                    s.processStatAllocations();
                    return;
                }
                s.m_statPoints = out.allocated;
                s.m_itemUser.attributes = game::baseAttributes(s.m_itemUser.charClass, s.m_statPoints);
                (void)s.m_conn->sendMessage(vn::StatAllocateRes{seq, vn::StatAllocateResult::Ok,
                                                                game::unspentPoints(s.m_itemUser.level, s.m_statPoints)});
                // The world recomputes the stats and sends CHAR_STATS in its next tick.
                s.m_ctx->world.post(world::StatsCommand{s.m_id, s.m_statPoints});
                s.processStatAllocations();
            }));
    }
}

// ---- Skills (PROTOCOL_SKILLS.md §3.3, §3.5) ----

void Session::sendSkillState()
{
    const game::SkillCatalog& catalog = game::skills();
    vn::SkillState s;
    s.points = static_cast<std::uint16_t>(std::min<std::uint32_t>(game::skillPointsForLevel(catalog, m_itemUser.level), 65535));
    s.unspent = game::unspentSkillPoints(catalog, m_itemUser.charClass, m_itemUser.level, m_skillRanks);
    for (std::uint16_t id = 1; id <= game::kMaxSkillId && s.skills.size() < vn::kMaxSkillStateEntries; ++id)
        if (m_skillRanks[id] > 0)
            s.skills.push_back({id, m_skillRanks[id]});
    (void)m_conn->sendMessage(s);
}

void Session::handleSkillRequest(std::uint16_t opcode, std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    if (!m_skillLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} skill requests per second from {}", m_id,
                    world::SkillRateLimiter::kLimit, m_address);
        kick(vn::ErrorCode::Flood, "too many skill requests");
        return;
    }
    SkillOp op;
    if (is(opcode, Opcode::SkillLearnReq))
    {
        vn::SkillLearnReq req;
        if (!vn::decodeMessage(payload, req))
        {
            protocolViolation("malformed SKILL_LEARN_REQ");
            return;
        }
        op = req;
    }
    else
    {
        vn::SkillBarSetReq req;
        if (!vn::decodeMessage(payload, req))
        {
            protocolViolation("malformed SKILL_BAR_SET_REQ");
            return;
        }
        op = req;
    }
    if (m_leavePending)
        return; // the player is already on its way out of the world
    if (m_skillInFlight && m_skillQueue.size() >= vn::kMaxQueuedSkillRequests)
    {
        // Only reachable while the database is very slow.
        const game::SkillCatalog& catalog = game::skills();
        if (const auto* learn = std::get_if<vn::SkillLearnReq>(&op))
        {
            const game::SkillDef* s = catalog.find(learn->skillId);
            const std::uint8_t rank = s && s->charClass == m_itemUser.charClass ? m_skillRanks[s->id] : 0;
            (void)m_conn->sendMessage(vn::SkillLearnRes{
                learn->seq, learn->skillId, vn::SkillLearnResult::ServerError, rank,
                game::unspentSkillPoints(catalog, m_itemUser.charClass, m_itemUser.level, m_skillRanks)});
        }
        else
        {
            (void)m_conn->sendMessage(vn::SkillBarSetRes{std::get<vn::SkillBarSetReq>(op).seq,
                                                         vn::SkillBarResult::ServerError, m_skillBar});
        }
        return;
    }
    m_skillQueue.push_back(op);
    processSkillRequests();
}

void Session::processSkillRequests()
{
    const game::SkillCatalog& catalog = game::skills();
    while (!m_skillInFlight && !m_skillQueue.empty() && m_state == State::InWorld && !m_leavePending)
    {
        const SkillOp op = m_skillQueue.front();
        m_skillQueue.pop_front();
        const std::uint8_t cls = m_itemUser.charClass;
        const std::uint64_t generation = m_worldGeneration;
        if (const auto* learn = std::get_if<vn::SkillLearnReq>(&op))
        {
            const std::uint16_t level = m_itemUser.level;
            const vn::SkillLearnResult check = game::checkSkillLearn(catalog, cls, level, m_skillRanks, learn->skillId);
            const auto rankOf = [&catalog, cls, id = learn->skillId](const game::SkillRanks& r) {
                const game::SkillDef* s = catalog.find(id);
                return s && s->charClass == cls ? r[s->id] : std::uint8_t{0};
            };
            if (check != vn::SkillLearnResult::Ok)
            {
                VO_LOG_TRACE("session {}: skill learn seq {} ({}) refused: {}", m_id, learn->seq, learn->skillId,
                             vn::skillLearnResultName(check));
                (void)m_conn->sendMessage(vn::SkillLearnRes{learn->seq, learn->skillId, check, rankOf(m_skillRanks),
                                                            game::unspentSkillPoints(catalog, cls, level, m_skillRanks)});
                continue;
            }
            m_skillInFlight = true;
            const vn::SkillLearnReq req = *learn;
            m_ctx->accounts.learnSkill(
                m_accountId, m_characterId, auth::SkillLearn{req.skillId, cls, level},
                onExecutor<auth::SkillLearnOutcome>([req, generation, rankOf](Session& s, auth::SkillLearnOutcome out) {
                    s.m_skillInFlight = false;
                    if (s.m_state != State::InWorld || generation != s.m_worldGeneration)
                    {
                        s.processSkillRequests(); // left meanwhile: the next entry loads the stored ranks
                        return;
                    }
                    const game::SkillCatalog& cat = game::skills();
                    if (out.result == vn::SkillLearnResult::ServerError)
                    {
                        VO_LOG_WARN("session {}: skill learn seq {} of character {} was not stored", s.m_id, req.seq,
                                    s.m_characterId);
                    }
                    else
                    {
                        s.m_skillRanks = out.ranks; // the stored truth (e.g. after a reset-skills)
                        s.m_skillBar = game::sanitizeSkillBar(cat, s.m_itemUser.charClass, s.m_skillRanks, s.m_skillBar,
                                                              s.m_ctx->items.get());
                    }
                    (void)s.m_conn->sendMessage(vn::SkillLearnRes{
                        req.seq, req.skillId, out.result, rankOf(s.m_skillRanks),
                        game::unspentSkillPoints(cat, s.m_itemUser.charClass, s.m_itemUser.level, s.m_skillRanks)});
                    if (out.result != vn::SkillLearnResult::ServerError)
                    {
                        s.sendSkillState();
                        s.m_ctx->world.post(world::SkillsCommand{s.m_id, s.m_skillRanks});
                    }
                    s.processSkillRequests();
                }));
            continue;
        }
        const vn::SkillBarSetReq req = std::get<vn::SkillBarSetReq>(op);
        if (!game::validSkillBar(catalog, cls, m_skillRanks, req.slots, m_ctx->items.get()))
        {
            (void)m_conn->sendMessage(vn::SkillBarSetRes{req.seq, vn::SkillBarResult::NotLearned, m_skillBar});
            continue;
        }
        m_skillInFlight = true;
        m_ctx->accounts.setSkillBar(
            m_accountId, m_characterId, auth::SkillBarWrite{req.slots, cls},
            onExecutor<auth::SkillBarOutcome>([req, generation](Session& s, auth::SkillBarOutcome out) {
                s.m_skillInFlight = false;
                if (s.m_state != State::InWorld || generation != s.m_worldGeneration)
                {
                    s.processSkillRequests();
                    return;
                }
                if (out.result == vn::SkillBarResult::ServerError)
                    VO_LOG_WARN("session {}: skill bar seq {} of character {} was not stored", s.m_id, req.seq,
                                s.m_characterId);
                else
                    s.m_skillBar = out.bar;
                (void)s.m_conn->sendMessage(vn::SkillBarSetRes{req.seq, out.result, s.m_skillBar});
                s.processSkillRequests();
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
    const game::EquipBonus bonus = game::equipmentBonus(*m_ctx->items, m_inventory);
    if (visible != m_visible || !(bonus == m_equipBonus))
    {
        m_visible = visible;
        m_equipBonus = bonus;
        m_ctx->world.post(world::EquipCommand{m_id, visible.mainHand, visible.offHand, bonus, visible.armor});
    }
    processItemMoves();
}

void Session::handleCombat(std::uint16_t opcode, std::span<const std::uint8_t> payload, core::MonoTime receivedAt)
{
    using K = world::CombatCommand::Kind;
    const auto op = static_cast<Opcode>(opcode);
    // §4: ATTACK_REQ / SKILL_REQ / TARGET_REQ / STANCE_REQ share one cap of 10 per second.
    if ((op == Opcode::AttackReq || op == Opcode::SkillReq || op == Opcode::TargetReq || op == Opcode::StanceReq) &&
        !m_combatLimiter.allow(receivedAt))
    {
        VO_LOG_INFO("session {}: more than {} combat requests per second from {}", m_id,
                    world::CombatRateLimiter::kLimit, m_address);
        kick(vn::ErrorCode::Flood, "too many combat requests");
        return;
    }
    if (m_leavePending)
        return; // the player is already on its way out of the world
    world::CombatCommand cmd;
    cmd.session = m_id;
    bool ok = false;
    switch (op)
    {
    case Opcode::TargetReq:
    {
        vn::TargetReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Target;
        cmd.target = m.entityId;
        break;
    }
    case Opcode::AttackReq:
    {
        vn::AttackReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Attack;
        cmd.target = m.entityId;
        break;
    }
    case Opcode::AttackStopReq:
    {
        vn::AttackStopReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::AttackStop;
        break;
    }
    case Opcode::SkillReq:
    {
        vn::SkillReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Skill;
        cmd.skill = m.skillId;
        cmd.target = m.targetId;
        break;
    }
    case Opcode::RespawnReq:
    {
        vn::RespawnReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Respawn;
        break;
    }
    case Opcode::StanceReq:
    {
        vn::StanceReq m;
        ok = vn::decodeMessage(payload, m);
        cmd.kind = K::Stance;
        cmd.stance = m.stance;
        break;
    }
    default: break;
    }
    if (!ok)
    {
        protocolViolation(std::string("malformed ") + std::string(vn::opcodeName(op)));
        return;
    }
    m_ctx->world.post(std::move(cmd));
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
    if (completesEnter && batch.entered->zoneChange)
    {
        // PROTOCOL_ZONES.md §3.3 step 4: the arrival, then what a fresh entry gets.
        if (m_state != State::InWorld || !m_zoneLoading)
            return;
        const world::EnteredInfo& e = *batch.entered;
        m_entityId = e.entityId;
        vn::ZoneChange zc;
        zc.result = e.zoneChangeResult;
        zc.fromZone = e.fromZone;
        vn::EnterWorldRes& res = zc.enter;
        res.entityId = e.entityId;
        res.zoneId = e.zoneId;
        res.x = e.x;
        res.y = e.y;
        res.z = e.z;
        res.yaw = e.yaw;
        res.walkSpeed = e.walkSpeed;
        res.runSpeed = e.runSpeed;
        res.serverTimeMs = e.serverTimeMs;
        res.level = e.level;
        res.xp = e.xp;
        res.xpToNext = e.xpToNext;
        res.hp = e.hp;
        res.maxHp = e.maxHp;
        res.mp = e.mp;
        res.maxMp = e.maxMp;
        res.attack = e.attack;
        res.defense = e.defense;
        res.isDead = e.dead;
        (void)m_conn->sendMessage(zc);
        sendInventory();
        (void)m_conn->sendMessage(vn::Coins{m_coins, 0});
        m_itemUser.level = std::max<std::uint16_t>(e.level, 1);
        sendSkillState();
        (void)m_conn->sendMessage(vn::SkillBar{m_skillBar});
        m_dead = e.dead;
        m_zoneAwaitLoaded = true;
        m_zoneLoadingId = e.zoneId;
        VO_LOG_INFO("session {}: character {} {} zone {} as entity {} at ({:.1f}, {:.1f}, {:.1f})", m_id,
                    m_characterId, e.zoneChangeResult == vn::ZoneChangeResult::Ok ? "changed to" : "stays in (failed change)",
                    e.zoneId, e.entityId, e.x, e.y, e.z);
    }
    else if (completesEnter)
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
        res.level = e.level;
        res.xp = e.xp;
        res.xpToNext = e.xpToNext;
        res.hp = e.hp;
        res.maxHp = e.maxHp;
        res.mp = e.mp;
        res.maxMp = e.maxMp;
        res.attack = e.attack;
        res.defense = e.defense;
        res.isDead = e.dead;
        setState(State::InWorld);
        (void)m_conn->sendMessage(res);
        sendInventory(); // PROTOCOL_ITEMS.md §2: right after ENTER_WORLD_RES, before any ENTITY_SPAWN
        (void)m_conn->sendMessage(vn::Coins{m_coins, 0}); // PROTOCOL_LOOT.md §4.3: then the balance
        // The world's level (a recently-left character may be ahead of the stored one).
        m_itemUser.level = std::max<std::uint16_t>(e.level, 1);
        sendSkillState(); // PROTOCOL_SKILLS.md §4: then the skills and the bar
        (void)m_conn->sendMessage(vn::SkillBar{m_skillBar});
        VO_LOG_INFO("session {}: character {} entered zone {} as entity {} at ({:.1f}, {:.1f}, {:.1f})", m_id,
                    m_characterId, e.zoneId, e.entityId, e.x, e.y, e.z);
    }
    if (m_state != State::InWorld)
        return;
    if (batch.dead)
        m_dead = *batch.dead;
    bool levelChanged = false;
    if (batch.level)
    {
        levelChanged = !completesEnter && *batch.level != m_itemUser.level;
        m_itemUser.level = *batch.level;
    }
    for (const auto& item : batch.items)
    {
        if (!m_conn->send(item.opcode, batch.payload(item)))
            break; // closing (e.g. the 256 KB outbound cap was exceeded)
    }
    if (levelChanged)
        sendSkillState(); // PROTOCOL_SKILLS.md §4: more unspent points after a level-up
    if (!batch.lootGrants.empty() || !batch.shopGrants.empty() || !batch.useGrants.empty())
    {
        // PROTOCOL_LOOT.md §3.4: decided in order with the item moves, against the committed state.
        for (auto& grant : batch.lootGrants)
            m_itemQueue.emplace_back(std::move(grant));
        // PROTOCOL_SHOP.md §2.2 / §2.3: the same queue; nothing is reserved, so a long queue (a very
        // slow database) answers ServerError at once.
        const auto waiting = [this] { return m_itemQueue.size(); };
        for (const auto& grant : batch.shopGrants)
        {
            if (waiting() >= kMaxQueuedItemMoves)
            {
                if (grant.kind == world::ShopGrant::Kind::Buy)
                    (void)m_conn->sendMessage(vn::ShopBuyRes{grant.seq, vn::ShopResult::ServerError, grant.templateId,
                                                             grant.quantity, 0});
                else if (grant.kind == world::ShopGrant::Kind::Upgrade)
                    (void)m_conn->sendMessage(
                        vn::UpgradeRes{grant.seq, vn::UpgradeResult::ServerError, grant.bagSlot, 0, 0, 0, 0});
                else
                    (void)m_conn->sendMessage(vn::ShopSellRes{grant.seq, vn::ShopResult::ServerError, 0, grant.quantity, 0});
                continue;
            }
            m_itemQueue.emplace_back(grant);
        }
        for (const auto& grant : batch.useGrants)
        {
            if (waiting() >= kMaxQueuedItemMoves)
            {
                (void)m_conn->sendMessage(vn::ItemUseRes{grant.seq, vn::ItemUseResult::ServerError, grant.templateId});
                continue;
            }
            m_itemQueue.emplace_back(grant);
        }
        processItemMoves();
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
    if (batch.zoneChangeBegin)
    {
        // PROTOCOL_ZONES.md §3.3 step 2: after the old zone's last packets.
        (void)m_conn->sendMessage(*batch.zoneChangeBegin);
        m_zoneLoading = true;
        m_zoneAwaitLoaded = false;
        m_zoneDropped = 0;
    }
    if (completesEnter && !batch.entered->zoneChange)
    {
        m_busy = false;
        drainDeferred();
    }
}

void Session::handleZoneLoaded(std::span<const std::uint8_t> payload)
{
    vn::ZoneLoadedReq req;
    if (!vn::decodeMessage(payload, req))
    {
        protocolViolation("malformed ZONE_LOADED_REQ");
        return;
    }
    if (m_leavePending)
        return; // the player is already on its way out of the world
    if (!m_zoneAwaitLoaded)
    {
        VO_LOG_INFO("session {}: ZONE_LOADED_REQ without a zone change", m_id);
        kick(vn::ErrorCode::BadState, "ZONE_LOADED_REQ without a zone change");
        return;
    }
    if (req.zoneId != m_zoneLoadingId)
    {
        protocolViolation("ZONE_LOADED_REQ for another zone");
        return;
    }
    // §3.4: movement may start again, with a new MOVE_REQ seq sequence.
    m_zoneAwaitLoaded = false;
    m_zoneLoading = false;
    m_hasMoveSeq = false;
    m_ctx->world.post(world::ZoneReadyCommand{m_id});
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
