#include "shared/net/Messages.h"

#include <algorithm>
#include <cmath>

namespace vo::net {

namespace {

// Reads a u8 result code and rejects values above `max`.
template <class E>
E readResult(PacketReader& r, std::uint8_t max)
{
    const std::uint8_t v = r.readU8();
    if (v > max)
        r.fail();
    return static_cast<E>(v);
}

} // namespace

// ---- Hello ----
void Hello::write(PacketWriter& w) const
{
    w.writeU32(magic);
    w.writeU16(protocolVersion);
    w.writeU32(clientBuild);
    w.writeBytes(clientKxPk);
}

bool Hello::read(PacketReader& r, Hello& out)
{
    out.magic = r.readU32();
    out.protocolVersion = r.readU16();
    out.clientBuild = r.readU32();
    r.readBytes(out.clientKxPk);
    return r.ok();
}

// ---- HelloAck ----
void HelloAck::write(PacketWriter& w) const
{
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU16(serverProtocolVersion);
    w.writeBytes(serverKxPk);
    w.writeBytes(signature);
}

bool HelloAck::read(PacketReader& r, HelloAck& out)
{
    out.result = readResult<HelloResult>(r, kHelloResultMax);
    out.serverProtocolVersion = r.readU16();
    r.readBytes(out.serverKxPk);
    r.readBytes(out.signature);
    return r.ok();
}

// ---- Ping / Pong ----
void Ping::write(PacketWriter& w) const
{
    w.writeU32(clientTimeMs);
}

bool Ping::read(PacketReader& r, Ping& out)
{
    out.clientTimeMs = r.readU32();
    return r.ok();
}

void Pong::write(PacketWriter& w) const
{
    w.writeU32(clientTimeMs);
    w.writeU32(serverTimeMs);
}

bool Pong::read(PacketReader& r, Pong& out)
{
    out.clientTimeMs = r.readU32();
    out.serverTimeMs = r.readU32();
    return r.ok();
}

// ---- Error ----
void Error::write(PacketWriter& w) const
{
    w.writeU16(static_cast<std::uint16_t>(errorCode));
    w.writeStr8(debugMessage);
}

bool Error::read(PacketReader& r, Error& out)
{
    out.errorCode = static_cast<ErrorCode>(r.readU16());
    out.debugMessage = r.readStr8();
    return r.ok();
}

// ---- Logout ----
void Logout::write(PacketWriter&) const {}

bool Logout::read(PacketReader& r, Logout&)
{
    return r.ok();
}

// ---- Login ----
void LoginReq::write(PacketWriter& w) const
{
    w.writeStr8(username);
    w.writeStr8(password);
}

bool LoginReq::read(PacketReader& r, LoginReq& out)
{
    out.username = r.readStr8();
    out.password = r.readStr8();
    return r.ok();
}

void LoginRes::write(PacketWriter& w) const
{
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU32(accountId);
}

bool LoginRes::read(PacketReader& r, LoginRes& out)
{
    out.result = readResult<LoginResult>(r, kLoginResultMax);
    out.accountId = r.readU32();
    return r.ok();
}

// ---- CharSummary ----
void CharSummary::write(PacketWriter& w) const
{
    w.writeU32(charId);
    w.writeStr8(name);
    w.writeU8(race);
    w.writeU8(charClass);
    w.writeU8(face);
    w.writeU8(hair);
    w.writeU16(level);
    w.writeU16(zoneId);
    w.writeU16(mainHandTemplate);
    w.writeU16(offHandTemplate);
    for (const std::uint16_t a : armorTemplates) // M17
        w.writeU16(a);
}

bool CharSummary::read(PacketReader& r, CharSummary& out)
{
    out.charId = r.readU32();
    out.name = r.readStr8();
    out.race = r.readU8();
    out.charClass = r.readU8();
    out.face = r.readU8();
    out.hair = r.readU8();
    out.level = r.readU16();
    out.zoneId = r.readU16();
    out.mainHandTemplate = r.readU16();
    out.offHandTemplate = r.readU16();
    for (std::uint16_t& a : out.armorTemplates) // M17
        a = r.readU16();
    return r.ok();
}

// ---- Character list ----
void CharListReq::write(PacketWriter&) const {}

bool CharListReq::read(PacketReader& r, CharListReq&)
{
    return r.ok();
}

void CharListRes::write(PacketWriter& w) const
{
    if (characters.size() > kMaxCharactersPerAccount)
    {
        w.fail();
        return;
    }
    w.writeU8(static_cast<std::uint8_t>(characters.size()));
    for (const auto& c : characters)
        c.write(w);
}

bool CharListRes::read(PacketReader& r, CharListRes& out)
{
    const std::uint8_t count = r.readU8();
    if (count > kMaxCharactersPerAccount)
    {
        r.fail();
        return false;
    }
    out.characters.clear();
    out.characters.resize(count);
    for (auto& c : out.characters)
    {
        if (!CharSummary::read(r, c))
            return false;
    }
    return r.ok();
}

// ---- Character create ----
void CharCreateReq::write(PacketWriter& w) const
{
    w.writeStr8(name);
    w.writeU8(race);
    w.writeU8(charClass);
    w.writeU8(face);
    w.writeU8(hair);
}

bool CharCreateReq::read(PacketReader& r, CharCreateReq& out)
{
    out.name = r.readStr8();
    out.race = r.readU8();
    out.charClass = r.readU8();
    out.face = r.readU8();
    out.hair = r.readU8();
    return r.ok();
}

void CharCreateRes::write(PacketWriter& w) const
{
    w.writeU8(static_cast<std::uint8_t>(result));
    if (result == CharCreateResult::Ok)
        character.write(w);
}

bool CharCreateRes::read(PacketReader& r, CharCreateRes& out)
{
    out.result = readResult<CharCreateResult>(r, kCharCreateResultMax);
    out.character = {};
    if (r.ok() && out.result == CharCreateResult::Ok)
        CharSummary::read(r, out.character);
    return r.ok();
}

// ---- Character delete ----
void CharDeleteReq::write(PacketWriter& w) const
{
    w.writeU32(charId);
}

bool CharDeleteReq::read(PacketReader& r, CharDeleteReq& out)
{
    out.charId = r.readU32();
    return r.ok();
}

void CharDeleteRes::write(PacketWriter& w) const
{
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU32(charId);
}

bool CharDeleteRes::read(PacketReader& r, CharDeleteRes& out)
{
    out.result = readResult<CharDeleteResult>(r, kCharDeleteResultMax);
    out.charId = r.readU32();
    return r.ok();
}

// ---- Enter / leave world ----
void EnterWorldReq::write(PacketWriter& w) const
{
    w.writeU32(charId);
}

bool EnterWorldReq::read(PacketReader& r, EnterWorldReq& out)
{
    out.charId = r.readU32();
    return r.ok();
}

void EnterWorldRes::write(PacketWriter& w) const
{
    w.writeU8(static_cast<std::uint8_t>(result));
    if (result != EnterWorldResult::Ok)
        return;
    w.writeU32(entityId);
    w.writeU16(zoneId);
    w.writeF32(x);
    w.writeF32(y);
    w.writeF32(z);
    w.writeF32(yaw);
    w.writeF32(walkSpeed);
    w.writeF32(runSpeed);
    w.writeU32(serverTimeMs);
    w.writeU16(level);
    w.writeU32(xp);
    w.writeU32(xpToNext);
    w.writeU32(hp);
    w.writeU32(maxHp);
    w.writeU32(mp);
    w.writeU32(maxMp);
    w.writeU16(attack);
    w.writeU16(defense);
    w.writeBool(isDead);
}

bool EnterWorldRes::read(PacketReader& r, EnterWorldRes& out)
{
    const EnterWorldResult result = readResult<EnterWorldResult>(r, kEnterWorldResultMax);
    out = {};
    out.result = result;
    if (!r.ok() || result != EnterWorldResult::Ok)
        return r.ok();
    out.entityId = r.readU32();
    out.zoneId = r.readU16();
    out.x = r.readF32();
    out.y = r.readF32();
    out.z = r.readF32();
    out.yaw = r.readF32();
    out.walkSpeed = r.readF32();
    out.runSpeed = r.readF32();
    out.serverTimeMs = r.readU32();
    out.level = r.readU16();
    out.xp = r.readU32();
    out.xpToNext = r.readU32();
    out.hp = r.readU32();
    out.maxHp = r.readU32();
    out.mp = r.readU32();
    out.maxMp = r.readU32();
    out.attack = r.readU16();
    out.defense = r.readU16();
    out.isDead = r.readBool();
    return r.ok();
}

void LeaveWorldReq::write(PacketWriter&) const {}

bool LeaveWorldReq::read(PacketReader& r, LeaveWorldReq&)
{
    return r.ok();
}

void LeaveWorldRes::write(PacketWriter& w) const
{
    w.writeU8(result);
}

bool LeaveWorldRes::read(PacketReader& r, LeaveWorldRes& out)
{
    out.result = r.readU8();
    return r.ok();
}

// ---- World ----
void MoveReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU8(flags);
    w.writeF32(x);
    w.writeF32(y);
    w.writeF32(z);
    w.writeF32(yaw);
}

