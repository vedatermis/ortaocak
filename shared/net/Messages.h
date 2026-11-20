// One plain struct per packet of docs/PROTOCOL.md §5, docs/PROTOCOL_ITEMS.md §3, docs/PROTOCOL_STATS.md §3,
// docs/PROTOCOL_COMBAT.md §3 and docs/PROTOCOL_LOOT.md §4 (plus CharSummary, ItemEntry, LootEntry).
//
// Each message has
//   static constexpr Opcode kOpcode;
//   void write(PacketWriter&) const;          // appends the payload (not the opcode)
//   static bool read(PacketReader&, T& out);  // false on malformed input; does NOT check trailing bytes
//
// Use decodeMessage() for a complete payload: it also rejects trailing bytes (§1), and
// encodeMessage() to get a payload buffer.
//
// Field values that the server must answer with a result code instead of a protocol error (race,
// class, face, hair, ids, names) are carried raw; read() only rejects what the wire format itself
// forbids (bad lengths, bad bool, bad UTF-8, list count > 3, unknown result codes).
#pragma once

#include "shared/net/Opcodes.h"
#include "shared/net/PacketReader.h"
#include "shared/net/PacketWriter.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vo::net {

using KxPublicKeyBytes = std::array<std::uint8_t, 32>;
using SignatureBytes = std::array<std::uint8_t, 64>;

// ---- Handshake (§3) ----

struct Hello
{
    static constexpr Opcode kOpcode = Opcode::Hello;
    std::uint32_t magic = kMagic;
    std::uint16_t protocolVersion = kProtocolVersion;
    std::uint32_t clientBuild = 0;
    KxPublicKeyBytes clientKxPk{};

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, Hello& out);
    bool operator==(const Hello&) const = default;
};

struct HelloAck
{
    static constexpr Opcode kOpcode = Opcode::HelloAck;
    HelloResult result = HelloResult::Ok;
    std::uint16_t serverProtocolVersion = kProtocolVersion;
    KxPublicKeyBytes serverKxPk{};
    SignatureBytes signature{};

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, HelloAck& out);
    bool operator==(const HelloAck&) const = default;
};

// ---- Common (§5.1) ----

struct Ping
{
    static constexpr Opcode kOpcode = Opcode::Ping;
    std::uint32_t clientTimeMs = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, Ping& out);
    bool operator==(const Ping&) const = default;
};

struct Pong
{
    static constexpr Opcode kOpcode = Opcode::Pong;
    std::uint32_t clientTimeMs = 0;
    std::uint32_t serverTimeMs = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, Pong& out);
    bool operator==(const Pong&) const = default;
};

struct Error
{
    static constexpr Opcode kOpcode = Opcode::Error;
    ErrorCode errorCode = ErrorCode::InternalError; // any u16 is accepted on read
    std::string debugMessage;                       // str8: at most 255 bytes

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, Error& out);
    bool operator==(const Error&) const = default;
};

struct Logout
{
    static constexpr Opcode kOpcode = Opcode::Logout;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, Logout& out);
    bool operator==(const Logout&) const = default;
};

// ---- Login (§5.2) ----

struct LoginReq
{
    static constexpr Opcode kOpcode = Opcode::LoginReq;
    std::string username;
    std::string password;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LoginReq& out);
    bool operator==(const LoginReq&) const = default;
};

struct LoginRes
{
    static constexpr Opcode kOpcode = Opcode::LoginRes;
    LoginResult result = LoginResult::Ok;
    std::uint32_t accountId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LoginRes& out);
    bool operator==(const LoginRes&) const = default;
};

// ---- Characters (§5.3) ----

struct CharSummary
{
    std::uint32_t charId = 0;
    std::string name;
    std::uint8_t race = 0;
    std::uint8_t charClass = 0; // "class" on the wire
    std::uint8_t face = 0;
    std::uint8_t hair = 0;
    std::uint16_t level = 0;
    std::uint16_t zoneId = 0;
    std::uint16_t mainHandTemplate = 0; // v2 (PROTOCOL_ITEMS.md §2): visible equipment, 0 = none
    std::uint16_t offHandTemplate = 0;
    // M17 (PROTOCOL_ITEMS.md §2.1): worn armour template ids Head, Chest, Legs, Hands, Feet; 0 = none.
    std::array<std::uint16_t, 5> armorTemplates{};

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CharSummary& out);
    bool operator==(const CharSummary&) const = default;
};

struct CharListReq
{
    static constexpr Opcode kOpcode = Opcode::CharListReq;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CharListReq& out);
    bool operator==(const CharListReq&) const = default;
};

struct CharListRes
{
    static constexpr Opcode kOpcode = Opcode::CharListRes;
    std::vector<CharSummary> characters; // 0..3 (kMaxCharactersPerAccount)

    void write(PacketWriter& w) const;   // fails the writer if more than 3 entries
    static bool read(PacketReader& r, CharListRes& out);
    bool operator==(const CharListRes&) const = default;
};

struct CharCreateReq
{
    static constexpr Opcode kOpcode = Opcode::CharCreateReq;
    std::string name;
    std::uint8_t race = 0;
    std::uint8_t charClass = 0;
    std::uint8_t face = 0;
    std::uint8_t hair = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CharCreateReq& out);
    bool operator==(const CharCreateReq&) const = default;
};

