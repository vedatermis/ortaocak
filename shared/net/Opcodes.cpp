#include "shared/net/Opcodes.h"

namespace vo::net {

std::string_view opcodeName(Opcode op) noexcept
{
    switch (op)
    {
    case Opcode::Hello: return "HELLO";
    case Opcode::HelloAck: return "HELLO_ACK";
    case Opcode::Ping: return "PING";
    case Opcode::Pong: return "PONG";
    case Opcode::Error: return "ERROR";
    case Opcode::Logout: return "LOGOUT";
    case Opcode::LoginReq: return "LOGIN_REQ";
    case Opcode::LoginRes: return "LOGIN_RES";
    case Opcode::CharListReq: return "CHAR_LIST_REQ";
    case Opcode::CharListRes: return "CHAR_LIST_RES";
    case Opcode::CharCreateReq: return "CHAR_CREATE_REQ";
    case Opcode::CharCreateRes: return "CHAR_CREATE_RES";
    case Opcode::CharDeleteReq: return "CHAR_DELETE_REQ";
    case Opcode::CharDeleteRes: return "CHAR_DELETE_RES";
    case Opcode::EnterWorldReq: return "ENTER_WORLD_REQ";
    case Opcode::EnterWorldRes: return "ENTER_WORLD_RES";
    case Opcode::LeaveWorldReq: return "LEAVE_WORLD_REQ";
    case Opcode::LeaveWorldRes: return "LEAVE_WORLD_RES";
    case Opcode::MoveReq: return "MOVE_REQ";
    case Opcode::EntitySpawn: return "ENTITY_SPAWN";
    case Opcode::EntityDespawn: return "ENTITY_DESPAWN";
    case Opcode::EntityMove: return "ENTITY_MOVE";
    case Opcode::PositionCorrect: return "POSITION_CORRECT";
    case Opcode::Inventory: return "INVENTORY";
    case Opcode::ItemMoveReq: return "ITEM_MOVE_REQ";
    case Opcode::ItemMoveRes: return "ITEM_MOVE_RES";
    case Opcode::ItemUpdate: return "ITEM_UPDATE";
    case Opcode::EntityEquip: return "ENTITY_EQUIP";
    }
    return "UNKNOWN";
}

bool isKnownOpcode(std::uint16_t value) noexcept
{
    return opcodeName(static_cast<Opcode>(value)) != "UNKNOWN";
}

std::string_view errorCodeName(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::ProtocolViolation: return "ProtocolViolation";
    case ErrorCode::BadState: return "BadState";
    case ErrorCode::Flood: return "Flood";
    case ErrorCode::Timeout: return "Timeout";
    case ErrorCode::KickedDuplicateLogin: return "KickedDuplicateLogin";
    case ErrorCode::ServerShutdown: return "ServerShutdown";
    case ErrorCode::InternalError: return "InternalError";
    }
    return "Unknown";
}

std::string_view itemMoveResultName(ItemMoveResult r) noexcept
{
    switch (r)
    {
    case ItemMoveResult::Ok: return "Ok";
    case ItemMoveResult::NoItem: return "NoItem";
    case ItemMoveResult::BadSlot: return "BadSlot";
    case ItemMoveResult::WrongSlot: return "WrongSlot";
    case ItemMoveResult::WrongClass: return "WrongClass";
    case ItemMoveResult::LevelTooLow: return "LevelTooLow";
    case ItemMoveResult::SlotBlocked: return "SlotBlocked";
    case ItemMoveResult::BagFull: return "BagFull";
    case ItemMoveResult::ServerError: return "ServerError";
    }
    return "Unknown";
}

} // namespace vo::net
