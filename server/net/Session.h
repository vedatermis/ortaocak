// Session: one client connection on the server and its state machine (docs/PROTOCOL.md §4):
//   Handshake -> Auth -> CharSelect <-> InWorld
// Handles the handshake, timeouts (handshake 5 s, login 30 s, idle 30 s), PING, opcode/state
// validation (ERROR(BadState) + close), malformed payloads (ERROR(ProtocolViolation) + close) and the
// login/character requests against IAccountService.
//
// In the world (docs/SERVER_WORLD.md): ENTER_WORLD_REQ loads the character, then hands it to the world
// (EnterCommand); the world's first batch for this session completes the request (ENTER_WORLD_RES,
// then ENTITY_SPAWN of everyone in range). MOVE_REQ is checked here (30/s flood cap, finite and
// plausible values, strictly increasing seq) and posted to the world (MoveCommand) with its receive
// time; validation, corrections and relays happen on the world thread. Leave/logout/disconnect post
// a LeaveCommand; LEAVE_WORLD_RES / the logout close follow once the position is saved.
//
// Items (docs/PROTOCOL_ITEMS.md): ENTER_WORLD_REQ also loads the items (granting the starter kit on
// the first entry); INVENTORY follows ENTER_WORLD_RES before the first world packet. ITEM_MOVE_REQ
// (10/s flood cap) is decided here with the shared rules (game::planItemMove) against the committed
// inventory, written through the account service, and answered (ITEM_UPDATE + ITEM_MOVE_RES) only
// after the commit; a visible-equipment change is then posted to the world (EquipCommand), which
// sends ENTITY_EQUIP to the player and everyone who has it in view.
//
// Threading: every session lives on one of the server's network io_contexts (net.io_threads, each
// run by exactly one thread) and all its members run there. Other threads only use kickAsync() and
// the posted handlers below. Account-service callbacks may arrive on any thread; they are posted back
// onto the connection's executor before touching the session. The world never calls into the
// session directly: its batches are posted onto the same executor. The shared registry in
// ServerContext (sessions, online, counters) is guarded by ServerContext::registryMutex.
// While a service or world request is pending, further requests are queued (max 16, then
// ERROR(Flood)) and processed in order afterwards; PING is answered immediately.
#pragma once

#include "server/auth/IAccountService.h"
#include "server/net/ServerContext.h"
#include "server/world/MoveRateLimiter.h"
#include "server/world/WorldTypes.h"
#include "shared/game/Combat.h"
#include "shared/game/Items.h"
#include "shared/net/Connection.h"
#include "shared/net/Messages.h"
#include "shared/net/Opcodes.h"

#include <asio/any_io_executor.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
    // kick() posted onto this session's executor: callable from any thread.
    void kickAsync(vo::net::ErrorCode code, std::string_view debugMessage);
    [[nodiscard]] asio::any_io_executor executor() const { return m_executor; }

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }
    [[nodiscard]] bool overCapacity() const noexcept { return m_overCapacity; }

    // The world's packets for this session for one tick (posted onto the session's executor by its
    // world link). Batches of an earlier world session (other generation) are dropped.
    void onWorldBatch(world::WorldBatch batch);