struct CharCreateRes
{
    static constexpr Opcode kOpcode = Opcode::CharCreateRes;
    CharCreateResult result = CharCreateResult::Ok;
    CharSummary character; // on the wire only if result == Ok

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CharCreateRes& out);
    bool operator==(const CharCreateRes&) const = default;
};

struct CharDeleteReq
{
    static constexpr Opcode kOpcode = Opcode::CharDeleteReq;
    std::uint32_t charId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CharDeleteReq& out);
    bool operator==(const CharDeleteReq&) const = default;
};

struct CharDeleteRes
{
    static constexpr Opcode kOpcode = Opcode::CharDeleteRes;
    CharDeleteResult result = CharDeleteResult::Ok;
    std::uint32_t charId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CharDeleteRes& out);
    bool operator==(const CharDeleteRes&) const = default;
};

struct EnterWorldReq
{
    static constexpr Opcode kOpcode = Opcode::EnterWorldReq;
    std::uint32_t charId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EnterWorldReq& out);
    bool operator==(const EnterWorldReq&) const = default;
};

struct EnterWorldRes
{
    static constexpr Opcode kOpcode = Opcode::EnterWorldRes;
    EnterWorldResult result = EnterWorldResult::Ok;
    // The remaining fields are on the wire only if result == Ok.
    std::uint32_t entityId = 0;
    std::uint16_t zoneId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float walkSpeed = 0.0f; // m/s
    float runSpeed = 0.0f;  // m/s
    std::uint32_t serverTimeMs = 0;
    // v3 (PROTOCOL_COMBAT.md §2): the character's combat state.
    std::uint16_t level = 0;
    std::uint32_t xp = 0;
    std::uint32_t xpToNext = 0;
    std::uint32_t hp = 0;
    std::uint32_t maxHp = 0;
    std::uint32_t mp = 0;
    std::uint32_t maxMp = 0;
    std::uint16_t attack = 0;
    std::uint16_t defense = 0;
    bool isDead = false;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EnterWorldRes& out);
    bool operator==(const EnterWorldRes&) const = default;
};

struct LeaveWorldReq
{
    static constexpr Opcode kOpcode = Opcode::LeaveWorldReq;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LeaveWorldReq& out);
    bool operator==(const LeaveWorldReq&) const = default;
};

struct LeaveWorldRes
{
    static constexpr Opcode kOpcode = Opcode::LeaveWorldRes;
    std::uint8_t result = kLeaveWorldOk; // only 0 (ok) is defined by the spec

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LeaveWorldRes& out);
    bool operator==(const LeaveWorldRes&) const = default;
};

// ---- World (§5.4) ----

struct MoveReq
{
    static constexpr Opcode kOpcode = Opcode::MoveReq;
    std::uint32_t seq = 0;
    std::uint8_t flags = 0; // kMoveFlagMoving | kMoveFlagRunning
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, MoveReq& out);
    bool operator==(const MoveReq&) const = default;
};

struct EntitySpawn
{
    static constexpr Opcode kOpcode = Opcode::EntitySpawn;
    std::uint32_t entityId = 0;
    std::uint8_t entityType = kEntityTypePlayer;
    std::string name;
    std::uint8_t race = 0;
    std::uint8_t charClass = 0;
    std::uint8_t face = 0;
    std::uint8_t hair = 0;
    std::uint16_t level = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    std::uint8_t flags = 0;
    std::uint16_t mainHandTemplate = 0; // v2 (PROTOCOL_ITEMS.md §2)
    std::uint16_t offHandTemplate = 0;
    // v3 (PROTOCOL_COMBAT.md §2)
    std::uint32_t hp = 0;
    std::uint32_t maxHp = 0;
    std::uint16_t templateId = 0; // monster template for entity_type 2, 0 for players
    std::uint8_t state = 0;       // kEntityState* bits; unknown bits are rejected
    // M17 (PROTOCOL_ITEMS.md §2.1, after `state` on the wire): worn armour Head .. Feet; 0 = none.
    std::array<std::uint16_t, 5> armorTemplates{};

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EntitySpawn& out);
    bool operator==(const EntitySpawn&) const = default;
};

struct EntityDespawn
{
    static constexpr Opcode kOpcode = Opcode::EntityDespawn;
    std::uint32_t entityId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EntityDespawn& out);
    bool operator==(const EntityDespawn&) const = default;
};

struct EntityMove
{
    static constexpr Opcode kOpcode = Opcode::EntityMove;
    std::uint32_t entityId = 0;
    std::uint8_t flags = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    std::uint32_t serverTimeMs = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EntityMove& out);
    bool operator==(const EntityMove&) const = default;
};

struct PositionCorrect
{
    static constexpr Opcode kOpcode = Opcode::PositionCorrect;
    std::uint32_t seq = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, PositionCorrect& out);
    bool operator==(const PositionCorrect&) const = default;
};

// ---- Items (docs/PROTOCOL_ITEMS.md §3) ----
// Container / slot values are carried raw (the receiver range-checks them); read() rejects only
// list counts outside the wire limits and unknown result codes.