bool MoveReq::read(PacketReader& r, MoveReq& out)
{
    out.seq = r.readU32();
    out.flags = r.readU8();
    out.x = r.readF32();
    out.y = r.readF32();
    out.z = r.readF32();
    out.yaw = r.readF32();
    return r.ok();
}

void EntitySpawn::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU8(entityType);
    w.writeStr8(name);
    w.writeU8(race);
    w.writeU8(charClass);
    w.writeU8(face);
    w.writeU8(hair);
    w.writeU16(level);
    w.writeF32(x);
    w.writeF32(y);
    w.writeF32(z);
    w.writeF32(yaw);
    w.writeU8(flags);
    w.writeU16(mainHandTemplate);
    w.writeU16(offHandTemplate);
    w.writeU32(hp);
    w.writeU32(maxHp);
    w.writeU16(templateId);
    w.writeU8(state);
    for (const std::uint16_t a : armorTemplates) // M17 (PROTOCOL_ITEMS.md §2.1)
        w.writeU16(a);
}

bool EntitySpawn::read(PacketReader& r, EntitySpawn& out)
{
    out.entityId = r.readU32();
    out.entityType = r.readU8();
    out.name = r.readStr8();
    out.race = r.readU8();
    out.charClass = r.readU8();
    out.face = r.readU8();
    out.hair = r.readU8();
    out.level = r.readU16();
    out.x = r.readF32();
    out.y = r.readF32();
    out.z = r.readF32();
    out.yaw = r.readF32();
    out.flags = r.readU8();
    out.mainHandTemplate = r.readU16();
    out.offHandTemplate = r.readU16();
    out.hp = r.readU32();
    out.maxHp = r.readU32();
    out.templateId = r.readU16();
    out.state = r.readU8();
    for (std::uint16_t& a : out.armorTemplates) // M17
        a = r.readU16();
    if ((out.state & ~kEntityStateKnown) != 0)
        r.fail();
    return r.ok();
}

void EntityDespawn::write(PacketWriter& w) const
{
    w.writeU32(entityId);
}

bool EntityDespawn::read(PacketReader& r, EntityDespawn& out)
{
    out.entityId = r.readU32();
    return r.ok();
}

void EntityMove::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU8(flags);
    w.writeF32(x);
    w.writeF32(y);
    w.writeF32(z);
    w.writeF32(yaw);
    w.writeU32(serverTimeMs);
}

bool EntityMove::read(PacketReader& r, EntityMove& out)
{
    out.entityId = r.readU32();
    out.flags = r.readU8();
    out.x = r.readF32();
    out.y = r.readF32();
    out.z = r.readF32();
    out.yaw = r.readF32();
    out.serverTimeMs = r.readU32();
    return r.ok();
}

void PositionCorrect::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeF32(x);
    w.writeF32(y);
    w.writeF32(z);
    w.writeF32(yaw);
}

bool PositionCorrect::read(PacketReader& r, PositionCorrect& out)
{
    out.seq = r.readU32();
    out.x = r.readF32();
    out.y = r.readF32();
    out.z = r.readF32();
    out.yaw = r.readF32();
    return r.ok();
}

// ---- Items ----
namespace {

template <class List>
bool readEntries(PacketReader& r, List& out, std::size_t count)
{
    out.clear();
    out.resize(count);
    for (auto& e : out)
    {
        if (!ItemEntry::read(r, e))
            return false;
    }
    return r.ok();
}

} // namespace

void ItemEntry::write(PacketWriter& w) const
{
    w.writeU8(container);
    w.writeU8(slot);
    w.writeU64(itemId);
    w.writeU16(templateId);
    w.writeU16(quantity);
    if (upgrade > kMaxUpgradeLevel)
        w.fail();
    w.writeU8(upgrade);
}

