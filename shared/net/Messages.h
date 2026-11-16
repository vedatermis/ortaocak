// One plain struct per packet of docs/PROTOCOL.md §5 and docs/PROTOCOL_ITEMS.md §3 (plus CharSummary
// and ItemEntry).
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
