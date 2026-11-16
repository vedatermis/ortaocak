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
    case Opcode::TargetReq: return "TARGET_REQ";
    case Opcode::AttackReq: return "ATTACK_REQ";
    case Opcode::AttackStopReq: return "ATTACK_STOP_REQ";
    case Opcode::SkillReq: return "SKILL_REQ";
    case Opcode::ActionFail: return "ACTION_FAIL";
    case Opcode::CombatEvent: return "COMBAT_EVENT";
    case Opcode::CastStart: return "CAST_START";
    case Opcode::CastEnd: return "CAST_END";
    case Opcode::EntityStats: return "ENTITY_STATS";
    case Opcode::SelfStats: return "SELF_STATS";
    case Opcode::XpGain: return "XP_GAIN";
    case Opcode::LevelUp: return "LEVEL_UP";
    case Opcode::EntityDeath: return "ENTITY_DEATH";
    case Opcode::RespawnReq: return "RESPAWN_REQ";
    case Opcode::RespawnRes: return "RESPAWN_RES";
    case Opcode::EntityState: return "ENTITY_STATE";
    case Opcode::StanceReq: return "STANCE_REQ";
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

std::string_view actionFailReasonName(ActionFailReason r) noexcept
{
    switch (r)
    {
    case ActionFailReason::NoTarget: return "NoTarget";
    case ActionFailReason::OutOfRange: return "OutOfRange";
    case ActionFailReason::NotAttackable: return "NotAttackable";
    case ActionFailReason::TargetDead: return "TargetDead";
    case ActionFailReason::YouAreDead: return "YouAreDead";
    case ActionFailReason::OnCooldown: return "OnCooldown";
    case ActionFailReason::NotEnoughMp: return "NotEnoughMp";
    case ActionFailReason::WrongTargetType: return "WrongTargetType";
    case ActionFailReason::Interrupted: return "Interrupted";
    case ActionFailReason::InSafeZone: return "InSafeZone";
    case ActionFailReason::UnknownSkill: return "UnknownSkill";
    case ActionFailReason::Busy: return "Busy";
    }
    return "Unknown";
}

} // namespace vo::net
