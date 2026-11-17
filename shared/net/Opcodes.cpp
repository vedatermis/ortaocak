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
    case Opcode::LootOpenReq: return "LOOT_OPEN_REQ";
    case Opcode::LootOpenRes: return "LOOT_OPEN_RES";
    case Opcode::LootContents: return "LOOT_CONTENTS";
    case Opcode::LootTakeReq: return "LOOT_TAKE_REQ";
    case Opcode::LootTakeRes: return "LOOT_TAKE_RES";
    case Opcode::LootCloseReq: return "LOOT_CLOSE_REQ";
    case Opcode::LootClosed: return "LOOT_CLOSED";
    case Opcode::Coins: return "COINS";
    case Opcode::CharStats: return "CHAR_STATS";
    case Opcode::StatAllocateReq: return "STAT_ALLOCATE_REQ";
    case Opcode::StatAllocateRes: return "STAT_ALLOCATE_RES";
    case Opcode::SkillState: return "SKILL_STATE";
    case Opcode::SkillLearnReq: return "SKILL_LEARN_REQ";
    case Opcode::SkillLearnRes: return "SKILL_LEARN_RES";
    case Opcode::SkillBar: return "SKILL_BAR";
    case Opcode::SkillBarSetReq: return "SKILL_BAR_SET_REQ";
    case Opcode::SkillBarSetRes: return "SKILL_BAR_SET_RES";
    case Opcode::SkillCooldowns: return "SKILL_COOLDOWNS";
    case Opcode::EffectList: return "EFFECT_LIST";
    case Opcode::EffectApply: return "EFFECT_APPLY";
    case Opcode::EffectRemove: return "EFFECT_REMOVE";
    case Opcode::SkillLeap: return "SKILL_LEAP";
    case Opcode::SkillUsed: return "SKILL_USED";
    case Opcode::ZoneChangeBegin: return "ZONE_CHANGE_BEGIN";
    case Opcode::ZoneChange: return "ZONE_CHANGE";
    case Opcode::ZoneLoadedReq: return "ZONE_LOADED_REQ";
    case Opcode::ZoneGateRefused: return "ZONE_GATE_REFUSED";
    case Opcode::ShopOpenReq: return "SHOP_OPEN_REQ";
    case Opcode::ShopOpenRes: return "SHOP_OPEN_RES";
    case Opcode::ShopCloseReq: return "SHOP_CLOSE_REQ";
    case Opcode::ShopClosed: return "SHOP_CLOSED";
    case Opcode::ShopBuyReq: return "SHOP_BUY_REQ";
    case Opcode::ShopBuyRes: return "SHOP_BUY_RES";
    case Opcode::ShopSellReq: return "SHOP_SELL_REQ";
    case Opcode::ShopSellRes: return "SHOP_SELL_RES";
    case Opcode::ItemUseReq: return "ITEM_USE_REQ";
    case Opcode::ItemUseRes: return "ITEM_USE_RES";
    case Opcode::ItemCooldown: return "ITEM_COOLDOWN";
    case Opcode::ItemUsed: return "ITEM_USED";
    }
    return "UNKNOWN";
}

std::string_view shopResultName(ShopResult r) noexcept
{
    switch (r)
    {
    case ShopResult::Ok: return "Ok";
    case ShopResult::NotFound: return "NotFound";
    case ShopResult::YouAreDead: return "YouAreDead";
    case ShopResult::OutOfRange: return "OutOfRange";
    case ShopResult::NotOpen: return "NotOpen";
    case ShopResult::NotInStock: return "NotInStock";
    case ShopResult::BadQuantity: return "BadQuantity";
    case ShopResult::NotEnoughCoins: return "NotEnoughCoins";
    case ShopResult::BagFull: return "BagFull";
    case ShopResult::NoItem: return "NoItem";
    case ShopResult::CannotSell: return "CannotSell";
    case ShopResult::PurseFull: return "PurseFull";
    case ShopResult::ServerError: return "ServerError";
    }
    return "Unknown";
}

std::string_view itemUseResultName(ItemUseResult r) noexcept
{
    switch (r)
    {
    case ItemUseResult::Ok: return "Ok";
    case ItemUseResult::NoItem: return "NoItem";
    case ItemUseResult::NotUsable: return "NotUsable";
    case ItemUseResult::YouAreDead: return "YouAreDead";
    case ItemUseResult::OnCooldown: return "OnCooldown";
    case ItemUseResult::AlreadyFull: return "AlreadyFull";
    case ItemUseResult::Stunned: return "Stunned";
    case ItemUseResult::ServerError: return "ServerError";
    }
    return "Unknown";
}

std::string_view lootResultName(LootResult r) noexcept
{
    switch (r)
    {
    case LootResult::Ok: return "Ok";
    case LootResult::NotFound: return "NotFound";
    case LootResult::YouAreDead: return "YouAreDead";
    case LootResult::OutOfRange: return "OutOfRange";
    case LootResult::NotYours: return "NotYours";
    case LootResult::AlreadyTaken: return "AlreadyTaken";
    case LootResult::BagFull: return "BagFull";
    case LootResult::PurseFull: return "PurseFull";
    case LootResult::ServerError: return "ServerError";
    }
    return "Unknown";
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
    case ItemMoveResult::AttributeTooLow: return "AttributeTooLow";
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
    case ActionFailReason::NotLearned: return "NotLearned";
    case ActionFailReason::NoPath: return "NoPath";
    }
    return "Unknown";
}

std::string_view statAllocateResultName(StatAllocateResult r) noexcept
{
    switch (r)
    {
    case StatAllocateResult::Ok: return "Ok";
    case StatAllocateResult::InvalidAmount: return "InvalidAmount";
    case StatAllocateResult::NotEnoughPoints: return "NotEnoughPoints";
    case StatAllocateResult::AttributeCap: return "AttributeCap";
    case StatAllocateResult::ServerError: return "ServerError";
    }
    return "Unknown";
}

std::string_view skillLearnResultName(SkillLearnResult r) noexcept
{
    switch (r)
    {
    case SkillLearnResult::Ok: return "Ok";
    case SkillLearnResult::UnknownSkill: return "UnknownSkill";
    case SkillLearnResult::MaxRank: return "MaxRank";
    case SkillLearnResult::NotEnoughPoints: return "NotEnoughPoints";
    case SkillLearnResult::TierLocked: return "TierLocked";
    case SkillLearnResult::ServerError: return "ServerError";
    }
    return "Unknown";
}

std::string_view skillBarResultName(SkillBarResult r) noexcept
{
    switch (r)
    {
    case SkillBarResult::Ok: return "Ok";
    case SkillBarResult::NotLearned: return "NotLearned";
    case SkillBarResult::ServerError: return "ServerError";
    }
    return "Unknown";
}

} // namespace vo::net