bool ItemEntry::read(PacketReader& r, ItemEntry& out)
{
    out.container = r.readU8();
    out.slot = r.readU8();
    out.itemId = r.readU64();
    out.templateId = r.readU16();
    out.quantity = r.readU16();
    out.upgrade = r.readU8(); // v10 (docs/PROTOCOL_UPGRADE.md §4)
    if (out.upgrade > kMaxUpgradeLevel)
        r.fail();
    return r.ok();
}

void Inventory::write(PacketWriter& w) const
{
    if (items.size() > kMaxInventoryEntries)
    {
        w.fail();
        return;
    }
    w.writeU8(bagSize);
    w.writeU8(static_cast<std::uint8_t>(items.size()));
    for (const auto& e : items)
        e.write(w);
}

bool Inventory::read(PacketReader& r, Inventory& out)
{
    out.bagSize = r.readU8();
    const std::uint8_t count = r.readU8();
    if (count > kMaxInventoryEntries)
    {
        r.fail();
        return false;
    }
    return r.ok() && readEntries(r, out.items, count);
}

void ItemMoveReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU8(fromContainer);
    w.writeU8(fromSlot);
    w.writeU8(toContainer);
    w.writeU8(toSlot);
}

bool ItemMoveReq::read(PacketReader& r, ItemMoveReq& out)
{
    out.seq = r.readU32();
    out.fromContainer = r.readU8();
    out.fromSlot = r.readU8();
    out.toContainer = r.readU8();
    out.toSlot = r.readU8();
    return r.ok();
}

void ItemMoveRes::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU8(static_cast<std::uint8_t>(result));
}

bool ItemMoveRes::read(PacketReader& r, ItemMoveRes& out)
{
    out.seq = r.readU32();
    out.result = readResult<ItemMoveResult>(r, kItemMoveResultMax);
    return r.ok();
}

void ItemUpdate::write(PacketWriter& w) const
{
    if (items.empty() || items.size() > kMaxItemUpdateEntries)
    {
        w.fail();
        return;
    }
    w.writeU8(static_cast<std::uint8_t>(items.size()));
    for (const auto& e : items)
        e.write(w);
}

bool ItemUpdate::read(PacketReader& r, ItemUpdate& out)
{
    const std::uint8_t count = r.readU8();
    if (count == 0 || count > kMaxItemUpdateEntries)
    {
        r.fail();
        return false;
    }
    return r.ok() && readEntries(r, out.items, count);
}

void EntityEquip::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU16(mainHandTemplate);
    w.writeU16(offHandTemplate);
    for (const std::uint16_t a : armorTemplates) // M17
        w.writeU16(a);
}

bool EntityEquip::read(PacketReader& r, EntityEquip& out)
{
    out.entityId = r.readU32();
    out.mainHandTemplate = r.readU16();
    out.offHandTemplate = r.readU16();
    for (std::uint16_t& a : out.armorTemplates) // M17
        a = r.readU16();
    return r.ok();
}

// ---- Combat ----

void TargetReq::write(PacketWriter& w) const
{
    w.writeU32(entityId);
}

bool TargetReq::read(PacketReader& r, TargetReq& out)
{
    out.entityId = r.readU32();
    return r.ok();
}

void AttackReq::write(PacketWriter& w) const
{
    w.writeU32(entityId);
}

bool AttackReq::read(PacketReader& r, AttackReq& out)
{
    out.entityId = r.readU32();
    return r.ok();
}

void AttackStopReq::write(PacketWriter&) const {}

bool AttackStopReq::read(PacketReader& r, AttackStopReq&)
{
    return r.ok();
}

void SkillReq::write(PacketWriter& w) const
{
    w.writeU16(skillId);
    w.writeU32(targetId);
}

bool SkillReq::read(PacketReader& r, SkillReq& out)
{
    out.skillId = r.readU16();
    out.targetId = r.readU32();
    return r.ok();
}

void ActionFail::write(PacketWriter& w) const
{
    w.writeU8(static_cast<std::uint8_t>(action));
    w.writeU16(skillId);
    w.writeU8(static_cast<std::uint8_t>(reason));
}

bool ActionFail::read(PacketReader& r, ActionFail& out)
{
    const std::uint8_t action = r.readU8();
    out.skillId = r.readU16();
    const std::uint8_t reason = r.readU8();
    if (action < kCombatActionMin || action > kCombatActionMax || reason < kActionFailReasonMin ||
        reason > kActionFailReasonMax)
        r.fail();
    out.action = static_cast<CombatAction>(action);
    out.reason = static_cast<ActionFailReason>(reason);
    return r.ok();
}

void CombatEvent::write(PacketWriter& w) const
{
    w.writeU32(sourceId);
    w.writeU32(targetId);
    w.writeU16(skillId);
    w.writeU8(kind);
    w.writeU8(flags);
    w.writeU32(amount);
    w.writeU32(targetHp);
    w.writeU32(serverTimeMs);
}

bool CombatEvent::read(PacketReader& r, CombatEvent& out)
{
    out.sourceId = r.readU32();
    out.targetId = r.readU32();
    out.skillId = r.readU16();
    out.kind = r.readU8();
    out.flags = r.readU8();
    out.amount = r.readU32();
    out.targetHp = r.readU32();
    out.serverTimeMs = r.readU32();
    if (out.kind < kCombatKindDamage || out.kind > kCombatKindMax || (out.flags & ~kCombatKnownFlags) != 0)
        r.fail();
    return r.ok();
}

void CastStart::write(PacketWriter& w) const
{
    w.writeU32(casterId);
    w.writeU16(skillId);
    w.writeU32(targetId);
    w.writeU16(castMs);
}

bool CastStart::read(PacketReader& r, CastStart& out)
{
    out.casterId = r.readU32();
    out.skillId = r.readU16();
    out.targetId = r.readU32();
    out.castMs = r.readU16();
    return r.ok();
}

void CastEnd::write(PacketWriter& w) const
{
    w.writeU32(casterId);
    w.writeU16(skillId);
    w.writeU8(result);
}

bool CastEnd::read(PacketReader& r, CastEnd& out)
{
    out.casterId = r.readU32();
    out.skillId = r.readU16();
    out.result = r.readU8();
    if (out.result > kCastInterrupted)
        r.fail();
    return r.ok();
}

void EntityStats::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU32(hp);
    w.writeU32(maxHp);
    w.writeU16(level);
}

