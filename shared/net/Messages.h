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