struct ItemEntry
{
    std::uint8_t container = 0;
    std::uint8_t slot = 0;
    std::uint64_t itemId = 0; // 0 = the slot is (now) empty; then templateId and quantity are 0
    std::uint16_t templateId = 0;
    std::uint16_t quantity = 0;
    std::uint8_t upgrade = 0; // v10 (docs/PROTOCOL_UPGRADE.md §4): 0..10; read() rejects more

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ItemEntry& out);
    bool operator==(const ItemEntry&) const = default;
};

struct Inventory
{
    static constexpr Opcode kOpcode = Opcode::Inventory;
    std::uint8_t bagSize = static_cast<std::uint8_t>(kItemBagSlots);
    std::vector<ItemEntry> items; // 0..kMaxInventoryEntries: every occupied slot, equipment first, by slot

    void write(PacketWriter& w) const; // fails the writer if more than kMaxInventoryEntries
    static bool read(PacketReader& r, Inventory& out);
    bool operator==(const Inventory&) const = default;
};

struct ItemMoveReq
{
    static constexpr Opcode kOpcode = Opcode::ItemMoveReq;
    std::uint32_t seq = 0; // client-chosen, echoed in ITEM_MOVE_RES
    std::uint8_t fromContainer = 0;
    std::uint8_t fromSlot = 0;
    std::uint8_t toContainer = 0;
    std::uint8_t toSlot = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ItemMoveReq& out);
    bool operator==(const ItemMoveReq&) const = default;
};

struct ItemMoveRes
{
    static constexpr Opcode kOpcode = Opcode::ItemMoveRes;
    std::uint32_t seq = 0;
    ItemMoveResult result = ItemMoveResult::Ok;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ItemMoveRes& out);
    bool operator==(const ItemMoveRes&) const = default;
};

struct ItemUpdate
{
    static constexpr Opcode kOpcode = Opcode::ItemUpdate;
    std::vector<ItemEntry> items; // 1..kMaxItemUpdateEntries: new content of each changed slot

    void write(PacketWriter& w) const; // fails the writer if empty or more than 8 entries
    static bool read(PacketReader& r, ItemUpdate& out);
    bool operator==(const ItemUpdate&) const = default;
};

struct EntityEquip
{
    static constexpr Opcode kOpcode = Opcode::EntityEquip;
    std::uint32_t entityId = 0;
    std::uint16_t mainHandTemplate = 0;
    std::uint16_t offHandTemplate = 0;
    // M17 (PROTOCOL_ITEMS.md §2.1): worn armour template ids Head, Chest, Legs, Hands, Feet; 0 = none.
    std::array<std::uint16_t, 5> armorTemplates{};

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EntityEquip& out);
    bool operator==(const EntityEquip&) const = default;
};

// ---- Combat (docs/PROTOCOL_COMBAT.md §3) ----
// Ids are carried raw (the server checks them against what the requester has spawned); read()
// rejects action/reason/kind/result/stance values and flag/state bits the contract does not define.

struct TargetReq
{
    static constexpr Opcode kOpcode = Opcode::TargetReq;
    std::uint32_t entityId = 0; // 0 clears

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, TargetReq& out);
    bool operator==(const TargetReq&) const = default;
};

struct AttackReq
{
    static constexpr Opcode kOpcode = Opcode::AttackReq;
    std::uint32_t entityId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, AttackReq& out);
    bool operator==(const AttackReq&) const = default;
};

struct AttackStopReq
{
    static constexpr Opcode kOpcode = Opcode::AttackStopReq;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, AttackStopReq& out);
    bool operator==(const AttackStopReq&) const = default;
};

struct SkillReq
{
    static constexpr Opcode kOpcode = Opcode::SkillReq;
    std::uint16_t skillId = 0;
    std::uint32_t targetId = 0; // 0 = self where the skill allows it

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SkillReq& out);
    bool operator==(const SkillReq&) const = default;
};

struct ActionFail
{
    static constexpr Opcode kOpcode = Opcode::ActionFail;
    CombatAction action = CombatAction::Attack;
    std::uint16_t skillId = 0; // 0 unless action == Skill
    ActionFailReason reason = ActionFailReason::NoTarget;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ActionFail& out);
    bool operator==(const ActionFail&) const = default;
};

struct CombatEvent
{
    static constexpr Opcode kOpcode = Opcode::CombatEvent;
    std::uint32_t sourceId = 0;
    std::uint32_t targetId = 0;
    std::uint16_t skillId = 0; // 0 = basic attack
    std::uint8_t kind = kCombatKindDamage;
    std::uint8_t flags = 0; // kCombatFlag*
    std::uint32_t amount = 0;
    std::uint32_t targetHp = 0; // after applying
    std::uint32_t serverTimeMs = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CombatEvent& out);
    bool operator==(const CombatEvent&) const = default;
};

struct CastStart
{
    static constexpr Opcode kOpcode = Opcode::CastStart;
    std::uint32_t casterId = 0;
    std::uint16_t skillId = 0;
    std::uint32_t targetId = 0;
    std::uint16_t castMs = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CastStart& out);
    bool operator==(const CastStart&) const = default;
};

struct CastEnd
{
    static constexpr Opcode kOpcode = Opcode::CastEnd;
    std::uint32_t casterId = 0;
    std::uint16_t skillId = 0;
    std::uint8_t result = kCastCompleted; // kCastCompleted / kCastInterrupted

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CastEnd& out);
    bool operator==(const CastEnd&) const = default;
};

