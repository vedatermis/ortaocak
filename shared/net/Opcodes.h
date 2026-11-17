// Wire protocol constants (docs/PROTOCOL.md; v2 adds the items extension, docs/PROTOCOL_ITEMS.md;
// v3 combat, docs/PROTOCOL_COMBAT.md; v4 loot, docs/PROTOCOL_LOOT.md; v5 stats,
// docs/PROTOCOL_STATS.md; v6 skills, docs/PROTOCOL_SKILLS.md; v7 zones, docs/PROTOCOL_ZONES.md):
// opcodes, error codes, result enums and limits.
// This header is the single source of truth for numbers that appear on the wire.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vo::net {

// ---- Handshake (§3) ----
inline constexpr std::uint32_t kMagic = 0x4C4E4F56; // "VONL" on the wire (little-endian)
inline constexpr std::uint16_t kProtocolVersion = 7; // 2: items, 3: combat, 4: loot, 5: stats, 6: skills, 7: zones (docs/PROTOCOL_ZONES.md)
inline constexpr std::string_view kHandshakeSignatureLabel = "VO-HS1";
inline constexpr std::uint16_t kDefaultPort = 15001;

// ---- Framing (§2) ----
inline constexpr std::size_t kFrameHeaderBytes = 2;     // u16 length
inline constexpr std::size_t kOpcodeBytes = 2;          // u16 opcode
inline constexpr std::size_t kMinPlainFrameLength = 2;  // length field of a plaintext frame
inline constexpr std::size_t kMaxPlainFrameLength = 512;
inline constexpr std::size_t kAeadTagBytes = 16;
inline constexpr std::size_t kMinPlaintextPerFrame = 2; // opcode only
inline constexpr std::size_t kMaxPlaintextPerFrame = 8192;
inline constexpr std::size_t kMinCipherFrameLength = kMinPlaintextPerFrame + kAeadTagBytes; // 18
inline constexpr std::size_t kMaxCipherFrameLength = kMaxPlaintextPerFrame + kAeadTagBytes; // 8208
inline constexpr std::size_t kMaxPayloadPerFrame = kMaxPlaintextPerFrame - kOpcodeBytes;
inline constexpr std::size_t kMaxPlainPayload = kMaxPlainFrameLength - kOpcodeBytes;

// ---- Limits (§7) ----
inline constexpr std::chrono::milliseconds kHandshakeTimeout{5000};
inline constexpr std::chrono::milliseconds kLoginTimeout{30000};
inline constexpr std::chrono::milliseconds kIdleTimeout{30000};
inline constexpr std::chrono::milliseconds kPingInterval{5000};
inline constexpr std::uint32_t kMaxFailedLoginsPerIpPerMinute = 5;
inline constexpr std::uint32_t kMaxMoveReqPerSecond = 30;
inline constexpr std::size_t kMaxCharactersPerAccount = 3;
inline constexpr std::size_t kMaxOutboundQueueBytes = 256 * 1024;
// Items (docs/PROTOCOL_ITEMS.md §4).
inline constexpr std::uint32_t kMaxItemMoveReqPerSecond = 10;
inline constexpr std::size_t kItemEquipmentSlots = 8;
inline constexpr std::size_t kItemBagSlots = 28;
inline constexpr std::size_t kMaxInventoryEntries = kItemEquipmentSlots + kItemBagSlots; // INVENTORY count
inline constexpr std::size_t kMaxItemUpdateEntries = 8;                                   // ITEM_UPDATE count
// Combat (docs/PROTOCOL_COMBAT.md §4): ATTACK_REQ + SKILL_REQ + TARGET_REQ + STANCE_REQ combined.
inline constexpr std::uint32_t kMaxCombatReqPerSecond = 10;
// Loot (docs/PROTOCOL_LOOT.md §5): LOOT_OPEN_REQ + LOOT_TAKE_REQ + LOOT_CLOSE_REQ combined.
inline constexpr std::uint32_t kMaxLootReqPerSecond = 10;
inline constexpr std::size_t kMaxLootEntries = 8;         // LOOT_CONTENTS count, entry index 0..7
inline constexpr std::uint32_t kMaxCoins = 1'000'000'000; // Akçe balance cap
inline constexpr std::size_t kMaxLootTakesInFlight = 8;   // per player
// Stats (docs/PROTOCOL_STATS.md §4).
inline constexpr std::uint32_t kMaxStatAllocateReqPerSecond = 5;
inline constexpr std::size_t kMaxQueuedStatAllocations = 4; // waiting behind the one in flight
inline constexpr std::size_t kStatAttributeCount = 5;       // CHAR_STATS / STAT_ALLOCATE_REQ arrays
inline constexpr std::uint16_t kStatAttributeCap = 255;     // class start + allocated
inline constexpr std::uint16_t kMaxPermille = 1000;         // CHAR_STATS crit / dodge / heal
// Skills (docs/PROTOCOL_SKILLS.md §5).
inline constexpr std::uint32_t kMaxSkillReqPerSecond = 10; // SKILL_LEARN_REQ + SKILL_BAR_SET_REQ combined
inline constexpr std::size_t kMaxQueuedSkillRequests = 8;   // waiting behind the one in flight
inline constexpr std::size_t kSkillBarSlotCount = 8;        // SKILL_BAR slots (keys 1-8)
inline constexpr std::size_t kMaxSkillStateEntries = 32;    // SKILL_STATE count
inline constexpr std::size_t kMaxSkillCooldownEntries = 32; // SKILL_COOLDOWNS count
inline constexpr std::size_t kMaxEffectEntries = 16;        // EFFECT_LIST count, effects per entity
inline constexpr std::uint8_t kMaxSkillRankWire = 3;        // ranks 0..3