bool EntityStats::read(PacketReader& r, EntityStats& out)
{
    out.entityId = r.readU32();
    out.hp = r.readU32();
    out.maxHp = r.readU32();
    out.level = r.readU16();
    return r.ok();
}

void SelfStats::write(PacketWriter& w) const
{
    w.writeU32(hp);
    w.writeU32(maxHp);
    w.writeU32(mp);
    w.writeU32(maxMp);
    w.writeU16(attack);
    w.writeU16(defense);
}

bool SelfStats::read(PacketReader& r, SelfStats& out)
{
    out.hp = r.readU32();
    out.maxHp = r.readU32();
    out.mp = r.readU32();
    out.maxMp = r.readU32();
    out.attack = r.readU16();
    out.defense = r.readU16();
    return r.ok();
}

void XpGain::write(PacketWriter& w) const
{
    w.writeU32(amount);
    w.writeU32(xp);
    w.writeU32(xpToNext);
    w.writeU32(sourceEntityId);
}

bool XpGain::read(PacketReader& r, XpGain& out)
{
    out.amount = r.readU32();
    out.xp = r.readU32();
    out.xpToNext = r.readU32();
    out.sourceEntityId = r.readU32();
    return r.ok();
}

void LevelUp::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU16(level);
}

bool LevelUp::read(PacketReader& r, LevelUp& out)
{
    out.entityId = r.readU32();
    out.level = r.readU16();
    return r.ok();
}

void EntityDeath::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU32(killerId);
}

bool EntityDeath::read(PacketReader& r, EntityDeath& out)
{
    out.entityId = r.readU32();
    out.killerId = r.readU32();
    return r.ok();
}

void RespawnReq::write(PacketWriter&) const {}

bool RespawnReq::read(PacketReader& r, RespawnReq&)
{
    return r.ok();
}

void RespawnRes::write(PacketWriter& w) const
{
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeF32(x);
    w.writeF32(y);
    w.writeF32(z);
    w.writeF32(yaw);
    w.writeU32(hp);
    w.writeU32(mp);
}

bool RespawnRes::read(PacketReader& r, RespawnRes& out)
{
    out.result = readResult<RespawnResult>(r, kRespawnResultMax);
    out.x = r.readF32();
    out.y = r.readF32();
    out.z = r.readF32();
    out.yaw = r.readF32();
    out.hp = r.readU32();
    out.mp = r.readU32();
    return r.ok();
}

void EntityState::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU8(state);
}

bool EntityState::read(PacketReader& r, EntityState& out)
{
    out.entityId = r.readU32();
    out.state = r.readU8();
    if ((out.state & ~kEntityStateKnown) != 0)
        r.fail();
    return r.ok();
}

void StanceReq::write(PacketWriter& w) const
{
    w.writeU8(stance);
}

bool StanceReq::read(PacketReader& r, StanceReq& out)
{
    out.stance = r.readU8();
    if (out.stance > kStanceMax)
        r.fail();
    return r.ok();
}

// ---- Loot ----

void LootOpenReq::write(PacketWriter& w) const
{
    w.writeU32(lootId);
}

bool LootOpenReq::read(PacketReader& r, LootOpenReq& out)
{
    out.lootId = r.readU32();
    return r.ok();
}

void LootOpenRes::write(PacketWriter& w) const
{
    w.writeU32(lootId);
    w.writeU8(static_cast<std::uint8_t>(result));
}

bool LootOpenRes::read(PacketReader& r, LootOpenRes& out)
{
    out.lootId = r.readU32();
    out.result = readResult<LootResult>(r, kLootResultMax);
    return r.ok();
}

void LootEntry::write(PacketWriter& w) const
{
    w.writeU8(index);
    w.writeU16(templateId);
    w.writeU16(quantity);
}

bool LootEntry::read(PacketReader& r, LootEntry& out)
{
    out.index = r.readU8();
    out.templateId = r.readU16();
    out.quantity = r.readU16();
    if (out.index >= kMaxLootEntries || out.templateId == 0 || out.quantity == 0)
        r.fail();
    return r.ok();
}

void LootContents::write(PacketWriter& w) const
{
    if (entries.size() > kMaxLootEntries)
    {
        w.fail();
        return;
    }
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const LootEntry& e = entries[i];
        if (e.index >= kMaxLootEntries || e.templateId == 0 || e.quantity == 0 ||
            (i > 0 && entries[i - 1].index >= e.index))
        {
            w.fail();
            return;
        }
    }
    w.writeU32(lootId);
    w.writeU32(coins);
    w.writeU8(static_cast<std::uint8_t>(entries.size()));
    for (const auto& e : entries)
        e.write(w);
}

bool LootContents::read(PacketReader& r, LootContents& out)
{
    out.lootId = r.readU32();
    out.coins = r.readU32();
    const std::uint8_t count = r.readU8();
    if (!r.ok() || count > kMaxLootEntries)
    {
        r.fail();
        return false;
    }
    out.entries.clear();
    out.entries.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!LootEntry::read(r, out.entries[i]))
            return false;
        if (i > 0 && out.entries[i - 1].index >= out.entries[i].index)
        {
            r.fail();
            return false;
        }
    }
    return r.ok();
}

void LootTakeReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU32(lootId);
    w.writeU8(what);
}

bool LootTakeReq::read(PacketReader& r, LootTakeReq& out)
{
    out.seq = r.readU32();
    out.lootId = r.readU32();
    out.what = r.readU8();
    return r.ok();
}

void LootTakeRes::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU32(lootId);
    w.writeU8(static_cast<std::uint8_t>(result));
}

bool LootTakeRes::read(PacketReader& r, LootTakeRes& out)
{
    out.seq = r.readU32();
    out.lootId = r.readU32();
    out.result = readResult<LootResult>(r, kLootResultMax);
    return r.ok();
}

void LootCloseReq::write(PacketWriter& w) const
{
    w.writeU32(lootId);
}

bool LootCloseReq::read(PacketReader& r, LootCloseReq& out)
{
    out.lootId = r.readU32();
    return r.ok();
}

void LootClosed::write(PacketWriter& w) const
{
    w.writeU32(lootId);
    w.writeU8(static_cast<std::uint8_t>(reason));
}

bool LootClosed::read(PacketReader& r, LootClosed& out)
{
    out.lootId = r.readU32();
    const std::uint8_t reason = r.readU8();
    if (reason < kLootClosedReasonMin || reason > kLootClosedReasonMax)
        r.fail();
    out.reason = static_cast<LootClosedReason>(reason);
    return r.ok();
}