struct EntityStats
{
    static constexpr Opcode kOpcode = Opcode::EntityStats;
    std::uint32_t entityId = 0;
    std::uint32_t hp = 0;
    std::uint32_t maxHp = 0;
    std::uint16_t level = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EntityStats& out);
    bool operator==(const EntityStats&) const = default;
};

struct SelfStats
{
    static constexpr Opcode kOpcode = Opcode::SelfStats;
    std::uint32_t hp = 0;
    std::uint32_t maxHp = 0;
    std::uint32_t mp = 0;
    std::uint32_t maxMp = 0;
    std::uint16_t attack = 0;
    std::uint16_t defense = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SelfStats& out);
    bool operator==(const SelfStats&) const = default;
};

struct XpGain
{
    static constexpr Opcode kOpcode = Opcode::XpGain;
    std::uint32_t amount = 0;
    std::uint32_t xp = 0; // after the gain (and any level-up)
    std::uint32_t xpToNext = 0;
    std::uint32_t sourceEntityId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, XpGain& out);
    bool operator==(const XpGain&) const = default;
};

struct LevelUp
{
    static constexpr Opcode kOpcode = Opcode::LevelUp;
    std::uint32_t entityId = 0;
    std::uint16_t level = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LevelUp& out);
    bool operator==(const LevelUp&) const = default;
};

struct EntityDeath
{
    static constexpr Opcode kOpcode = Opcode::EntityDeath;
    std::uint32_t entityId = 0;
    std::uint32_t killerId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EntityDeath& out);
    bool operator==(const EntityDeath&) const = default;
};

struct RespawnReq
{
    static constexpr Opcode kOpcode = Opcode::RespawnReq;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, RespawnReq& out);
    bool operator==(const RespawnReq&) const = default;
};

struct RespawnRes
{
    static constexpr Opcode kOpcode = Opcode::RespawnRes;
    RespawnResult result = RespawnResult::Ok;
    // Always on the wire; meaningful for Ok only (otherwise the current position and values).
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    std::uint32_t hp = 0;
    std::uint32_t mp = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, RespawnRes& out);
    bool operator==(const RespawnRes&) const = default;
};

struct EntityState
{
    static constexpr Opcode kOpcode = Opcode::EntityState;
    std::uint32_t entityId = 0;
    std::uint8_t state = 0; // kEntityState* bits

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EntityState& out);
    bool operator==(const EntityState&) const = default;
};

struct StanceReq
{
    static constexpr Opcode kOpcode = Opcode::StanceReq;
    std::uint8_t stance = kStanceStand; // kStanceStand / kStanceSit

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, StanceReq& out);
    bool operator==(const StanceReq&) const = default;
};

// ---- Loot (docs/PROTOCOL_LOOT.md §4) ----
// Ids and `what` are carried raw (the server checks them); read() rejects unknown result / reason
// values and LOOT_CONTENTS lists the contract forbids (count > 8, index >= 8, indices not strictly
// ascending, template id 0, quantity 0).

struct LootOpenReq
{
    static constexpr Opcode kOpcode = Opcode::LootOpenReq;
    std::uint32_t lootId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LootOpenReq& out);
    bool operator==(const LootOpenReq&) const = default;
};

struct LootOpenRes
{
    static constexpr Opcode kOpcode = Opcode::LootOpenRes;
    std::uint32_t lootId = 0;
    LootResult result = LootResult::Ok;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LootOpenRes& out);
    bool operator==(const LootOpenRes&) const = default;
};

struct LootEntry
{
    std::uint8_t index = 0; // 0..7, fixed for the container's life
    std::uint16_t templateId = 0;
    std::uint16_t quantity = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LootEntry& out);
    bool operator==(const LootEntry&) const = default;
};

struct LootContents
{
    static constexpr Opcode kOpcode = Opcode::LootContents;
    std::uint32_t lootId = 0;
    std::uint32_t coins = 0;
    std::vector<LootEntry> entries; // 0..kMaxLootEntries, strictly ascending index

    void write(PacketWriter& w) const; // fails the writer on an invalid list
    static bool read(PacketReader& r, LootContents& out);
    bool operator==(const LootContents&) const = default;
};

struct LootTakeReq
{
    static constexpr Opcode kOpcode = Opcode::LootTakeReq;
    std::uint32_t seq = 0; // client-chosen, echoed in LOOT_TAKE_RES
    std::uint32_t lootId = 0;
    std::uint8_t what = kLootTakeAll; // entry index 0..7, kLootTakeCoins, kLootTakeAll (others: AlreadyTaken)

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LootTakeReq& out);
    bool operator==(const LootTakeReq&) const = default;
};

struct LootTakeRes
{
    static constexpr Opcode kOpcode = Opcode::LootTakeRes;
    std::uint32_t seq = 0;
    std::uint32_t lootId = 0;
    LootResult result = LootResult::Ok;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LootTakeRes& out);
    bool operator==(const LootTakeRes&) const = default;
};

struct LootCloseReq
{
    static constexpr Opcode kOpcode = Opcode::LootCloseReq;
    std::uint32_t lootId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LootCloseReq& out);
    bool operator==(const LootCloseReq&) const = default;
};