private:
    struct Deferred
    {
        std::uint16_t opcode = 0;
        std::vector<std::uint8_t> payload;
        core::MonoTime receivedAt{};
    };

    void onMessage(std::uint16_t opcode, std::span<const std::uint8_t> payload);
    void dispatch(std::uint16_t opcode, std::span<const std::uint8_t> payload, core::MonoTime receivedAt);
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
    void handleMove(std::span<const std::uint8_t> payload, core::MonoTime receivedAt);
    void handleItemMove(std::span<const std::uint8_t> payload, core::MonoTime receivedAt);
    // Combat requests (PROTOCOL_COMBAT.md §3): decoded, flood-checked (§4) and posted to the world.
    void handleCombat(std::uint16_t opcode, std::span<const std::uint8_t> payload, core::MonoTime receivedAt);
    // Loot requests (PROTOCOL_LOOT.md §4): decoded, flood-checked (§5) and posted to the world.
    void handleLoot(std::uint16_t opcode, std::span<const std::uint8_t> payload, core::MonoTime receivedAt);

    void onLoginDone(const auth::LoginOutcome& outcome);
    // ENTER_WORLD: the character is loaded; load (and on first entry grant) its items next.
    void onCharacterLoaded(auth::CharacterInfo character);
    void onItemsLoaded(auth::CharacterInfo character, auth::LoadItemsOutcome items);
    // Items: decides queued ITEM_MOVE_REQs one at a time against the committed inventory; a valid
    // move is written through and answered only after its transaction committed.
    void processItemMoves();
    void onItemMoveDone(const game::ItemMovePlan& plan, std::uint32_t seq, std::uint64_t generation, bool ok);
    // Loot (PROTOCOL_LOOT.md §3.4): places a grant against the committed inventory and balance,
    // writes it through, answers the client and settles the reservation with the world.
    // Returns true if a write was started (the queue then waits for it).
    bool startLootTake(const world::LootGrant& grant);
    void sendInventory();
    // ITEM_UPDATE packets (at most 8 entries each) for these slot changes.
    void sendItemChanges(const std::vector<game::ItemSlotChange>& changes);
    [[nodiscard]] vo::net::CharSummary toSummary(const auth::CharacterInfo& c) const;

    // PROTOCOL §5.4 "Leaving": a MOVE_REQ that was in flight when the client left the world.
    // True while a leave/logout is being processed and, in CharSelect, for kLeaveGrace after the
    // player left; such moves are dropped silently instead of being deferred or answered BadState.
    [[nodiscard]] bool isStaleMove(core::MonoTime now) const noexcept;
    void protocolViolation(std::string_view what);
    void closeSilently(std::string_view why);
    void setState(State s);
    // Hands the player back from the world (despawn + position save); `done` as LeaveCommand.
    void leaveWorld(std::function<void(bool)> done);

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
    core::MonoTime m_lastBusyAt; // last session tick that found a request pending

    auth::AccountId m_accountId = 0;
    auth::CharacterId m_characterId = 0;
    std::uint32_t m_entityId = 0;

    // World session: the player is (or is about to be) in the world while m_worldJoined is set.
    bool m_worldJoined = false;
    bool m_enterPending = false;      // EnterCommand posted, first batch not received yet
    std::uint64_t m_worldGeneration = 0; // batches of other generations are stale
    std::uint64_t m_generationCounter = 0;
    world::MoveRateLimiter m_moveLimiter;
    std::uint32_t m_lastMoveSeq = 0;
    bool m_hasMoveSeq = false;
    bool m_leavePending = false;        // LEAVE_WORLD_REQ / LOGOUT in world posted, not answered yet
    core::MonoTime m_leftWorldAt{};     // when the last LEAVE_WORLD_RES was sent
    bool m_hasLeftWorld = false;
    std::uint32_t m_staleMovesDropped = 0; // in-flight MOVE_REQ dropped after the last leave
    core::MonoTime m_lastQueueWarn{};

    // Items of the character in the world (docs/PROTOCOL_ITEMS.md). m_inventory is the committed
    // state: a move changes it only after its transaction committed. Requests wait in m_itemQueue
    // while one is in flight, so rapid moves are decided in order against up-to-date state.
    game::Inventory m_inventory;
    game::ItemUser m_itemUser;
    game::VisibleEquipment m_visible;
    game::EquipBonus m_equipBonus;
    // Item moves and loot grants share one queue: both change the inventory and are decided one at
    // a time against the committed state. Grants are never refused (their reservation would leak);
    // the move cap counts moves only.
    using ItemOp = std::variant<vo::net::ItemMoveReq, world::LootGrant>;
    std::deque<ItemOp> m_itemQueue;
    bool m_itemInFlight = false;
    world::ItemMoveRateLimiter m_itemLimiter;
    // Loot (PROTOCOL_LOOT.md): the committed Akçe balance, and the reservation whose write is in
    // flight (0 = none); a LeaveCommand lists it so the world keeps it until it is settled.
    std::uint32_t m_coins = 0;
    std::uint64_t m_lootInFlight = 0;
    world::LootRateLimiter m_lootLimiter;

    // Combat (PROTOCOL_COMBAT.md): the world tells the session (in its batches) when the character
    // dies / respawns and when its level changes; item moves are refused while dead.
    world::CombatRateLimiter m_combatLimiter;
    bool m_dead = false;
};

// ITEM_MOVE_REQs waiting behind one in flight; more are answered ServerError at once.
inline constexpr std::size_t kMaxQueuedItemMoves = 16;

// How long after LEAVE_WORLD_RES a MOVE_REQ in CharSelect is still dropped silently (§5.4).
inline constexpr std::chrono::milliseconds kLeaveGrace{2000};

[[nodiscard]] std::string_view sessionStateName(Session::State s) noexcept;
// Whether `opcode` is a valid client opcode in `state` (§4 table).
[[nodiscard]] bool isOpcodeAllowed(Session::State state, std::uint16_t opcode) noexcept;

} // namespace vo::server::net
