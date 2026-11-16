// The local character's inventory as the client sees it (M8 "Kuşan", docs/PROTOCOL_ITEMS.md), no D3D,
// no sockets: the confirmed state the server sent (INVENTORY, then ITEM_UPDATE) plus the moves the
// client asked for and has no answer for yet, shown optimistically.
//
//   beginMove(from, to)   plans the move with the shared rules (game::planItemMove). A move the rules
//                         refuse is not sent (the result says why); a legal one gets a seq, becomes
//                         pending and the ITEM_MOVE_REQ to send is returned.
//   view()                confirmed state with every still-unanswered pending move replayed on top
//                         (re-planned in order; a pending move that no longer fits is skipped).
//   ITEM_UPDATE           changes the confirmed state. The server answers in order and sends
//                         ITEM_UPDATE before ITEM_MOVE_RES(Ok), so an update settles the oldest pending
//                         move: it is no longer replayed (else it would apply twice).
//   ITEM_MOVE_RES         removes the pending move with that seq; on a failure its optimistic effect
//                         simply disappears from view() (rollback) and the result is reported.
//
// At most kMaxMovesPerSecond requests are started per rolling second (the server closes the
// connection above 10/s, PROTOCOL_ITEMS.md §4).
#pragma once

#include "shared/game/Items.h"
#include "shared/net/Messages.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace vo::client::world {

class ClientInventory
{
public:
    static constexpr int kMaxMovesPerSecond = 8;

    enum class BeginStatus : std::uint8_t
    {
        Sent,      // request returned, pending
        Refused,   // the shared rules refuse it (see `result`); nothing sent
        Throttled, // too many requests this second; nothing sent
        NotReady,  // no INVENTORY yet
    };
    struct Begin
    {
        BeginStatus status = BeginStatus::NotReady;
        game::ItemMoveResult result = game::ItemMoveResult::Ok; // Refused: why
        std::optional<vo::net::ItemMoveReq> request;
    };
    struct Answer
    {
        std::uint32_t seq = 0;
        game::ItemMoveResult result = game::ItemMoveResult::Ok;
        game::ItemPlace from;
        game::ItemPlace to;
    };

    // The catalog must outlive the inventory; the user (class, level) decides what may be equipped.
    void setRules(const game::ItemCatalog* catalog, game::ItemUser user) noexcept;
    // Forgets everything (entering / leaving the world).
    void reset();

    // INVENTORY: replaces the confirmed state, drops pending moves. False if an entry is outside the
    // places of §1 or two entries name one place (the message is then ignored).
    bool applyInventory(const vo::net::Inventory& msg, std::string& error);
    // ITEM_UPDATE: each entry sets one slot (item id 0 = now empty). Entries with invalid places are
    // skipped (false, but the valid ones are applied).
    bool applyUpdate(const vo::net::ItemUpdate& msg);
    // ITEM_MOVE_RES: the pending move it answers, or nullopt for an unknown seq.
    std::optional<Answer> applyResult(const vo::net::ItemMoveRes& msg);

    // `now` = seconds on any monotonic clock (throttling). `force` sends a move the local rules
    // refuse (automation: tests the server's answer and the rollback); it is not shown optimistically.
    Begin beginMove(game::ItemPlace from, game::ItemPlace to, double now, bool force = false);

    [[nodiscard]] bool loaded() const noexcept { return m_loaded; }
    [[nodiscard]] const game::Inventory& view() const noexcept { return m_view; }
    [[nodiscard]] const game::Inventory& confirmed() const noexcept { return m_confirmed; }
    [[nodiscard]] std::size_t pendingCount() const noexcept { return m_pending.size(); }
    [[nodiscard]] bool hasPending() const noexcept { return !m_pending.empty(); }
    // MainHand / OffHand template ids of view() (what the local character is drawn with).
    [[nodiscard]] game::VisibleEquipment visible() const noexcept;
    [[nodiscard]] const game::ItemUser& user() const noexcept { return m_user; }
    [[nodiscard]] const game::ItemCatalog* catalog() const noexcept { return m_catalog; }
    // Where a double click / right click on `from` would send the item (game::quickMoveTarget on view()).
    [[nodiscard]] std::optional<game::ItemPlace> quickTarget(game::ItemPlace from) const;
    // Result the rules give for moving `from` -> `to` on view() (drop-target highlighting).
    [[nodiscard]] game::ItemMoveResult check(game::ItemPlace from, game::ItemPlace to) const;

private:
    struct Pending
    {
        std::uint32_t seq = 0;
        game::ItemPlace from;
        game::ItemPlace to;
        bool settled = false; // an ITEM_UPDATE already carried its effect
        bool optimistic = true; // false for forced (locally refused) moves
    };
    void rebuildView();

    const game::ItemCatalog* m_catalog = nullptr;
    game::ItemUser m_user;
    game::Inventory m_confirmed;
    game::Inventory m_view;
    std::deque<Pending> m_pending;
    std::deque<double> m_sentTimes;
    std::uint32_t m_nextSeq = 1;
    bool m_loaded = false;
};

} // namespace vo::client::world