struct LootClosed
{
    static constexpr Opcode kOpcode = Opcode::LootClosed;
    std::uint32_t lootId = 0;
    LootClosedReason reason = LootClosedReason::Gone;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, LootClosed& out);
    bool operator==(const LootClosed&) const = default;
};

struct Coins
{
    static constexpr Opcode kOpcode = Opcode::Coins;
    std::uint32_t coins = 0;  // balance after the change
    std::uint32_t gained = 0; // added by this change (0 on world entry)

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, Coins& out);
    bool operator==(const Coins&) const = default;
};

// ---- Stats (docs/PROTOCOL_STATS.md §3) ----
// Attribute arrays are in the index order strength, dexterity, vitality, intelligence, spirit.
// read() rejects a base above 255, a permille above 1000 and unknown results; STAT_ALLOCATE_REQ
// amounts are carried raw (the server answers with a result).

struct StatAttribute
{
    std::uint16_t base = 0;  // class start + allocated, 0..kStatAttributeCap
    std::uint16_t bonus = 0; // from equipped items
    bool operator==(const StatAttribute&) const = default;
};

struct CharStats
{
    static constexpr Opcode kOpcode = Opcode::CharStats;
    std::uint16_t level = 0;
    std::array<StatAttribute, kStatAttributeCount> attributes{};
    std::uint16_t unspent = 0;
    std::uint32_t maxHp = 0;
    std::uint32_t maxMp = 0;
    std::uint16_t attack = 0;
    std::uint16_t defense = 0;
    std::uint16_t critPermille = 0;  // 0..1000
    std::uint16_t dodgePermille = 0; // 0..1000
    std::uint16_t hpRegenTenths = 0; // hp per second x 10, out of combat, standing
    std::uint16_t mpRegenTenths = 0;
    std::uint16_t attackIntervalMs = 0;
    std::uint16_t healPermille = 0; // Soluk Güneş: 1/1000 of the target's max hp, 0..1000
    // v12 (docs/PROTOCOL_EQUIPMENT.md §4.1): the attack power range (attack_min <= attack <= attack_max),
    // the basic attack reach in centimetres, the basic hit factor x 1000 (weapon interval / class
    // interval) and the equipment's bonus lines (already counted in max_hp, max_mp, crit and dodge).
    std::uint16_t attackMin = 0;
    std::uint16_t attackMax = 0;
    std::uint16_t reachCm = 0;
    std::uint16_t swingPermille = 1000;
    std::uint16_t bonusHp = 0;
    std::uint16_t bonusMp = 0;
    std::uint16_t bonusCritPermille = 0;  // 0..1000
    std::uint16_t bonusDodgePermille = 0; // 0..1000

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, CharStats& out);
    bool operator==(const CharStats&) const = default;
};

struct StatAllocateReq
{
    static constexpr Opcode kOpcode = Opcode::StatAllocateReq;
    std::uint32_t seq = 0; // client-chosen, echoed in STAT_ALLOCATE_RES
    std::array<std::uint16_t, kStatAttributeCount> points{};

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, StatAllocateReq& out);
    bool operator==(const StatAllocateReq&) const = default;
};

struct StatAllocateRes
{
    static constexpr Opcode kOpcode = Opcode::StatAllocateRes;
    std::uint32_t seq = 0;
    StatAllocateResult result = StatAllocateResult::Ok;
    std::uint16_t unspent = 0; // after the request (unchanged on failure)

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, StatAllocateRes& out);
    bool operator==(const StatAllocateRes&) const = default;
};

// ---- Skills (docs/PROTOCOL_SKILLS.md §3) ----
// read() rejects counts above the limits, SKILL_STATE ids not strictly ascending or ranks outside
// 1..3, unknown results / kinds / reasons, stacks 0, remaining > duration, cooldowns with remaining 0
// or above the total, non-finite floats. Skill ids in requests and bar slots are carried raw.

using SkillBarSlots = std::array<std::uint16_t, kSkillBarSlotCount>;

struct SkillRankEntry
{
    std::uint16_t skillId = 0;
    std::uint8_t rank = 1; // 1..3
    bool operator==(const SkillRankEntry&) const = default;
};

struct SkillState
{
    static constexpr Opcode kOpcode = Opcode::SkillState;
    std::uint16_t points = 0;  // points(level)
    std::uint16_t unspent = 0;
    std::vector<SkillRankEntry> skills; // 0..kMaxSkillStateEntries, strictly ascending id

    void write(PacketWriter& w) const; // fails the writer on an invalid list
    static bool read(PacketReader& r, SkillState& out);
    bool operator==(const SkillState&) const = default;
};

struct SkillLearnReq
{
    static constexpr Opcode kOpcode = Opcode::SkillLearnReq;
    std::uint32_t seq = 0;
    std::uint16_t skillId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SkillLearnReq& out);
    bool operator==(const SkillLearnReq&) const = default;
};

struct SkillLearnRes
{
    static constexpr Opcode kOpcode = Opcode::SkillLearnRes;
    std::uint32_t seq = 0;
    std::uint16_t skillId = 0;
    SkillLearnResult result = SkillLearnResult::Ok;
    std::uint8_t rank = 0;     // after the request, 0..3
    std::uint16_t unspent = 0; // after the request

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SkillLearnRes& out);
    bool operator==(const SkillLearnRes&) const = default;
};

