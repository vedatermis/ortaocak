// Types at the boundary between the world simulation and the network sessions
// (docs/SERVER_WORLD.md §2): the commands sessions post into the world, the per-tick outbound batch
// the world hands back to each player's link, and the link interface itself.
//
// Threading: commands are created on the network thread and consumed on the world thread; batches
// are produced on the world thread and consumed on the network thread. Both are plain values that
// are moved across, never shared.
#pragma once

#include "server/auth/IAccountService.h"
#include "shared/core/Clock.h"
#include "shared/game/Combat.h"
#include "shared/net/Messages.h"
#include "shared/net/PacketWriter.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace vo::server::world {

using EntityId = std::uint32_t;
using SessionKey = std::uint64_t; // Session::id(), unique per connection

// Everything ENTER_WORLD_RES needs; carried in the first batch of a world session.
struct EnteredInfo
{
    EntityId entityId = 0;
    std::uint16_t zoneId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float walkSpeed = 0.0f;
    float runSpeed = 0.0f;
    std::uint32_t serverTimeMs = 0;
    // Combat state (PROTOCOL_COMBAT.md §2).
    std::uint16_t level = 1;
    std::uint32_t xp = 0;
    std::uint32_t xpToNext = 0;
    std::uint32_t hp = 0;
    std::uint32_t maxHp = 0;
    std::uint32_t mp = 0;
    std::uint32_t maxMp = 0;
    std::uint16_t attack = 0;
    std::uint16_t defense = 0;
    bool dead = false;
    // Zones (PROTOCOL_ZONES.md §3.3): set when this completes a zone change, not a world entry. The
    // session then sends ZONE_CHANGE (result, from_zone) instead of ENTER_WORLD_RES.
    bool zoneChange = false;
    net::ZoneChangeResult zoneChangeResult = net::ZoneChangeResult::Ok;
    std::uint16_t fromZone = 0;
};

// Loot units the world reserved for one LOOT_TAKE_REQ of a player (docs/PROTOCOL_LOOT.md §3.4),
// handed to the player's session in its batch. The session places them, writes them through and
// answers LOOT_TAKE_RES; whatever was not taken goes back with a LootSettleCommand. A grant that
// never reaches its session (it left the world meanwhile) is returned by the LeaveCommand.
struct LootGrant
{
    std::uint64_t reservation = 0; // unique per zone and process
    EntityId loot = 0;             // the container
    std::uint32_t seq = 0;         // LOOT_TAKE_REQ.seq
    std::uint32_t coins = 0;
    struct Item
    {
        std::uint8_t index = 0; // entry index in the container
        std::uint16_t templateId = 0;
        std::uint16_t quantity = 0;
        bool operator==(const Item&) const = default;
    };
    std::vector<Item> items;
};

// A shop request the world accepted (docs/PROTOCOL_SHOP.md §2.2: the shop is open, the player alive
// and in range, the item in stock), handed to the player's session in its batch. The session decides
// the rest against the committed inventory and balance, writes and answers. Nothing is reserved:
// a grant that never reaches its session is simply lost (the client got no answer, nothing changed).
struct ShopGrant
{
    enum class Kind : std::uint8_t
    {
        Buy,
        Sell,
        Upgrade, // v10 (PROTOCOL_UPGRADE.md §2.2): a smith accepted an upgrade attempt
    };
    Kind kind = Kind::Buy;
    std::uint32_t seq = 0;
    EntityId npc = 0;
    std::uint16_t templateId = 0; // buy: what to buy
    std::uint16_t quantity = 0;
    std::uint8_t bagSlot = 0;     // sell: which stack; upgrade: the item's bag slot
    std::uint32_t unitPrice = 0;  // buy: the merchant's price per unit
    // Upgrade: the item the client means, its level as the client knows it, the stone slot.
    std::uint64_t itemId = 0;
    std::uint8_t fromLevel = 0;
    std::uint8_t stoneSlot = 0;
};

// A consumable use the world accepted (PROTOCOL_SHOP.md §2.3: alive, not stunned, the shared
// cooldown was ready and has started, something to restore). The session consumes one unit and,
// after the commit, posts an ItemUseCommand(Apply).
struct ItemUseGrant
{
    std::uint32_t seq = 0;
    std::uint16_t templateId = 0;
    std::uint8_t bagSlot = 0; // as requested (0xFF = any stack)
    std::uint32_t restoreHp = 0;
    std::uint32_t restoreMp = 0;
};

// All packets one player receives in one tick, in send order. Payloads are stored back to back in
// one buffer (one allocation per batch instead of one per packet).
struct WorldBatch
{
    struct Item
    {
        std::uint16_t opcode = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };

    std::uint64_t generation = 0;      // the session's world-session counter (stale batches are dropped)
    std::optional<EnteredInfo> entered; // set in the batch that completes ENTER_WORLD (sent first)
    // PROTOCOL_ZONES.md §3.3: the gate fired; the player is out of its zone until the arrival.
    std::optional<net::ZoneChangeBegin> zoneChangeBegin;
    // Notes for the session itself (not packets): the player died / respawned, or its level changed
    // (item moves are refused while dead; the level decides item requirements).
    std::optional<bool> dead;
    std::optional<std::uint16_t> level;
    std::vector<LootGrant> lootGrants; // handled after the packets of this batch
    std::vector<ShopGrant> shopGrants; // PROTOCOL_SHOP.md §2.2, handled after the packets
    std::vector<ItemUseGrant> useGrants; // PROTOCOL_SHOP.md §2.3, handled after the packets
    std::vector<std::uint8_t> bytes;
    std::vector<Item> items;

    [[nodiscard]] bool empty() const noexcept
    {
        return !entered && !zoneChangeBegin && !dead && !level && lootGrants.empty() && shopGrants.empty() &&
               useGrants.empty() && items.empty();
    }
    [[nodiscard]] std::size_t packetCount() const noexcept { return items.size(); }
    [[nodiscard]] std::span<const std::uint8_t> payload(const Item& item) const noexcept
    {
        return std::span<const std::uint8_t>(bytes).subspan(item.offset, item.size);
    }

    void append(std::uint16_t opcode, std::span<const std::uint8_t> payload)
    {
        items.push_back({opcode, static_cast<std::uint32_t>(bytes.size()), static_cast<std::uint32_t>(payload.size())});
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }

    // Encodes `msg` through `scratch` (reused to avoid allocations) and appends it.
    template <class T>
    void appendMessage(const T& msg, net::PacketWriter& scratch)
    {
        scratch.clear();
        msg.write(scratch);
        if (scratch.ok())
            append(static_cast<std::uint16_t>(T::kOpcode), scratch.data());
    }
};

// The world's handle to one player's connection. Implemented by the network layer (Session) and by
// fakes in the tests.
class IPlayerLink
{
public:
    virtual ~IPlayerLink() = default;
    // Called on the world thread, at most once per tick per player, with everything the player
    // receives this tick. Must not block and must not call back into the world.
    virtual void deliver(WorldBatch batch) = 0;
};

// ---- Commands (network thread -> world thread) ----

struct EnterCommand
{
    SessionKey session = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<IPlayerLink> link;
    auth::AccountId account = 0;
    auth::CharacterInfo character; // as loaded; mainHand/offHandTemplate = the visible equipment
    game::EquipBonus equip;        // attack/defense/attributes of everything equipped (PROTOCOL_COMBAT.md §1.1)
};                                 // (character.statPoints: the stored stat allocation, PROTOCOL_STATS.md)

struct MoveCommand
{
    SessionKey session = 0;
    net::MoveReq request;       // already checked for plausibility (finite, known flags, seq)
    core::MonoTime receivedAt{}; // when the network thread read it (server-side elapsed time)
};

struct LeaveCommand
{
    SessionKey session = 0;
    // Invoked exactly once, on any thread: after the position save completed (true = stored), or
    // at once if the session had no entity in the world. May be empty.
    std::function<void(bool saved)> done;
    // Loot reservations of this session whose write has started: they stay reserved until their
    // LootSettleCommand arrives. Every other reservation of the session is returned to its container.
    std::vector<std::uint64_t> lootInFlight;
};

// The equipment of a player changed (an ITEM_MOVE committed): if the visible part changed,
// ENTITY_EQUIP to the player and to everyone who has it in view (later ENTITY_SPAWNs carry the new
// ids); if the bonuses changed, the stats are recomputed and SELF_STATS goes to the player.
struct EquipCommand
{
    SessionKey session = 0;
    std::uint16_t mainHandTemplate = 0;
    std::uint16_t offHandTemplate = 0;
    game::EquipBonus equip;
    game::ArmorAppearance armor{}; // M17: worn armour Head .. Feet (ENTITY_EQUIP carries it)
};

// A combat request of a player (PROTOCOL_COMBAT.md §3), already decoded and flood-checked.
struct CombatCommand
{
    enum class Kind : std::uint8_t
    {
        Target,     // TARGET_REQ(target)
        Attack,     // ATTACK_REQ(target)
        AttackStop, // ATTACK_STOP_REQ
        Skill,      // SKILL_REQ(skill, target)
        Respawn,    // RESPAWN_REQ
        Stance,     // STANCE_REQ(stance)
    };
    SessionKey session = 0;
    Kind kind = Kind::Target;
    std::uint32_t target = 0;
    std::uint16_t skill = 0;
    std::uint8_t stance = 0;
};