// ---- Opcodes (§5) ----
enum class Opcode : std::uint16_t
{
    Hello = 0x0001,
    HelloAck = 0x0002,
    Ping = 0x0010,
    Pong = 0x0011,
    Error = 0x0012,
    Logout = 0x0013,
    LoginReq = 0x0100,
    LoginRes = 0x0101,
    CharListReq = 0x0110,
    CharListRes = 0x0111,
    CharCreateReq = 0x0112,
    CharCreateRes = 0x0113,
    CharDeleteReq = 0x0114,
    CharDeleteRes = 0x0115,
    EnterWorldReq = 0x0120,
    EnterWorldRes = 0x0121,
    LeaveWorldReq = 0x0122,
    LeaveWorldRes = 0x0123,
    MoveReq = 0x0200,
    EntitySpawn = 0x0201,
    EntityDespawn = 0x0202,
    EntityMove = 0x0203,
    PositionCorrect = 0x0204,
    TargetReq = 0x0300,
    AttackReq = 0x0301,
    AttackStopReq = 0x0302,
    SkillReq = 0x0303,
    ActionFail = 0x0304,
    CombatEvent = 0x0305,
    CastStart = 0x0306,
    CastEnd = 0x0307,
    EntityStats = 0x0308,
    SelfStats = 0x0309,
    XpGain = 0x030A,
    LevelUp = 0x030B,
    EntityDeath = 0x030C,
    RespawnReq = 0x030D,
    RespawnRes = 0x030E,
    EntityState = 0x030F,
    StanceReq = 0x0310,
    Inventory = 0x0400,
    ItemMoveReq = 0x0401,
    ItemMoveRes = 0x0402,
    ItemUpdate = 0x0403,
    EntityEquip = 0x0404,
    LootOpenReq = 0x0500,
    LootOpenRes = 0x0501,
    LootContents = 0x0502,
    LootTakeReq = 0x0503,
    LootTakeRes = 0x0504,
    LootCloseReq = 0x0505,
    LootClosed = 0x0506,
    Coins = 0x0507,
    CharStats = 0x0600,
    StatAllocateReq = 0x0601,
    StatAllocateRes = 0x0602,
    SkillState = 0x0700,
    SkillLearnReq = 0x0701,
    SkillLearnRes = 0x0702,
    SkillBar = 0x0703,
    SkillBarSetReq = 0x0704,
    SkillBarSetRes = 0x0705,
    SkillCooldowns = 0x0706,
    EffectList = 0x0707,
    EffectApply = 0x0708,
    EffectRemove = 0x0709,
    SkillLeap = 0x070A,
    SkillUsed = 0x070B,
    ZoneChangeBegin = 0x0800,
    ZoneChange = 0x0801,
    ZoneLoadedReq = 0x0802,
    ZoneGateRefused = 0x0803,
};