void Coins::write(PacketWriter& w) const
{
    w.writeU32(coins);
    w.writeU32(gained);
}

bool Coins::read(PacketReader& r, Coins& out)
{
    out.coins = r.readU32();
    out.gained = r.readU32();
    return r.ok();
}

// ---- Stats ----

void CharStats::write(PacketWriter& w) const
{
    w.writeU16(level);
    for (const auto& a : attributes)
    {
        w.writeU16(a.base);
        w.writeU16(a.bonus);
    }
    w.writeU16(unspent);
    w.writeU32(maxHp);
    w.writeU32(maxMp);
    w.writeU16(attack);
    w.writeU16(defense);
    w.writeU16(critPermille);
    w.writeU16(dodgePermille);
    w.writeU16(hpRegenTenths);
    w.writeU16(mpRegenTenths);
    w.writeU16(attackIntervalMs);
    w.writeU16(healPermille);
}

bool CharStats::read(PacketReader& r, CharStats& out)
{
    out.level = r.readU16();
    for (auto& a : out.attributes)
    {
        a.base = r.readU16();
        a.bonus = r.readU16();
        if (a.base > kStatAttributeCap)
            r.fail();
    }
    out.unspent = r.readU16();
    out.maxHp = r.readU32();
    out.maxMp = r.readU32();
    out.attack = r.readU16();
    out.defense = r.readU16();
    out.critPermille = r.readU16();
    out.dodgePermille = r.readU16();
    out.hpRegenTenths = r.readU16();
    out.mpRegenTenths = r.readU16();
    out.attackIntervalMs = r.readU16();
    out.healPermille = r.readU16();
    if (out.critPermille > kMaxPermille || out.dodgePermille > kMaxPermille || out.healPermille > kMaxPermille)
        r.fail();
    return r.ok();
}

void StatAllocateReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    for (const auto p : points)
        w.writeU16(p);
}

bool StatAllocateReq::read(PacketReader& r, StatAllocateReq& out)
{
    out.seq = r.readU32();
    for (auto& p : out.points)
        p = r.readU16();
    return r.ok();
}

void StatAllocateRes::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU16(unspent);
}

bool StatAllocateRes::read(PacketReader& r, StatAllocateRes& out)
{
    out.seq = r.readU32();
    out.result = readResult<StatAllocateResult>(r, kStatAllocateResultMax);
    out.unspent = r.readU16();
    return r.ok();
}

// ---- Skills ----

namespace {

bool validSkillList(const std::vector<SkillRankEntry>& skills) noexcept
{
    if (skills.size() > kMaxSkillStateEntries)
        return false;
    for (std::size_t i = 0; i < skills.size(); ++i)
    {
        if (skills[i].rank < 1 || skills[i].rank > kMaxSkillRankWire ||
            (i > 0 && skills[i - 1].skillId >= skills[i].skillId))
            return false;
    }
    return true;
}

bool validCooldown(const SkillCooldownEntry& e) noexcept
{
    return e.remainingMs >= 1 && e.remainingMs <= e.totalMs;
}

void writeSlots(PacketWriter& w, const SkillBarSlots& slots)
{
    for (const auto s : slots)
        w.writeU16(s);
}

void readSlots(PacketReader& r, SkillBarSlots& slots)
{
    for (auto& s : slots)
        s = r.readU16();
}

} // namespace

void SkillState::write(PacketWriter& w) const
{
    if (!validSkillList(skills))
    {
        w.fail();
        return;
    }
    w.writeU16(points);
    w.writeU16(unspent);
    w.writeU8(static_cast<std::uint8_t>(skills.size()));
    for (const auto& s : skills)
    {
        w.writeU16(s.skillId);
        w.writeU8(s.rank);
    }
}

bool SkillState::read(PacketReader& r, SkillState& out)
{
    out.points = r.readU16();
    out.unspent = r.readU16();
    const std::uint8_t count = r.readU8();
    if (!r.ok() || count > kMaxSkillStateEntries)
    {
        r.fail();
        return false;
    }
    out.skills.clear();
    out.skills.resize(count);
    for (auto& s : out.skills)
    {
        s.skillId = r.readU16();
        s.rank = r.readU8();
    }
    if (r.ok() && !validSkillList(out.skills))
        r.fail();
    return r.ok();
}

void SkillLearnReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU16(skillId);
}

bool SkillLearnReq::read(PacketReader& r, SkillLearnReq& out)
{
    out.seq = r.readU32();
    out.skillId = r.readU16();
    return r.ok();
}

void SkillLearnRes::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU16(skillId);
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU8(rank);
    w.writeU16(unspent);
}

bool SkillLearnRes::read(PacketReader& r, SkillLearnRes& out)
{
    out.seq = r.readU32();
    out.skillId = r.readU16();
    out.result = readResult<SkillLearnResult>(r, kSkillLearnResultMax);
    out.rank = r.readU8();
    out.unspent = r.readU16();
    if (out.rank > kMaxSkillRankWire)
        r.fail();
    return r.ok();
}

void SkillBar::write(PacketWriter& w) const
{
    writeSlots(w, slots);
}

bool SkillBar::read(PacketReader& r, SkillBar& out)
{
    readSlots(r, out.slots);
    return r.ok();
}

void SkillBarSetReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    writeSlots(w, slots);
}

bool SkillBarSetReq::read(PacketReader& r, SkillBarSetReq& out)
{
    out.seq = r.readU32();
    readSlots(r, out.slots);
    return r.ok();
}

void SkillBarSetRes::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU8(static_cast<std::uint8_t>(result));
    writeSlots(w, slots);
}

bool SkillBarSetRes::read(PacketReader& r, SkillBarSetRes& out)
{
    out.seq = r.readU32();
    out.result = readResult<SkillBarResult>(r, kSkillBarResultMax);
    readSlots(r, out.slots);
    return r.ok();
}

void SkillCooldowns::write(PacketWriter& w) const
{
    if (entries.empty() || entries.size() > kMaxSkillCooldownEntries ||
        !std::all_of(entries.begin(), entries.end(), validCooldown))
    {
        w.fail();
        return;
    }
    w.writeU8(static_cast<std::uint8_t>(entries.size()));
    for (const auto& e : entries)
    {
        w.writeU16(e.skillId);
        w.writeU32(e.remainingMs);
        w.writeU32(e.totalMs);
    }
}

