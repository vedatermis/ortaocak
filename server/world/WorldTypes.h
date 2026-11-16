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
    // Notes for the session itself (not packets): the player died / respawned, or its level changed
    // (item moves are refused while dead; the level decides item requirements).
    std::optional<bool> dead;
    std::optional<std::uint16_t> level;
    std::vector<std::uint8_t> bytes;
    std::vector<Item> items;

    [[nodiscard]] bool empty() const noexcept { return !entered && !dead && !level && items.empty(); }
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
    game::EquipBonus equip;        // attack/defense of everything equipped (PROTOCOL_COMBAT.md §1.1)
};

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

using WorldCommand = std::variant<EnterCommand, MoveCommand, LeaveCommand, EquipCommand, CombatCommand>;

// Persists a character's position and progress (level, xp, hp, mp); same contract as
// IAccountService::saveCharacterState.
using SavePositionFn = std::function<void(auth::AccountId, auth::CharacterId, auth::CharacterPosition,
                                          auth::CharacterProgress, std::function<void(bool ok)> done)>;

} // namespace vo::server::world