struct SkillBar
{
    static constexpr Opcode kOpcode = Opcode::SkillBar;
    SkillBarSlots slots{}; // 0 = empty

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SkillBar& out);
    bool operator==(const SkillBar&) const = default;
};

struct SkillBarSetReq
{
    static constexpr Opcode kOpcode = Opcode::SkillBarSetReq;
    std::uint32_t seq = 0;
    SkillBarSlots slots{};

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SkillBarSetReq& out);
    bool operator==(const SkillBarSetReq&) const = default;
};

struct SkillBarSetRes
{
    static constexpr Opcode kOpcode = Opcode::SkillBarSetRes;
    std::uint32_t seq = 0;
    SkillBarResult result = SkillBarResult::Ok;
    SkillBarSlots slots{}; // the stored bar after the request

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SkillBarSetRes& out);
    bool operator==(const SkillBarSetRes&) const = default;
};

struct SkillCooldownEntry
{
    std::uint16_t skillId = 0;
    std::uint32_t remainingMs = 0; // 1..totalMs
    std::uint32_t totalMs = 0;
    bool operator==(const SkillCooldownEntry&) const = default;
};

struct SkillCooldowns
{
    static constexpr Opcode kOpcode = Opcode::SkillCooldowns;
    std::vector<SkillCooldownEntry> entries; // 1..kMaxSkillCooldownEntries

    void write(PacketWriter& w) const; // fails the writer on an invalid list
    static bool read(PacketReader& r, SkillCooldowns& out);
    bool operator==(const SkillCooldowns&) const = default;
};

struct EffectEntry
{
    std::uint16_t skillId = 0;
    std::uint8_t kind = kEffectKindMin; // 1..13
    std::uint8_t stacks = 1;
    std::uint32_t sourceId = 0;
    std::uint32_t durationMs = 0;
    std::uint32_t remainingMs = 0; // 0..durationMs
    std::uint32_t amount = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EffectEntry& out);
    bool operator==(const EffectEntry&) const = default;
};

struct EffectList
{
    static constexpr Opcode kOpcode = Opcode::EffectList;
    std::uint32_t entityId = 0;
    std::vector<EffectEntry> effects; // 0..kMaxEffectEntries

    void write(PacketWriter& w) const; // fails the writer on an invalid list
    static bool read(PacketReader& r, EffectList& out);
    bool operator==(const EffectList&) const = default;
};

struct EffectApply
{
    static constexpr Opcode kOpcode = Opcode::EffectApply;
    std::uint32_t entityId = 0;
    EffectEntry effect;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EffectApply& out);
    bool operator==(const EffectApply&) const = default;
};

struct EffectRemove
{
    static constexpr Opcode kOpcode = Opcode::EffectRemove;
    std::uint32_t entityId = 0;
    std::uint16_t skillId = 0;
    std::uint8_t kind = kEffectKindMin;
    EffectRemoveReason reason = EffectRemoveReason::Expired;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, EffectRemove& out);
    bool operator==(const EffectRemove&) const = default;
};

struct SkillLeap
{
    static constexpr Opcode kOpcode = Opcode::SkillLeap;
    std::uint32_t entityId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    std::uint32_t targetId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SkillLeap& out);
    bool operator==(const SkillLeap&) const = default;
};

struct SkillUsed
{
    static constexpr Opcode kOpcode = Opcode::SkillUsed;
    std::uint32_t casterId = 0;
    std::uint16_t skillId = 0;
    std::uint32_t targetId = 0; // primary target; the caster for self / around-self skills
    std::uint8_t rank = 1;      // 1..3

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, SkillUsed& out);
    bool operator==(const SkillUsed&) const = default;
};

// ---- Zones (docs/PROTOCOL_ZONES.md §3) ----

struct ZoneChangeBegin
{
    static constexpr Opcode kOpcode = Opcode::ZoneChangeBegin;
    std::uint16_t fromZone = 0;
    std::uint16_t toZone = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ZoneChangeBegin& out);
    bool operator==(const ZoneChangeBegin&) const = default;
};

// `enter` carries the ENTER_WORLD_RES body; its `result` is not on the wire here (always Ok).
struct ZoneChange
{
    static constexpr Opcode kOpcode = Opcode::ZoneChange;
    ZoneChangeResult result = ZoneChangeResult::Ok;
    std::uint16_t fromZone = 0;
    EnterWorldRes enter;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ZoneChange& out);
    bool operator==(const ZoneChange&) const = default;
};

struct ZoneLoadedReq
{
    static constexpr Opcode kOpcode = Opcode::ZoneLoadedReq;
    std::uint16_t zoneId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ZoneLoadedReq& out);
    bool operator==(const ZoneLoadedReq&) const = default;
};

struct ZoneGateRefused
{
    static constexpr Opcode kOpcode = Opcode::ZoneGateRefused;
    std::uint16_t toZone = 0;
    GateRefusal reason = GateRefusal::RaceForbidden;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ZoneGateRefused& out);
    bool operator==(const ZoneGateRefused&) const = default;
};