bool SkillCooldowns::read(PacketReader& r, SkillCooldowns& out)
{
    const std::uint8_t count = r.readU8();
    if (!r.ok() || count == 0 || count > kMaxSkillCooldownEntries)
    {
        r.fail();
        return false;
    }
    out.entries.clear();
    out.entries.resize(count);
    for (auto& e : out.entries)
    {
        e.skillId = r.readU16();
        e.remainingMs = r.readU32();
        e.totalMs = r.readU32();
        if (r.ok() && !validCooldown(e))
            r.fail();
    }
    return r.ok();
}

void EffectEntry::write(PacketWriter& w) const
{
    if (kind < kEffectKindMin || kind > kEffectKindMax || stacks == 0 || remainingMs > durationMs)
    {
        w.fail();
        return;
    }
    w.writeU16(skillId);
    w.writeU8(kind);
    w.writeU8(stacks);
    w.writeU32(sourceId);
    w.writeU32(durationMs);
    w.writeU32(remainingMs);
    w.writeU32(amount);
}

bool EffectEntry::read(PacketReader& r, EffectEntry& out)
{
    out.skillId = r.readU16();
    out.kind = r.readU8();
    out.stacks = r.readU8();
    out.sourceId = r.readU32();
    out.durationMs = r.readU32();
    out.remainingMs = r.readU32();
    out.amount = r.readU32();
    if (out.kind < kEffectKindMin || out.kind > kEffectKindMax || out.stacks == 0 || out.remainingMs > out.durationMs)
        r.fail();
    return r.ok();
}

void EffectList::write(PacketWriter& w) const
{
    if (effects.size() > kMaxEffectEntries)
    {
        w.fail();
        return;
    }
    w.writeU32(entityId);
    w.writeU8(static_cast<std::uint8_t>(effects.size()));
    for (const auto& e : effects)
        e.write(w);
}

bool EffectList::read(PacketReader& r, EffectList& out)
{
    out.entityId = r.readU32();
    const std::uint8_t count = r.readU8();
    if (!r.ok() || count > kMaxEffectEntries)
    {
        r.fail();
        return false;
    }
    out.effects.clear();
    out.effects.resize(count);
    for (auto& e : out.effects)
        if (!EffectEntry::read(r, e))
            return false;
    return r.ok();
}

void EffectApply::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    effect.write(w);
}

bool EffectApply::read(PacketReader& r, EffectApply& out)
{
    out.entityId = r.readU32();
    return EffectEntry::read(r, out.effect) && r.ok();
}

void EffectRemove::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU16(skillId);
    w.writeU8(kind);
    w.writeU8(static_cast<std::uint8_t>(reason));
}

bool EffectRemove::read(PacketReader& r, EffectRemove& out)
{
    out.entityId = r.readU32();
    out.skillId = r.readU16();
    out.kind = r.readU8();
    const std::uint8_t reason = r.readU8();
    if (out.kind < kEffectKindMin || out.kind > kEffectKindMax || reason < kEffectRemoveReasonMin ||
        reason > kEffectRemoveReasonMax)
        r.fail();
    out.reason = static_cast<EffectRemoveReason>(reason);
    return r.ok();
}

void SkillLeap::write(PacketWriter& w) const
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(yaw))
    {
        w.fail();
        return;
    }
    w.writeU32(entityId);
    w.writeF32(x);
    w.writeF32(y);
    w.writeF32(z);
    w.writeF32(yaw);
    w.writeU32(targetId);
}

bool SkillLeap::read(PacketReader& r, SkillLeap& out)
{
    out.entityId = r.readU32();
    out.x = r.readF32();
    out.y = r.readF32();
    out.z = r.readF32();
    out.yaw = r.readF32();
    out.targetId = r.readU32();
    if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z) || !std::isfinite(out.yaw))
        r.fail();
    return r.ok();
}

void SkillUsed::write(PacketWriter& w) const
{
    w.writeU32(casterId);
    w.writeU16(skillId);
    w.writeU32(targetId);
    w.writeU8(rank);
}

bool SkillUsed::read(PacketReader& r, SkillUsed& out)
{
    out.casterId = r.readU32();
    out.skillId = r.readU16();
    out.targetId = r.readU32();
    out.rank = r.readU8();
    if (out.rank < 1 || out.rank > kMaxSkillRankWire)
        r.fail();
    return r.ok();
}

} // namespace vo::net

// ---- Zones (docs/PROTOCOL_ZONES.md §3.2) ----

namespace vo::net {

void ZoneChangeBegin::write(PacketWriter& w) const
{
    w.writeU16(fromZone);
    w.writeU16(toZone);
}

bool ZoneChangeBegin::read(PacketReader& r, ZoneChangeBegin& out)
{
    out.fromZone = r.readU16();
    out.toZone = r.readU16();
    return r.ok();
}

void ZoneChange::write(PacketWriter& w) const
{
    const EnterWorldRes& e = enter;
    if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z) || !std::isfinite(e.yaw) ||
        !std::isfinite(e.walkSpeed) || !std::isfinite(e.runSpeed))
    {
        w.fail();
        return;
    }
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU16(fromZone);
    w.writeU32(e.entityId);
    w.writeU16(e.zoneId);
    w.writeF32(e.x);
    w.writeF32(e.y);
    w.writeF32(e.z);
    w.writeF32(e.yaw);
    w.writeF32(e.walkSpeed);
    w.writeF32(e.runSpeed);
    w.writeU32(e.serverTimeMs);
    w.writeU16(e.level);
    w.writeU32(e.xp);
    w.writeU32(e.xpToNext);
    w.writeU32(e.hp);
    w.writeU32(e.maxHp);
    w.writeU32(e.mp);
    w.writeU32(e.maxMp);
    w.writeU16(e.attack);
    w.writeU16(e.defense);
    w.writeBool(e.isDead);
}

bool ZoneChange::read(PacketReader& r, ZoneChange& out)
{
    out = {};
    const std::uint8_t result = r.readU8();
    if (result > kZoneChangeResultMax)
        r.fail();
    out.result = static_cast<ZoneChangeResult>(result);
    out.fromZone = r.readU16();
    EnterWorldRes& e = out.enter;
    e.result = EnterWorldResult::Ok;
    e.entityId = r.readU32();
    e.zoneId = r.readU16();
    e.x = r.readF32();
    e.y = r.readF32();
    e.z = r.readF32();
    e.yaw = r.readF32();
    e.walkSpeed = r.readF32();
    e.runSpeed = r.readF32();
    e.serverTimeMs = r.readU32();
    e.level = r.readU16();
    e.xp = r.readU32();
    e.xpToNext = r.readU32();
    e.hp = r.readU32();
    e.maxHp = r.readU32();
    e.mp = r.readU32();
    e.maxMp = r.readU32();
    e.attack = r.readU16();
    e.defense = r.readU16();
    e.isDead = r.readBool();
    if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z) || !std::isfinite(e.yaw) ||
        !std::isfinite(e.walkSpeed) || !std::isfinite(e.runSpeed))
        r.fail();
    return r.ok();
}