// A loot request of a player (PROTOCOL_LOOT.md §4), already decoded and flood-checked.
struct LootCommand
{
    enum class Kind : std::uint8_t
    {
        Open,  // LOOT_OPEN_REQ(loot)
        Take,  // LOOT_TAKE_REQ(seq, loot, what)
        Close, // LOOT_CLOSE_REQ(loot)
    };
    SessionKey session = 0;
    Kind kind = Kind::Open;
    EntityId loot = 0;
    std::uint32_t seq = 0;
    std::uint8_t what = 0;
};

// The end of a loot reservation (from the session that received the grant, or on its behalf when
// it is gone): the units that were NOT taken go back into the container. Nothing returned = all
// of it was committed to the database.
struct LootSettleCommand
{
    std::uint64_t reservation = 0;
    std::uint32_t coinsReturned = 0;
    std::vector<LootGrant::Item> itemsReturned; // index + quantity going back (templateId informational)
};

// The stored stat allocation of a player changed (a STAT_ALLOCATE_REQ committed; PROTOCOL_STATS.md
// §3.3): the world recomputes the stats and sends CHAR_STATS (and SELF_STATS) to the player.
struct StatsCommand
{
    SessionKey session = 0;
    game::Attributes allocated{};
};

// The stored skill ranks of a player changed (a SKILL_LEARN_REQ committed; PROTOCOL_SKILLS.md §3.3):
// the world uses them from its next tick on.
struct SkillsCommand
{
    SessionKey session = 0;
    game::SkillRanks ranks{};
};

// The arrival of a zone change was stored (or not): posted by the save's completion (any thread).
// PROTOCOL_ZONES.md §3.5.
struct ZoneArrivedCommand
{
    SessionKey session = 0;
    std::uint64_t token = 0; // identifies the change (a later change of the same session has another)
    bool stored = false;
};

// The client loaded the zone it changed to (ZONE_LOADED_REQ, PROTOCOL_ZONES.md §3.4).
struct ZoneReadyCommand
{
    SessionKey session = 0;
};

// A shop request of a player (PROTOCOL_SHOP.md §3), already decoded and flood-checked.
struct ShopCommand
{
    enum class Kind : std::uint8_t
    {
        Open,  // SHOP_OPEN_REQ(npc)
        Close, // SHOP_CLOSE_REQ(npc)
        Buy,   // SHOP_BUY_REQ(seq, npc, template, quantity)
        Sell,  // SHOP_SELL_REQ(seq, npc, bag_slot, quantity)
        Upgrade, // UPGRADE_REQ(seq, npc, bag_slot, item_id, from_level, stone_slot) (v10, PROTOCOL_UPGRADE.md)
    };
    SessionKey session = 0;
    Kind kind = Kind::Open;
    EntityId npc = 0;
    std::uint32_t seq = 0;
    std::uint16_t templateId = 0;
    std::uint16_t quantity = 0;
    std::uint8_t bagSlot = 0;
    std::uint64_t itemId = 0;   // upgrade
    std::uint8_t fromLevel = 0; // upgrade
    std::uint8_t stoneSlot = 0; // upgrade
};

// A consumable of a player (PROTOCOL_SHOP.md §2.3). Use: the session found the stack and asks the
// world to check and start the cooldown (answered with an ItemUseGrant or ITEM_USE_RES in the batch).
// Apply: the unit is consumed (committed); the world restores now.
struct ItemUseCommand
{
    enum class Kind : std::uint8_t
    {
        Use,
        Apply,
    };
    SessionKey session = 0;
    Kind kind = Kind::Use;
    std::uint32_t seq = 0;
    std::uint16_t templateId = 0;
    std::uint8_t bagSlot = 0;
    std::uint32_t restoreHp = 0;
    std::uint32_t restoreMp = 0;
};

using WorldCommand = std::variant<EnterCommand, MoveCommand, LeaveCommand, EquipCommand, CombatCommand, LootCommand,
                                  LootSettleCommand, StatsCommand, SkillsCommand, ZoneArrivedCommand, ZoneReadyCommand,
                                  ShopCommand, ItemUseCommand>;

// Persists a character's position and progress (level, xp, hp, mp); same contract as
// IAccountService::saveCharacterState.
using SavePositionFn = std::function<void(auth::AccountId, auth::CharacterId, auth::CharacterPosition,
                                          auth::CharacterProgress, std::function<void(bool ok)> done)>;

} // namespace vo::server::world