// ---- Merchants and consumables (docs/PROTOCOL_SHOP.md §3) ----
// Ids, template ids, slots and quantities in requests are carried raw (the server answers with a
// result); read() rejects unknown results / reasons, SHOP_OPEN_RES lists the contract forbids (count
// above 24, entries on a failure, template id 0, price 0) and an ITEM_COOLDOWN with remaining 0 or
// above the total.

struct ShopOpenReq
{
    static constexpr Opcode kOpcode = Opcode::ShopOpenReq;
    std::uint32_t npcId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ShopOpenReq& out);
    bool operator==(const ShopOpenReq&) const = default;
};

struct ShopEntry
{
    std::uint16_t templateId = 0;
    std::uint32_t price = 0; // Akçe per unit, >= 1
    bool operator==(const ShopEntry&) const = default;
};

struct ShopOpenRes
{
    static constexpr Opcode kOpcode = Opcode::ShopOpenRes;
    std::uint32_t npcId = 0;
    ShopResult result = ShopResult::Ok;
    std::vector<ShopEntry> entries; // the stock in the data file's order; empty unless Ok

    void write(PacketWriter& w) const; // fails the writer on an invalid list
    static bool read(PacketReader& r, ShopOpenRes& out);
    bool operator==(const ShopOpenRes&) const = default;
};

struct ShopCloseReq
{
    static constexpr Opcode kOpcode = Opcode::ShopCloseReq;
    std::uint32_t npcId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ShopCloseReq& out);
    bool operator==(const ShopCloseReq&) const = default;
};

struct ShopClosed
{
    static constexpr Opcode kOpcode = Opcode::ShopClosed;
    std::uint32_t npcId = 0;
    ShopClosedReason reason = ShopClosedReason::OutOfRange;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ShopClosed& out);
    bool operator==(const ShopClosed&) const = default;
};

struct ShopBuyReq
{
    static constexpr Opcode kOpcode = Opcode::ShopBuyReq;
    std::uint32_t seq = 0; // client-chosen, echoed in SHOP_BUY_RES
    std::uint32_t npcId = 0;
    std::uint16_t templateId = 0;
    std::uint16_t quantity = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ShopBuyReq& out);
    bool operator==(const ShopBuyReq&) const = default;
};

struct ShopBuyRes
{
    static constexpr Opcode kOpcode = Opcode::ShopBuyRes;
    std::uint32_t seq = 0;
    ShopResult result = ShopResult::Ok;
    std::uint16_t templateId = 0; // echoed
    std::uint16_t quantity = 0;   // echoed
    std::uint32_t cost = 0;       // Akçe paid (0 unless Ok)

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ShopBuyRes& out);
    bool operator==(const ShopBuyRes&) const = default;
};

struct ShopSellReq
{
    static constexpr Opcode kOpcode = Opcode::ShopSellReq;
    std::uint32_t seq = 0;
    std::uint32_t npcId = 0;
    std::uint8_t bagSlot = 0; // 0..27
    std::uint16_t quantity = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ShopSellReq& out);
    bool operator==(const ShopSellReq&) const = default;
};

struct ShopSellRes
{
    static constexpr Opcode kOpcode = Opcode::ShopSellRes;
    std::uint32_t seq = 0;
    ShopResult result = ShopResult::Ok;
    std::uint16_t templateId = 0; // what was sold (0 if the slot held nothing known)
    std::uint16_t quantity = 0;   // echoed
    std::uint32_t gained = 0;     // Akçe received (0 unless Ok)

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ShopSellRes& out);
    bool operator==(const ShopSellRes&) const = default;
};

struct ItemUseReq
{
    static constexpr Opcode kOpcode = Opcode::ItemUseReq;
    std::uint32_t seq = 0;
    std::uint16_t templateId = 0;
    std::uint8_t bagSlot = kItemUseAnyStack; // 0..27, or 0xFF = the lowest bag slot holding the template

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ItemUseReq& out);
    bool operator==(const ItemUseReq&) const = default;
};

struct ItemUseRes
{
    static constexpr Opcode kOpcode = Opcode::ItemUseRes;
    std::uint32_t seq = 0;
    ItemUseResult result = ItemUseResult::Ok;
    std::uint16_t templateId = 0; // echoed

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ItemUseRes& out);
    bool operator==(const ItemUseRes&) const = default;
};

struct ItemCooldown
{
    static constexpr Opcode kOpcode = Opcode::ItemCooldown;
    std::uint32_t remainingMs = 0; // 1..totalMs
    std::uint32_t totalMs = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ItemCooldown& out);
    bool operator==(const ItemCooldown&) const = default;
};

struct ItemUsed
{
    static constexpr Opcode kOpcode = Opcode::ItemUsed;
    std::uint32_t entityId = 0;
    std::uint16_t templateId = 0;
    std::uint32_t hpRestored = 0; // what was actually gained (clamped to the maximum)
    std::uint32_t mpRestored = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, ItemUsed& out);
    bool operator==(const ItemUsed&) const = default;
};

// ---- Item upgrades (docs/PROTOCOL_UPGRADE.md §4, v10) ----
// Requests are carried raw (the server answers with a result); read() rejects an unknown result and a
// level above 10.