void ZoneLoadedReq::write(PacketWriter& w) const
{
    w.writeU16(zoneId);
}

bool ZoneLoadedReq::read(PacketReader& r, ZoneLoadedReq& out)
{
    out.zoneId = r.readU16();
    return r.ok();
}

void ZoneGateRefused::write(PacketWriter& w) const
{
    w.writeU16(toZone);
    w.writeU8(static_cast<std::uint8_t>(reason));
}

bool ZoneGateRefused::read(PacketReader& r, ZoneGateRefused& out)
{
    out.toZone = r.readU16();
    const std::uint8_t reason = r.readU8();
    if (reason < kGateRefusalMin || reason > kGateRefusalMax)
        r.fail();
    out.reason = static_cast<GateRefusal>(reason);
    return r.ok();
}

// ---- Merchants and consumables (docs/PROTOCOL_SHOP.md §3) ----

namespace {

bool readShopResult(PacketReader& r, ShopResult& out)
{
    const std::uint8_t v = r.readU8();
    if (v > kShopResultMax)
        r.fail();
    out = static_cast<ShopResult>(v);
    return r.ok();
}

} // namespace

void ShopOpenReq::write(PacketWriter& w) const
{
    w.writeU32(npcId);
}

bool ShopOpenReq::read(PacketReader& r, ShopOpenReq& out)
{
    out.npcId = r.readU32();
    return r.ok();
}

void ShopOpenRes::write(PacketWriter& w) const
{
    if (entries.size() > kMaxShopEntries || (result != ShopResult::Ok && !entries.empty()) ||
        !std::all_of(entries.begin(), entries.end(), [](const ShopEntry& e) { return e.templateId != 0 && e.price != 0; }))
    {
        w.fail();
        return;
    }
    w.writeU32(npcId);
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU8(static_cast<std::uint8_t>(entries.size()));
    for (const auto& e : entries)
    {
        w.writeU16(e.templateId);
        w.writeU32(e.price);
    }
}

bool ShopOpenRes::read(PacketReader& r, ShopOpenRes& out)
{
    out.npcId = r.readU32();
    if (!readShopResult(r, out.result))
        return false;
    const std::uint8_t count = r.readU8();
    if (!r.ok() || count > kMaxShopEntries || (out.result != ShopResult::Ok && count != 0))
    {
        r.fail();
        return false;
    }
    out.entries.clear();
    out.entries.resize(count);
    for (auto& e : out.entries)
    {
        e.templateId = r.readU16();
        e.price = r.readU32();
        if (r.ok() && (e.templateId == 0 || e.price == 0))
            r.fail();
    }
    return r.ok();
}

void ShopCloseReq::write(PacketWriter& w) const
{
    w.writeU32(npcId);
}

bool ShopCloseReq::read(PacketReader& r, ShopCloseReq& out)
{
    out.npcId = r.readU32();
    return r.ok();
}

void ShopClosed::write(PacketWriter& w) const
{
    w.writeU32(npcId);
    w.writeU8(static_cast<std::uint8_t>(reason));
}

bool ShopClosed::read(PacketReader& r, ShopClosed& out)
{
    out.npcId = r.readU32();
    const std::uint8_t reason = r.readU8();
    if (reason < kShopClosedReasonMin || reason > kShopClosedReasonMax)
        r.fail();
    out.reason = static_cast<ShopClosedReason>(reason);
    return r.ok();
}

void ShopBuyReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU32(npcId);
    w.writeU16(templateId);
    w.writeU16(quantity);
}

bool ShopBuyReq::read(PacketReader& r, ShopBuyReq& out)
{
    out.seq = r.readU32();
    out.npcId = r.readU32();
    out.templateId = r.readU16();
    out.quantity = r.readU16();
    return r.ok();
}

void ShopBuyRes::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU16(templateId);
    w.writeU16(quantity);
    w.writeU32(cost);
}

bool ShopBuyRes::read(PacketReader& r, ShopBuyRes& out)
{
    out.seq = r.readU32();
    if (!readShopResult(r, out.result))
        return false;
    out.templateId = r.readU16();
    out.quantity = r.readU16();
    out.cost = r.readU32();
    return r.ok();
}

void ShopSellReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU32(npcId);
    w.writeU8(bagSlot);
    w.writeU16(quantity);
}

bool ShopSellReq::read(PacketReader& r, ShopSellReq& out)
{
    out.seq = r.readU32();
    out.npcId = r.readU32();
    out.bagSlot = r.readU8();
    out.quantity = r.readU16();
    return r.ok();
}

void ShopSellRes::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU16(templateId);
    w.writeU16(quantity);
    w.writeU32(gained);
}

bool ShopSellRes::read(PacketReader& r, ShopSellRes& out)
{
    out.seq = r.readU32();
    if (!readShopResult(r, out.result))
        return false;
    out.templateId = r.readU16();
    out.quantity = r.readU16();
    out.gained = r.readU32();
    return r.ok();
}

void ItemUseReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU16(templateId);
    w.writeU8(bagSlot);
}

bool ItemUseReq::read(PacketReader& r, ItemUseReq& out)
{
    out.seq = r.readU32();
    out.templateId = r.readU16();
    out.bagSlot = r.readU8();
    return r.ok();
}

void ItemUseRes::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU16(templateId);
}

bool ItemUseRes::read(PacketReader& r, ItemUseRes& out)
{
    out.seq = r.readU32();
    const std::uint8_t v = r.readU8();
    if (v > kItemUseResultMax)
        r.fail();
    out.result = static_cast<ItemUseResult>(v);
    out.templateId = r.readU16();
    return r.ok();
}

void ItemCooldown::write(PacketWriter& w) const
{
    if (remainingMs == 0 || remainingMs > totalMs)
    {
        w.fail();
        return;
    }
    w.writeU32(remainingMs);
    w.writeU32(totalMs);
}

bool ItemCooldown::read(PacketReader& r, ItemCooldown& out)
{
    out.remainingMs = r.readU32();
    out.totalMs = r.readU32();
    if (r.ok() && (out.remainingMs == 0 || out.remainingMs > out.totalMs))
        r.fail();
    return r.ok();
}