// Name for logs ("LOGIN_REQ"); "UNKNOWN" for values not in the table.
[[nodiscard]] std::string_view opcodeName(Opcode op) noexcept;
[[nodiscard]] bool isKnownOpcode(std::uint16_t value) noexcept;

// ---- ERROR.error_code (§6) ----
enum class ErrorCode : std::uint16_t
{
    ProtocolViolation = 1,
    BadState = 2,
    Flood = 3,
    Timeout = 4,
    KickedDuplicateLogin = 5,
    ServerShutdown = 6,
    InternalError = 7,
};
[[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

// ---- Result enums ----
enum class HelloResult : std::uint8_t
{
    Ok = 0,
    VersionMismatch = 1,
    ServerFull = 2,
    Maintenance = 3,
};
inline constexpr std::uint8_t kHelloResultMax = 3;

enum class LoginResult : std::uint8_t
{
    Ok = 0,
    InvalidCredentials = 1,
    Banned = 2,
    AlreadyLoggedIn = 3, // unused: the old session is kicked instead
    TooManyAttempts = 4,
    ServerError = 5,
};
inline constexpr std::uint8_t kLoginResultMax = 5;

enum class CharCreateResult : std::uint8_t
{
    Ok = 0,
    NameInvalid = 1,
    NameTaken = 2,
    NoFreeSlot = 3,
    InvalidRaceOrClass = 4,
    InvalidAppearance = 5,
    ServerError = 6,
};
inline constexpr std::uint8_t kCharCreateResultMax = 6;

enum class CharDeleteResult : std::uint8_t
{
    Ok = 0,
    NotFound = 1,
    ServerError = 2,
};
inline constexpr std::uint8_t kCharDeleteResultMax = 2;

enum class EnterWorldResult : std::uint8_t
{
    Ok = 0,
    NotFound = 1,
    ServerError = 2,
};
inline constexpr std::uint8_t kEnterWorldResultMax = 2;

// ITEM_MOVE_RES.result (docs/PROTOCOL_ITEMS.md §3).
enum class ItemMoveResult : std::uint8_t
{
    Ok = 0,
    NoItem = 1,      // `from` is empty
    BadSlot = 2,     // out of range, or from == to
    WrongSlot = 3,   // the template is not allowed there
    WrongClass = 4,
    LevelTooLow = 5,
    SlotBlocked = 6, // OffHand while MainHand holds a two-handed item
    BagFull = 7,
    ServerError = 8,
    AttributeTooLow = 9, // v5 (PROTOCOL_STATS.md §5): an attribute requirement is not met
};
inline constexpr std::uint8_t kItemMoveResultMax = 9;
[[nodiscard]] std::string_view itemMoveResultName(ItemMoveResult r) noexcept;

// LEAVE_WORLD_RES: the spec only defines "u8 result" (0 = ok, back to CharSelect). Other values are
// carried as-is and mean failure.
inline constexpr std::uint8_t kLeaveWorldOk = 0;

// MOVE_REQ / ENTITY_* flags.
inline constexpr std::uint8_t kMoveFlagMoving = 0x01;
inline constexpr std::uint8_t kMoveFlagRunning = 0x02;

// ENTITY_SPAWN.entity_type
inline constexpr std::uint8_t kEntityTypePlayer = 1;
inline constexpr std::uint8_t kEntityTypeMonster = 2; // v3 (docs/PROTOCOL_COMBAT.md §1)
inline constexpr std::uint8_t kEntityTypeLoot = 3;    // v4 (docs/PROTOCOL_LOOT.md §1.3)

// ---- Combat (docs/PROTOCOL_COMBAT.md §3) ----

// ACTION_FAIL.action
enum class CombatAction : std::uint8_t
{
    Attack = 1,
    Skill = 2,
    Respawn = 3,
    Stance = 4,
};
inline constexpr std::uint8_t kCombatActionMin = 1;
inline constexpr std::uint8_t kCombatActionMax = 4;

// ACTION_FAIL.reason
enum class ActionFailReason : std::uint8_t
{
    NoTarget = 1,
    OutOfRange = 2,
    NotAttackable = 3,
    TargetDead = 4,
    YouAreDead = 5,
    OnCooldown = 6,
    NotEnoughMp = 7,
    WrongTargetType = 8,
    Interrupted = 9,
    InSafeZone = 10,
    UnknownSkill = 11,
    Busy = 12,
    NotLearned = 13, // v6: the skill has rank 0 (PROTOCOL_SKILLS.md §1.8)
    NoPath = 14,     // v6: a leap found no walkable landing point
};
inline constexpr std::uint8_t kActionFailReasonMin = 1;
inline constexpr std::uint8_t kActionFailReasonMax = 14;
[[nodiscard]] std::string_view actionFailReasonName(ActionFailReason r) noexcept;

// COMBAT_EVENT.kind
inline constexpr std::uint8_t kCombatKindDamage = 1;
inline constexpr std::uint8_t kCombatKindHeal = 2;
inline constexpr std::uint8_t kCombatKindMiss = 3; // v5: a dodged monster attack (PROTOCOL_STATS.md §2.1)
inline constexpr std::uint8_t kCombatKindMax = 3;
// COMBAT_EVENT.flags
inline constexpr std::uint8_t kCombatFlagCritical = 0x01;
inline constexpr std::uint8_t kCombatFlagKillingBlow = 0x02;
inline constexpr std::uint8_t kCombatFlagPeriodic = 0x04; // v6: a damage / heal over time tick
inline constexpr std::uint8_t kCombatFlagAbsorbed = 0x08; // v6: a shield took (part of) the damage
inline constexpr std::uint8_t kCombatKnownFlags =
    kCombatFlagCritical | kCombatFlagKillingBlow | kCombatFlagPeriodic | kCombatFlagAbsorbed;

// CAST_END.result
inline constexpr std::uint8_t kCastCompleted = 0;
inline constexpr std::uint8_t kCastInterrupted = 1;

// ENTITY_SPAWN.state / ENTITY_STATE.state bits
inline constexpr std::uint8_t kEntityStateDead = 0x01;
inline constexpr std::uint8_t kEntityStateInCombat = 0x02;
inline constexpr std::uint8_t kEntityStateSitting = 0x04;
// v4, loot containers only (PROTOCOL_LOOT.md §1.4), per recipient.
inline constexpr std::uint8_t kEntityStateLootLocked = 0x08; // owner window running, recipient not the owner
inline constexpr std::uint8_t kEntityStateLootMine = 0x10;   // owner window running, recipient is the owner
inline constexpr std::uint8_t kEntityStateKnown = kEntityStateDead | kEntityStateInCombat | kEntityStateSitting |
                                                  kEntityStateLootLocked | kEntityStateLootMine;

// RESPAWN_RES.result
enum class RespawnResult : std::uint8_t
{
    Ok = 0,
    NotDead = 1,
    TooEarly = 2,
};
inline constexpr std::uint8_t kRespawnResultMax = 2;

// STANCE_REQ.stance
inline constexpr std::uint8_t kStanceStand = 0;
inline constexpr std::uint8_t kStanceSit = 1;
inline constexpr std::uint8_t kStanceMax = 1;

// ---- Loot (docs/PROTOCOL_LOOT.md §4) ----

// LOOT_OPEN_RES.result / LOOT_TAKE_RES.result
enum class LootResult : std::uint8_t
{
    Ok = 0,
    NotFound = 1,     // not a loot container spawned to the requester
    YouAreDead = 2,
    OutOfRange = 3,   // farther than 3 m (+ 0.5 m tolerance)
    NotYours = 4,     // the owner window is running
    AlreadyTaken = 5, // take: empty / bad index / being taken right now / nothing left
    BagFull = 6,      // take: some item units did not fit (what fit was taken)
    PurseFull = 7,    // take: the balance would pass kMaxCoins (what fit was taken)
    ServerError = 8,  // take: nothing taken (write failed, or too many takes in flight)
};
inline constexpr std::uint8_t kLootResultMax = 8;
[[nodiscard]] std::string_view lootResultName(LootResult r) noexcept;

// LOOT_TAKE_REQ.what besides an entry index 0..7
inline constexpr std::uint8_t kLootTakeCoins = 0xFE;
inline constexpr std::uint8_t kLootTakeAll = 0xFF;

// LOOT_CLOSED.reason
enum class LootClosedReason : std::uint8_t
{
    Gone = 1,       // emptied or expired; ENTITY_DESPAWN follows
    OutOfRange = 2, // the viewer is farther than 6 m
    YouDied = 3,
};
inline constexpr std::uint8_t kLootClosedReasonMin = 1;
inline constexpr std::uint8_t kLootClosedReasonMax = 3;

// ---- Stats (docs/PROTOCOL_STATS.md §3) ----

// STAT_ALLOCATE_RES.result
enum class StatAllocateResult : std::uint8_t
{
    Ok = 0,
    InvalidAmount = 1,   // all five amounts are 0
    NotEnoughPoints = 2, // more than the unspent points
    AttributeCap = 3,    // an attribute would pass 255 (class start + allocated)
    ServerError = 4,     // too many waiting, or the write failed (nothing changed)
};
inline constexpr std::uint8_t kStatAllocateResultMax = 4;
[[nodiscard]] std::string_view statAllocateResultName(StatAllocateResult r) noexcept;

// ---- Skills (docs/PROTOCOL_SKILLS.md §3) ----

// SKILL_LEARN_RES.result
enum class SkillLearnResult : std::uint8_t
{
    Ok = 0,
    UnknownSkill = 1,    // no such skill, or another class's
    MaxRank = 2,         // already rank 3
    NotEnoughPoints = 3, // no unspent skill point
    TierLocked = 4,      // not enough points spent in the branch
    ServerError = 5,     // too many waiting, or the write failed (nothing changed)
};
inline constexpr std::uint8_t kSkillLearnResultMax = 5;
[[nodiscard]] std::string_view skillLearnResultName(SkillLearnResult r) noexcept;

// SKILL_BAR_SET_RES.result
enum class SkillBarResult : std::uint8_t
{
    Ok = 0,
    NotLearned = 1,  // a slot holds an unknown, foreign or unlearned skill (nothing changed)
    ServerError = 2, // too many waiting, or the write failed (nothing changed)
};
inline constexpr std::uint8_t kSkillBarResultMax = 2;
[[nodiscard]] std::string_view skillBarResultName(SkillBarResult r) noexcept;

// Effect.kind (PROTOCOL_SKILLS.md §1.5) on the wire: 1..13 (game::EffectKind).
inline constexpr std::uint8_t kEffectKindMin = 1;
inline constexpr std::uint8_t kEffectKindMax = 13;

// EFFECT_REMOVE.reason
enum class EffectRemoveReason : std::uint8_t
{
    Expired = 1,
    Depleted = 2, // a shield was used up
    Cleared = 3,  // e.g. a monster started returning home
};
inline constexpr std::uint8_t kEffectRemoveReasonMin = 1;
inline constexpr std::uint8_t kEffectRemoveReasonMax = 3;

// ---- Zones (docs/PROTOCOL_ZONES.md §3) ----

// ZONE_CHANGE.result
enum class ZoneChangeResult : std::uint8_t
{
    Ok = 0,     // in the target zone
    Failed = 1, // the arrival could not be stored: back in the zone the player came from
};
inline constexpr std::uint8_t kZoneChangeResultMax = 1;

// ZONE_GATE_REFUSED.reason
enum class GateRefusal : std::uint8_t
{
    RaceForbidden = 1, // the target zone is the other race's homeland
};
inline constexpr std::uint8_t kGateRefusalMin = 1;
inline constexpr std::uint8_t kGateRefusalMax = 1;

} // namespace vo::net