struct UpgradeReq
{
    static constexpr Opcode kOpcode = Opcode::UpgradeReq;
    std::uint32_t seq = 0;   // client-chosen, echoed in UPGRADE_RES
    std::uint32_t npcId = 0; // the smith whose shop is open
    std::uint8_t bagSlot = 0; // 0..27: the item
    std::uint64_t itemId = 0; // the item the client means (must be in that slot)
    std::uint8_t fromLevel = 0; // its level as the client knows it (else Changed)
    std::uint8_t stoneSlot = kUpgradeAnyStone; // 0..27, or 0xFF = the lowest bag slot with the fitting stone

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, UpgradeReq& out);
    bool operator==(const UpgradeReq&) const = default;
};

struct UpgradeRes
{
    static constexpr Opcode kOpcode = Opcode::UpgradeRes;
    std::uint32_t seq = 0;
    UpgradeResult result = UpgradeResult::Success;
    std::uint8_t bagSlot = 0;          // echoed
    std::uint16_t templateId = 0;      // the item's template (0 if the slot held nothing known)
    std::uint8_t level = 0;            // Success: the new level; Failed / refusals: unchanged; Destroyed: the level it had
    std::uint16_t stoneTemplateId = 0; // the stone spent (0 unless an attempt)
    std::uint32_t fee = 0;             // Akçe paid (0 unless an attempt)

    void write(PacketWriter& w) const; // fails the writer on an unknown result or a level above 10
    static bool read(PacketReader& r, UpgradeRes& out);
    bool operator==(const UpgradeRes&) const = default;
};

// ---- Quests (docs/PROTOCOL_QUESTS.md §4, v11) ----
// The texts, objectives and rewards are in gamedata/quests.toml on both sides; the wire carries only
// the character's state. read() rejects unknown states / actions / results.

struct QuestEntry
{
    std::uint16_t questId = 0;                                   // 1..999
    QuestState state = QuestState::Active;                       // QUEST_LIST: Active / Done; QUEST_UPDATE also None
    std::array<std::uint16_t, kQuestObjectiveSlots> progress{}; // kill / boss counters (collect and talk: 0)
    std::uint32_t cooldownS = 0; // Done repeatable: seconds until it can be taken again (else 0)
    bool operator==(const QuestEntry&) const = default;
};

struct QuestList
{
    static constexpr Opcode kOpcode = Opcode::QuestList;
    std::vector<QuestEntry> quests; // 0..kMaxQuestListEntries, ascending id; never state None

    void write(PacketWriter& w) const; // fails the writer on an invalid list
    static bool read(PacketReader& r, QuestList& out);
    bool operator==(const QuestList&) const = default;
};

struct QuestUpdate
{
    static constexpr Opcode kOpcode = Opcode::QuestUpdate;
    QuestEntry quest; // the new state of one quest (None: abandoned)

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, QuestUpdate& out);
    bool operator==(const QuestUpdate&) const = default;
};

struct QuestAcceptReq
{
    static constexpr Opcode kOpcode = Opcode::QuestAcceptReq;
    std::uint32_t seq = 0;   // client-chosen, echoed in QUEST_RES
    std::uint32_t npcId = 0; // the giver (entity id of an NPC in view)
    std::uint16_t questId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, QuestAcceptReq& out);
    bool operator==(const QuestAcceptReq&) const = default;
};

struct QuestAbandonReq
{
    static constexpr Opcode kOpcode = Opcode::QuestAbandonReq;
    std::uint32_t seq = 0;
    std::uint16_t questId = 0;

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, QuestAbandonReq& out);
    bool operator==(const QuestAbandonReq&) const = default;
};

struct QuestHandInReq
{
    static constexpr Opcode kOpcode = Opcode::QuestHandInReq;
    std::uint32_t seq = 0;
    std::uint32_t npcId = 0; // the NPC it is handed in to (entity id)
    std::uint16_t questId = 0;
    std::uint8_t choice = kQuestNoChoice; // index into the reward's `choice` list, 0xFF = none

    void write(PacketWriter& w) const;
    static bool read(PacketReader& r, QuestHandInReq& out);
    bool operator==(const QuestHandInReq&) const = default;
};

struct QuestRes
{
    static constexpr Opcode kOpcode = Opcode::QuestRes;
    std::uint32_t seq = 0;
    QuestAction action = QuestAction::Accept;
    std::uint16_t questId = 0;
    QuestResult result = QuestResult::Ok;
    std::uint32_t xp = 0;    // hand-in Ok: the experience reward (XP_GAIN follows from the world)
    std::uint32_t coins = 0; // hand-in Ok: the Akçe reward (COINS came before)

    void write(PacketWriter& w) const; // fails the writer on an unknown action / result
    static bool read(PacketReader& r, QuestRes& out);
    bool operator==(const QuestRes&) const = default;
};

// ---- Helpers ----

// Parses a complete payload: read() must succeed and consume every byte.
template <class T>
[[nodiscard]] bool decodeMessage(std::span<const std::uint8_t> payload, T& out)
{
    PacketReader r(payload);
    return T::read(r, out) && r.finished();
}

// Serialises the payload (without opcode). nullopt if the writer failed (e.g. string too long).
template <class T>
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeMessage(const T& msg)
{
    PacketWriter w;
    msg.write(w);
    if (!w.ok())
        return std::nullopt;
    return w.take();
}

} // namespace vo::net