void ItemUsed::write(PacketWriter& w) const
{
    w.writeU32(entityId);
    w.writeU16(templateId);
    w.writeU32(hpRestored);
    w.writeU32(mpRestored);
}

bool ItemUsed::read(PacketReader& r, ItemUsed& out)
{
    out.entityId = r.readU32();
    out.templateId = r.readU16();
    out.hpRestored = r.readU32();
    out.mpRestored = r.readU32();
    return r.ok();
}

// ---- Item upgrades (docs/PROTOCOL_UPGRADE.md §4) ----

void UpgradeReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU32(npcId);
    w.writeU8(bagSlot);
    w.writeU64(itemId);
    w.writeU8(fromLevel);
    w.writeU8(stoneSlot);
}

bool UpgradeReq::read(PacketReader& r, UpgradeReq& out)
{
    out.seq = r.readU32();
    out.npcId = r.readU32();
    out.bagSlot = r.readU8();
    out.itemId = r.readU64();
    out.fromLevel = r.readU8();
    out.stoneSlot = r.readU8();
    return r.ok();
}

void UpgradeRes::write(PacketWriter& w) const
{
    if (static_cast<std::uint8_t>(result) > kUpgradeResultMax || level > kMaxUpgradeLevel)
    {
        w.fail();
        return;
    }
    w.writeU32(seq);
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU8(bagSlot);
    w.writeU16(templateId);
    w.writeU8(level);
    w.writeU16(stoneTemplateId);
    w.writeU32(fee);
}

bool UpgradeRes::read(PacketReader& r, UpgradeRes& out)
{
    out.seq = r.readU32();
    const std::uint8_t v = r.readU8();
    if (v > kUpgradeResultMax)
        r.fail();
    out.result = static_cast<UpgradeResult>(v);
    out.bagSlot = r.readU8();
    out.templateId = r.readU16();
    out.level = r.readU8();
    if (out.level > kMaxUpgradeLevel)
        r.fail();
    out.stoneTemplateId = r.readU16();
    out.fee = r.readU32();
    return r.ok();
}

// ---- Quests (docs/PROTOCOL_QUESTS.md §4) ----

namespace {

void writeQuestEntry(PacketWriter& w, const QuestEntry& e)
{
    w.writeU16(e.questId);
    w.writeU8(static_cast<std::uint8_t>(e.state));
    for (const std::uint16_t p : e.progress)
        w.writeU16(p);
    w.writeU32(e.cooldownS);
}

bool readQuestEntry(PacketReader& r, QuestEntry& e)
{
    e.questId = r.readU16();
    const std::uint8_t state = r.readU8();
    if (state > kQuestStateMax)
        r.fail();
    e.state = static_cast<QuestState>(state);
    for (std::uint16_t& p : e.progress)
        p = r.readU16();
    e.cooldownS = r.readU32();
    if (r.ok() && e.questId == 0)
        r.fail();
    return r.ok();
}

} // namespace

void QuestList::write(PacketWriter& w) const
{
    if (quests.size() > kMaxQuestListEntries ||
        !std::all_of(quests.begin(), quests.end(),
                     [](const QuestEntry& e) {
                         return e.questId != 0 && (e.state == QuestState::Active || e.state == QuestState::Done);
                     }))
    {
        w.fail();
        return;
    }
    w.writeU8(static_cast<std::uint8_t>(quests.size()));
    for (const auto& e : quests)
        writeQuestEntry(w, e);
}

bool QuestList::read(PacketReader& r, QuestList& out)
{
    const std::uint8_t count = r.readU8();
    if (!r.ok() || count > kMaxQuestListEntries)
    {
        r.fail();
        return false;
    }
    out.quests.clear();
    out.quests.resize(count);
    for (auto& e : out.quests)
    {
        if (!readQuestEntry(r, e))
            return false;
        if (e.state == QuestState::None)
        {
            r.fail();
            return false;
        }
    }
    return r.ok();
}

void QuestUpdate::write(PacketWriter& w) const
{
    if (quest.questId == 0 || static_cast<std::uint8_t>(quest.state) > kQuestStateMax)
    {
        w.fail();
        return;
    }
    writeQuestEntry(w, quest);
}

bool QuestUpdate::read(PacketReader& r, QuestUpdate& out)
{
    return readQuestEntry(r, out.quest);
}

void QuestAcceptReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU32(npcId);
    w.writeU16(questId);
}

bool QuestAcceptReq::read(PacketReader& r, QuestAcceptReq& out)
{
    out.seq = r.readU32();
    out.npcId = r.readU32();
    out.questId = r.readU16();
    return r.ok();
}

void QuestAbandonReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU16(questId);
}

bool QuestAbandonReq::read(PacketReader& r, QuestAbandonReq& out)
{
    out.seq = r.readU32();
    out.questId = r.readU16();
    return r.ok();
}

void QuestHandInReq::write(PacketWriter& w) const
{
    w.writeU32(seq);
    w.writeU32(npcId);
    w.writeU16(questId);
    w.writeU8(choice);
}

bool QuestHandInReq::read(PacketReader& r, QuestHandInReq& out)
{
    out.seq = r.readU32();
    out.npcId = r.readU32();
    out.questId = r.readU16();
    out.choice = r.readU8();
    return r.ok();
}

void QuestRes::write(PacketWriter& w) const
{
    const auto a = static_cast<std::uint8_t>(action);
    if (a < kQuestActionMin || a > kQuestActionMax || static_cast<std::uint8_t>(result) > kQuestResultMax)
    {
        w.fail();
        return;
    }
    w.writeU32(seq);
    w.writeU8(a);
    w.writeU16(questId);
    w.writeU8(static_cast<std::uint8_t>(result));
    w.writeU32(xp);
    w.writeU32(coins);
}

bool QuestRes::read(PacketReader& r, QuestRes& out)
{
    out.seq = r.readU32();
    const std::uint8_t a = r.readU8();
    if (a < kQuestActionMin || a > kQuestActionMax)
        r.fail();
    out.action = static_cast<QuestAction>(a);
    out.questId = r.readU16();
    const std::uint8_t v = r.readU8();
    if (v > kQuestResultMax)
        r.fail();
    out.result = static_cast<QuestResult>(v);
    out.xp = r.readU32();
    out.coins = r.readU32();
    return r.ok();
}

} // namespace vo::net
