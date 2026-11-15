// Wire protocol constants (docs/PROTOCOL.md v1): opcodes, error codes, result enums and limits.
// This header is the single source of truth for numbers that appear on the wire.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vo::net {

// ---- Handshake (§3) ----
inline constexpr std::uint32_t kMagic = 0x4C4E4F56; // "VONL" on the wire (little-endian)
inline constexpr std::uint16_t kProtocolVersion = 1;
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

// LEAVE_WORLD_RES: the spec only defines "u8 result" (0 = ok, back to CharSelect). Other values are
// carried as-is and mean failure.
inline constexpr std::uint8_t kLeaveWorldOk = 0;

// MOVE_REQ / ENTITY_* flags.
inline constexpr std::uint8_t kMoveFlagMoving = 0x01;
inline constexpr std::uint8_t kMoveFlagRunning = 0x02;

// ENTITY_SPAWN.entity_type
inline constexpr std::uint8_t kEntityTypePlayer = 1;

} // namespace vo::net
