#include "shared/net/Messages.h"

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

} // namespace vo::net
