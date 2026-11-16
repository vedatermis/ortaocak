#include "shared/net/Messages.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vn = vo::net;

namespace {

// Encodes, decodes and compares; then checks that every strict prefix and one trailing byte are
// rejected (§1: running out of bytes / trailing bytes are protocol errors).
template <class T>
void checkRoundTrip(const T& msg)
{
    const auto encoded = vn::encodeMessage(msg);
    REQUIRE(encoded.has_value());
    T decoded;
    REQUIRE(vn::decodeMessage(*encoded, decoded));
    CHECK(decoded == msg);

    for (std::size_t len = 0; len < encoded->size(); ++len)
    {
        T partial;
        INFO("prefix length " << len << " of " << encoded->size());
        CHECK_FALSE(vn::decodeMessage(std::span<const std::uint8_t>(encoded->data(), len), partial));
    }
    auto longer = *encoded;
    longer.push_back(0);
    T extra;
    CHECK_FALSE(vn::decodeMessage(longer, extra));
}

vn::CharSummary summary(std::uint32_t id, std::string name)
{
    vn::CharSummary s;
    s.charId = id;
    s.name = std::move(name);
    s.race = 2;
    s.charClass = 4;
    s.face = 3;
    s.hair = 1;
    s.level = 12;
    s.zoneId = 1;
    return s;
}

} // namespace

TEST_CASE("Every message round-trips and rejects truncation and trailing bytes", "[net][messages]")
{
    vn::Hello hello;
    hello.clientBuild = 1234;
    for (std::size_t i = 0; i < hello.clientKxPk.size(); ++i)
        hello.clientKxPk[i] = static_cast<std::uint8_t>(i);
    checkRoundTrip(hello);

    vn::HelloAck ack;
    ack.serverKxPk.fill(0x11);
    ack.signature.fill(0x22);
    checkRoundTrip(ack);
    vn::HelloAck rejected;
    rejected.result = vn::HelloResult::ServerFull;
    checkRoundTrip(rejected);

    checkRoundTrip(vn::Ping{0xFFFFFFFF});
    checkRoundTrip(vn::Pong{7, 9});
    checkRoundTrip(vn::Error{vn::ErrorCode::BadState, "nope"});
    checkRoundTrip(vn::Error{vn::ErrorCode::Flood, ""});
    checkRoundTrip(vn::Logout{});
    checkRoundTrip(vn::LoginReq{"Vedat_01", "şifre123"});
    checkRoundTrip(vn::LoginRes{vn::LoginResult::Ok, 42});
    checkRoundTrip(vn::LoginRes{vn::LoginResult::TooManyAttempts, 0});

    checkRoundTrip(vn::CharListReq{});
    checkRoundTrip(vn::CharListRes{});
    checkRoundTrip(vn::CharListRes{{summary(1, "Ali"), summary(2, "Çağrı"), summary(3, "İnci")}});

    checkRoundTrip(vn::CharCreateReq{"Ömer", 1, 2, 3, 0});
    checkRoundTrip(vn::CharCreateRes{vn::CharCreateResult::Ok, summary(5, "Şule")});
    checkRoundTrip(vn::CharCreateRes{vn::CharCreateResult::NameTaken, {}});
    checkRoundTrip(vn::CharDeleteReq{77});
    checkRoundTrip(vn::CharDeleteRes{vn::CharDeleteResult::NotFound, 77});

    checkRoundTrip(vn::EnterWorldReq{5});
    vn::EnterWorldRes enter;
    enter.entityId = 9;
    enter.zoneId = 1;
    enter.x = 256.0f;
    enter.y = 1.5f;
    enter.z = 256.0f;
    enter.yaw = 3.14f;
    enter.walkSpeed = 1.4f;
    enter.runSpeed = 5.5f;
    enter.serverTimeMs = 123456;
    checkRoundTrip(enter);
    checkRoundTrip(vn::EnterWorldRes{vn::EnterWorldResult::NotFound});
    checkRoundTrip(vn::LeaveWorldReq{});
    checkRoundTrip(vn::LeaveWorldRes{0});

    checkRoundTrip(vn::MoveReq{1, vn::kMoveFlagMoving | vn::kMoveFlagRunning, 1.0f, 2.0f, 3.0f, 0.5f});
    vn::EntitySpawn spawn;
    spawn.entityId = 3;
    spawn.name = "Kargan";
    spawn.race = 2;
    spawn.charClass = 1;
    spawn.level = 3;
    spawn.x = 10.0f;
    spawn.flags = 1;
    checkRoundTrip(spawn);
    checkRoundTrip(vn::EntityDespawn{3});
    checkRoundTrip(vn::EntityMove{3, 1, 1.0f, 2.0f, 3.0f, 4.0f, 99});
    checkRoundTrip(vn::PositionCorrect{8, 1.0f, 2.0f, 3.0f, 4.0f});
}

TEST_CASE("HELLO wire layout matches the spec", "[net][messages]")
{
    vn::Hello hello;
    hello.clientBuild = 0x01020304;
    hello.clientKxPk.fill(0xAA);
    const auto bytes = *vn::encodeMessage(hello);
    REQUIRE(bytes.size() == 4 + 2 + 4 + 32);
    CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "VONL"); // magic 0x4C4E4F56 LE
    CHECK(bytes[4] == 4);
    CHECK(bytes[5] == 0); // protocol version 4 (loot)
    CHECK(bytes[6] == 0x04);
    CHECK(bytes[9] == 0x01);
    CHECK(bytes[10] == 0xAA);

    const auto ack = *vn::encodeMessage(vn::HelloAck{});
    CHECK(ack.size() == 1 + 2 + 32 + 64);
}

TEST_CASE("Optional parts are only on the wire for result Ok", "[net][messages]")
{
    CHECK(vn::encodeMessage(vn::CharCreateRes{vn::CharCreateResult::NoFreeSlot, summary(1, "Ali")})->size() == 1);
    CHECK(vn::encodeMessage(vn::EnterWorldRes{vn::EnterWorldResult::ServerError})->size() == 1);
    CHECK(vn::encodeMessage(vn::EnterWorldRes{})->size() == 1 + 4 + 2 + 6 * 4 + 4 + 2 + 6 * 4 + 2 + 2 + 1);
}

TEST_CASE("Message readers reject values the wire format forbids", "[net][messages]")
{
    SECTION("CHAR_LIST_RES with more than 3 entries")
    {
        vn::PacketWriter w;
        w.writeU8(4);
        for (std::uint32_t i = 0; i < 4; ++i)
            summary(i, "Ali").write(w);
        vn::CharListRes res;
        CHECK_FALSE(vn::decodeMessage(w.data(), res));

        vn::CharListRes tooMany{{summary(1, "A1a"), summary(2, "B2b"), summary(3, "C3c"), summary(4, "D4d")}};
        CHECK_FALSE(vn::encodeMessage(tooMany).has_value());
    }
    SECTION("unknown result codes")
    {
        const std::vector<std::uint8_t> loginRes = {6, 0, 0, 0, 0};
        vn::LoginRes l;
        CHECK_FALSE(vn::decodeMessage(loginRes, l));
        const std::vector<std::uint8_t> helloAck(1 + 2 + 32 + 64, 4);
        vn::HelloAck a;
        CHECK_FALSE(vn::decodeMessage(helloAck, a));
        const std::vector<std::uint8_t> createRes = {7};
        vn::CharCreateRes c;
        CHECK_FALSE(vn::decodeMessage(createRes, c));
        const std::vector<std::uint8_t> deleteRes = {3, 0, 0, 0, 0};
        vn::CharDeleteRes d;
        CHECK_FALSE(vn::decodeMessage(deleteRes, d));
        const std::vector<std::uint8_t> enterRes = {3};
        vn::EnterWorldRes e;
        CHECK_FALSE(vn::decodeMessage(enterRes, e));
    }
    SECTION("race/class/appearance are carried raw so the server can answer with a result code")
    {
        vn::CharCreateReq req{"Ali", 99, 0, 200, 255};
        vn::CharCreateReq out;
        REQUIRE(vn::decodeMessage(*vn::encodeMessage(req), out));
        CHECK(out == req);
    }
    SECTION("invalid UTF-8 in a name")
    {
        const std::vector<std::uint8_t> bad = {2, 0xC0, 0x80, 1, 1, 0, 0};
        vn::CharCreateReq out;
        CHECK_FALSE(vn::decodeMessage(bad, out));
    }
}

TEST_CASE("Opcode names", "[net][messages]")
{
    CHECK(vn::opcodeName(vn::Opcode::CharCreateReq) == "CHAR_CREATE_REQ");
    CHECK(vn::isKnownOpcode(0x0204));
    CHECK_FALSE(vn::isKnownOpcode(0x0003));
    CHECK(vn::errorCodeName(vn::ErrorCode::KickedDuplicateLogin) == "KickedDuplicateLogin");
}

// ---- Items (docs/PROTOCOL_ITEMS.md, protocol v2) ----

TEST_CASE("Item messages round-trip and reject truncation and trailing bytes", "[net][messages][items]")
{
    vn::CharSummary s = summary(5, "Silahlı");
    s.mainHandTemplate = 1;
    s.offHandTemplate = 65535;
    checkRoundTrip(vn::CharListRes{{s}});
    checkRoundTrip(vn::CharCreateRes{vn::CharCreateResult::Ok, s});

    vn::EntitySpawn spawn;
    spawn.entityId = 9;
    spawn.name = "Ok";
    spawn.mainHandTemplate = 4;
    spawn.offHandTemplate = 0;
    checkRoundTrip(spawn);

    vn::Inventory inv;
    checkRoundTrip(inv); // empty: bag_size + count 0
    inv.items.push_back({0, 0, 1, 1, 1});
    inv.items.push_back({0, 1, 2, 2, 1});
    inv.items.push_back({1, 27, 0xFFFF'FFFF'FFFF'FFFFull, 6, 1});
    checkRoundTrip(inv);
    vn::Inventory full;
    for (std::uint8_t i = 0; i < vn::kMaxInventoryEntries; ++i)
        full.items.push_back({static_cast<std::uint8_t>(i < 8 ? 0 : 1), static_cast<std::uint8_t>(i < 8 ? i : i - 8),
                              100u + i, 3, 1});
    checkRoundTrip(full);

    checkRoundTrip(vn::ItemMoveReq{77, 1, 0, 0, 0});
    checkRoundTrip(vn::ItemMoveReq{0xFFFFFFFF, 255, 255, 255, 255}); // raw: the server answers BadSlot
    for (std::uint8_t r = 0; r <= vn::kItemMoveResultMax; ++r)
        checkRoundTrip(vn::ItemMoveRes{12, static_cast<vn::ItemMoveResult>(r)});
    checkRoundTrip(vn::ItemUpdate{{{1, 0, 0, 0, 0}}});
    vn::ItemUpdate eight;
    for (std::uint8_t i = 0; i < vn::kMaxItemUpdateEntries; ++i)
        eight.items.push_back({1, i, i, i, 1});
    checkRoundTrip(eight);
    checkRoundTrip(vn::EntityEquip{3, 6, 0});
}

TEST_CASE("Item message wire layout matches the contract", "[net][messages][items]")
{
    const auto entry = *vn::encodeMessage(vn::Inventory{28, {{1, 4, 0x0102030405060708ull, 0x0A0B, 1}}});
    REQUIRE(entry.size() == 2 + 14);
    CHECK(entry[0] == 28);
    CHECK(entry[1] == 1);
    CHECK(entry[2] == 1);    // container
    CHECK(entry[3] == 4);    // slot
    CHECK(entry[4] == 0x08); // item_id u64 LE
    CHECK(entry[11] == 0x01);
    CHECK(entry[12] == 0x0B); // template_id u16 LE
    CHECK(entry[14] == 1);    // quantity
    CHECK(vn::encodeMessage(vn::ItemMoveReq{})->size() == 8);
    CHECK(vn::encodeMessage(vn::ItemMoveRes{})->size() == 5);
    CHECK(vn::encodeMessage(vn::EntityEquip{})->size() == 8);
    vn::PacketWriter w;
    summary(1, "Ali").write(w);
    CHECK(w.data().size() == 4 + 1 + 3 + 4 + 2 + 2 + 2 + 2);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::Inventory) == 0x0400);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::EntityEquip) == 0x0404);
    CHECK(vn::kProtocolVersion >= 2); // 3 since combat (PROTOCOL_COMBAT.md)
}

TEST_CASE("Item message readers reject counts and results the wire format forbids", "[net][messages][items]")
{
    vn::PacketWriter w;
    w.writeU8(28);
    w.writeU8(37); // > 36 entries
    for (int i = 0; i < 37; ++i)
        vn::ItemEntry{}.write(w);
    vn::Inventory inv;
    CHECK_FALSE(vn::decodeMessage(w.data(), inv));
    vn::Inventory tooMany;
    tooMany.items.resize(37);
    CHECK_FALSE(vn::encodeMessage(tooMany).has_value());

    const std::vector<std::uint8_t> noEntries = {0};
    vn::ItemUpdate u;
    CHECK_FALSE(vn::decodeMessage(noEntries, u));
    CHECK_FALSE(vn::encodeMessage(vn::ItemUpdate{}).has_value());
    vn::PacketWriter nine;
    nine.writeU8(9);
    for (int i = 0; i < 9; ++i)
        vn::ItemEntry{}.write(nine);
    CHECK_FALSE(vn::decodeMessage(nine.data(), u));
    vn::ItemUpdate nineEntries;
    nineEntries.items.resize(9);
    CHECK_FALSE(vn::encodeMessage(nineEntries).has_value());

    const std::vector<std::uint8_t> badResult = {1, 0, 0, 0, 9};
    vn::ItemMoveRes r;
    CHECK_FALSE(vn::decodeMessage(badResult, r));
    CHECK(vn::itemMoveResultName(vn::ItemMoveResult::SlotBlocked) == "SlotBlocked");
    CHECK(vn::opcodeName(vn::Opcode::ItemMoveReq) == "ITEM_MOVE_REQ");
    CHECK(vn::isKnownOpcode(0x0403));
    CHECK_FALSE(vn::isKnownOpcode(0x0405));
}

namespace {

template <class T>
void fuzzDecode(const std::vector<std::uint8_t>& bytes, int& decoded)
{
    T out;
    if (!vn::decodeMessage(bytes, out))
        return;
    ++decoded;
    const auto again = vn::encodeMessage(out);
    REQUIRE(again.has_value());
    CHECK(*again == bytes);
}

} // namespace

// Random bytes into every decoder with lists or new fields: never crashes or reads out of bounds,
// and whatever decodes re-encodes to exactly the same bytes (one encoding per value).
TEST_CASE("Fuzz: decoders survive random payloads and decode canonically", "[net][messages][fuzz]")
{
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto rnd = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    int decoded = 0;
    for (int i = 0; i < 20000; ++i)
    {
        std::vector<std::uint8_t> bytes(rnd() % 64);
        for (auto& b : bytes)
            b = static_cast<std::uint8_t>(rnd());
        // Bias towards small counts so the list decoders get past their headers.
        if (!bytes.empty() && (i % 3) == 0)
            bytes[0] = static_cast<std::uint8_t>(rnd() % 4);
        if (bytes.size() > 1 && (i % 3) == 1)
            bytes[1] = static_cast<std::uint8_t>(rnd() % 4);
        fuzzDecode<vn::Inventory>(bytes, decoded);
        fuzzDecode<vn::ItemUpdate>(bytes, decoded);
        fuzzDecode<vn::ItemMoveReq>(bytes, decoded);
        fuzzDecode<vn::ItemMoveRes>(bytes, decoded);
        fuzzDecode<vn::EntityEquip>(bytes, decoded);
        fuzzDecode<vn::EntitySpawn>(bytes, decoded);
        fuzzDecode<vn::CharListRes>(bytes, decoded);
        fuzzDecode<vn::CharCreateRes>(bytes, decoded);
        fuzzDecode<vn::EnterWorldRes>(bytes, decoded);
        fuzzDecode<vn::MoveReq>(bytes, decoded);
        // v3 combat
        fuzzDecode<vn::TargetReq>(bytes, decoded);
        fuzzDecode<vn::AttackReq>(bytes, decoded);
        fuzzDecode<vn::AttackStopReq>(bytes, decoded);
        fuzzDecode<vn::SkillReq>(bytes, decoded);
        fuzzDecode<vn::ActionFail>(bytes, decoded);
        fuzzDecode<vn::CombatEvent>(bytes, decoded);
        fuzzDecode<vn::CastStart>(bytes, decoded);
        fuzzDecode<vn::CastEnd>(bytes, decoded);
        fuzzDecode<vn::EntityStats>(bytes, decoded);
        fuzzDecode<vn::SelfStats>(bytes, decoded);
        fuzzDecode<vn::XpGain>(bytes, decoded);
        fuzzDecode<vn::LevelUp>(bytes, decoded);
        fuzzDecode<vn::EntityDeath>(bytes, decoded);
        fuzzDecode<vn::RespawnReq>(bytes, decoded);
        fuzzDecode<vn::RespawnRes>(bytes, decoded);
        fuzzDecode<vn::EntityState>(bytes, decoded);
        fuzzDecode<vn::StanceReq>(bytes, decoded);
        // v4 loot
        fuzzDecode<vn::LootOpenReq>(bytes, decoded);
        fuzzDecode<vn::LootOpenRes>(bytes, decoded);
        fuzzDecode<vn::LootContents>(bytes, decoded);
        fuzzDecode<vn::LootTakeReq>(bytes, decoded);
        fuzzDecode<vn::LootTakeRes>(bytes, decoded);
        fuzzDecode<vn::LootCloseReq>(bytes, decoded);
        fuzzDecode<vn::LootClosed>(bytes, decoded);
        fuzzDecode<vn::Coins>(bytes, decoded);
    }
    CHECK(decoded > 100); // exact-size messages and short lists do decode now and then
}

// ---- Combat (docs/PROTOCOL_COMBAT.md, protocol v3) ----

TEST_CASE("Combat messages round-trip and reject truncation and trailing bytes", "[net][messages][combat]")
{
    checkRoundTrip(vn::TargetReq{0});
    checkRoundTrip(vn::TargetReq{0xFFFFFFFF});
    checkRoundTrip(vn::AttackReq{77});
    checkRoundTrip(vn::SkillReq{3, 1234});
    checkRoundTrip(vn::SkillReq{0xFFFF, 0}); // raw: the server answers UnknownSkill
    for (std::uint8_t a = vn::kCombatActionMin; a <= vn::kCombatActionMax; ++a)
        for (std::uint8_t r = vn::kActionFailReasonMin; r <= vn::kActionFailReasonMax; ++r)
            checkRoundTrip(vn::ActionFail{static_cast<vn::CombatAction>(a), 4, static_cast<vn::ActionFailReason>(r)});
    checkRoundTrip(vn::CombatEvent{1, 2, 0, vn::kCombatKindDamage, vn::kCombatFlagCritical | vn::kCombatFlagKillingBlow,
                                   17, 0, 123456});
    checkRoundTrip(vn::CombatEvent{1, 1, 4, vn::kCombatKindHeal, 0, 24, 90, 7});
    checkRoundTrip(vn::CombatEvent{1, 2, 0, vn::kCombatKindMiss, 0, 0, 5, 7});
    checkRoundTrip(vn::CastStart{5, 3, 9, 600});
    checkRoundTrip(vn::CastEnd{5, 3, vn::kCastInterrupted});
    checkRoundTrip(vn::EntityStats{9, 50, 80, 2});
    checkRoundTrip(vn::SelfStats{45, 90, 17, 34, 18, 7});
    checkRoundTrip(vn::XpGain{25, 25, 80, 9});
    checkRoundTrip(vn::LevelUp{5, 10});
    checkRoundTrip(vn::EntityDeath{9, 5});
    for (std::uint8_t r = 0; r <= vn::kRespawnResultMax; ++r)
        checkRoundTrip(vn::RespawnRes{static_cast<vn::RespawnResult>(r), 238.0f, 20.0f, 271.0f, 2.35f, 45, 17});
    checkRoundTrip(vn::EntityState{9, vn::kEntityStateDead | vn::kEntityStateInCombat | vn::kEntityStateSitting});
    checkRoundTrip(vn::StanceReq{vn::kStanceStand});
    checkRoundTrip(vn::StanceReq{vn::kStanceSit});

    // The appended v3 fields of ENTER_WORLD_RES and ENTITY_SPAWN.
    vn::EnterWorldRes enter;
    enter.entityId = 4;
    enter.level = 7;
    enter.xp = 1234;
    enter.xpToNext = 1810;
    enter.hp = 1;
    enter.maxHp = 210;
    enter.mp = 0;
    enter.maxMp = 58;
    enter.attack = 30;
    enter.defense = 13;
    enter.isDead = true;
    checkRoundTrip(enter);
    vn::EntitySpawn monster;
    monster.entityId = 1000;
    monster.entityType = vn::kEntityTypeMonster;
    monster.name = "Kor Bekçisi";
    monster.level = 6;
    monster.hp = 450;
    monster.maxHp = 900;
    monster.templateId = 5;
    monster.state = vn::kEntityStateInCombat;
    checkRoundTrip(monster);
}

TEST_CASE("Combat message wire layout matches the contract", "[net][messages][combat]")
{
    CHECK(static_cast<std::uint16_t>(vn::Opcode::TargetReq) == 0x0300);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::StanceReq) == 0x0310);
    CHECK(vn::kProtocolVersion == 4);
    CHECK(vn::encodeMessage(vn::TargetReq{})->size() == 4);
    CHECK(vn::encodeMessage(vn::AttackReq{})->size() == 4);
    CHECK(vn::encodeMessage(vn::AttackStopReq{})->empty());
    CHECK(vn::encodeMessage(vn::SkillReq{})->size() == 6);
    CHECK(vn::encodeMessage(vn::ActionFail{})->size() == 4);
    CHECK(vn::encodeMessage(vn::CombatEvent{})->size() == 4 + 4 + 2 + 1 + 1 + 4 + 4 + 4);
    CHECK(vn::encodeMessage(vn::CastStart{})->size() == 12);
    CHECK(vn::encodeMessage(vn::CastEnd{})->size() == 7);
    CHECK(vn::encodeMessage(vn::EntityStats{})->size() == 14);
    CHECK(vn::encodeMessage(vn::SelfStats{})->size() == 20);
    CHECK(vn::encodeMessage(vn::XpGain{})->size() == 16);
    CHECK(vn::encodeMessage(vn::LevelUp{})->size() == 6);
    CHECK(vn::encodeMessage(vn::EntityDeath{})->size() == 8);
    CHECK(vn::encodeMessage(vn::RespawnReq{})->empty());
    CHECK(vn::encodeMessage(vn::RespawnRes{})->size() == 1 + 16 + 8);
    CHECK(vn::encodeMessage(vn::EntityState{})->size() == 5);
    CHECK(vn::encodeMessage(vn::StanceReq{})->size() == 1);
    // ACTION_FAIL: u8 action, u16 skill_id, u8 reason.
    const auto fail = *vn::encodeMessage(vn::ActionFail{vn::CombatAction::Skill, 0x0304, vn::ActionFailReason::Busy});
    CHECK(fail == std::vector<std::uint8_t>{2, 0x04, 0x03, 12});
    // ENTITY_SPAWN appends hp, max_hp, template_id, state after the v2 equipment fields.
    vn::EntitySpawn s;
    s.name = "A";
    s.offHandTemplate = 0x0201;
    s.hp = 0x0A0B0C0D;
    s.templateId = 0x0505;
    s.state = 4;
    const auto b = *vn::encodeMessage(s);
    REQUIRE(b.size() == 4 + 1 + 2 + 4 + 2 + 16 + 1 + 4 + 4 + 4 + 2 + 1);
    const std::size_t v2End = 4 + 1 + 2 + 4 + 2 + 16 + 1 + 4;
    CHECK(b[v2End - 2] == 0x01);
    CHECK(b[v2End] == 0x0D);
    CHECK(b[v2End + 8] == 0x05);
    CHECK(b.back() == 4);
    // ENTER_WORLD_RES appends level .. is_dead after server_time_ms.
    vn::EnterWorldRes e;
    e.serverTimeMs = 0x01020304;
    e.level = 0x0909;
    e.isDead = true;
    const auto eb = *vn::encodeMessage(e);
    const std::size_t v2EnterEnd = 1 + 4 + 2 + 6 * 4 + 4;
    CHECK(eb[v2EnterEnd - 1] == 0x01);
    CHECK(eb[v2EnterEnd] == 0x09);
    CHECK(eb.back() == 1);
    CHECK(vn::opcodeName(vn::Opcode::CombatEvent) == "COMBAT_EVENT");
    CHECK(vn::opcodeName(vn::Opcode::RespawnRes) == "RESPAWN_RES");
    CHECK(vn::isKnownOpcode(0x030F));
    CHECK_FALSE(vn::isKnownOpcode(0x0311));
    CHECK(vn::actionFailReasonName(vn::ActionFailReason::InSafeZone) == "InSafeZone");
}

TEST_CASE("Combat message readers reject values the contract does not define", "[net][messages][combat]")
{
    vn::ActionFail f;
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{0, 0, 0, 1}, f));  // action 0
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{5, 0, 0, 1}, f));  // action 5
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0}, f));  // reason 0
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 13}, f)); // reason 13
    vn::CombatEvent ev;
    auto bytes = *vn::encodeMessage(vn::CombatEvent{});
    bytes[10] = 0; // kind 0
    CHECK_FALSE(vn::decodeMessage(bytes, ev));
    bytes[10] = 4; // kind 4
    CHECK_FALSE(vn::decodeMessage(bytes, ev));
    bytes[10] = 1;
    bytes[11] = 0x04; // unknown flag bit
    CHECK_FALSE(vn::decodeMessage(bytes, ev));
    vn::CastEnd ce;
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0, 3, 0, 2}, ce));
    vn::EntityState st;
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0, 0x20}, st)); // v4 knows 0x01..0x10
    vn::StanceReq stance;
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{2}, stance));
    vn::RespawnRes rr;
    std::vector<std::uint8_t> res(25, 0);
    res[0] = 3;
    CHECK_FALSE(vn::decodeMessage(res, rr));
    vn::EntitySpawn sp;
    auto spawnBytes = *vn::encodeMessage(vn::EntitySpawn{});
    spawnBytes.back() = 0x20; // unknown state bit
    CHECK_FALSE(vn::decodeMessage(spawnBytes, sp));
    vn::EnterWorldRes er;
    auto enterBytes = *vn::encodeMessage(vn::EnterWorldRes{});
    enterBytes.back() = 2; // is_dead is a bool
    CHECK_FALSE(vn::decodeMessage(enterBytes, er));
    // A v2 ENTITY_SPAWN (without the v3 tail) no longer decodes.
    spawnBytes.resize(spawnBytes.size() - 11);
    CHECK_FALSE(vn::decodeMessage(spawnBytes, sp));
}

// ---- Loot (docs/PROTOCOL_LOOT.md, protocol v4) ----

TEST_CASE("Loot messages round-trip and reject truncation and trailing bytes", "[net][messages][loot]")
{
    checkRoundTrip(vn::LootOpenReq{0x01020304});
    checkRoundTrip(vn::LootOpenRes{7, vn::LootResult::NotYours});
    checkRoundTrip(vn::LootContents{9, 120, {{0, 11, 2}, {3, 15, 1}, {7, 9, 20}}});
    checkRoundTrip(vn::LootContents{9, 0, {}});
    checkRoundTrip(vn::LootTakeReq{5, 9, vn::kLootTakeAll});
    checkRoundTrip(vn::LootTakeReq{6, 9, 0x42}); // any `what` is carried: the server answers AlreadyTaken
    checkRoundTrip(vn::LootTakeRes{5, 9, vn::LootResult::BagFull});
    checkRoundTrip(vn::LootCloseReq{9});
    checkRoundTrip(vn::LootClosed{9, vn::LootClosedReason::YouDied});
    checkRoundTrip(vn::Coins{1'000'000'000, 25});
}

TEST_CASE("Loot message wire layout matches the contract", "[net][messages][loot]")
{
    const auto contents = *vn::encodeMessage(vn::LootContents{0x11223344, 0x0A0B0C0D, {{2, 0x0102, 3}}});
    REQUIRE(contents.size() == 4 + 4 + 1 + 5);
    CHECK(contents[0] == 0x44);
    CHECK(contents[3] == 0x11);
    CHECK(contents[4] == 0x0D);
    CHECK(contents[8] == 1);     // count
    CHECK(contents[9] == 2);     // index
    CHECK(contents[10] == 0x02); // template id LE
    CHECK(contents[11] == 0x01);
    CHECK(contents[12] == 3); // quantity LE
    CHECK(contents[13] == 0);
    const auto take = *vn::encodeMessage(vn::LootTakeReq{1, 2, vn::kLootTakeCoins});
    CHECK(take == std::vector<std::uint8_t>{1, 0, 0, 0, 2, 0, 0, 0, 0xFE});
    const auto res = *vn::encodeMessage(vn::LootTakeRes{1, 2, vn::LootResult::ServerError});
    CHECK(res == std::vector<std::uint8_t>{1, 0, 0, 0, 2, 0, 0, 0, 8});
    CHECK(*vn::encodeMessage(vn::LootOpenRes{3, vn::LootResult::Ok}) == std::vector<std::uint8_t>{3, 0, 0, 0, 0});
    CHECK(*vn::encodeMessage(vn::LootClosed{3, vn::LootClosedReason::Gone}) == std::vector<std::uint8_t>{3, 0, 0, 0, 1});
    CHECK(*vn::encodeMessage(vn::Coins{0x01020304, 5}) == std::vector<std::uint8_t>{4, 3, 2, 1, 5, 0, 0, 0});
    CHECK(static_cast<std::uint16_t>(vn::Opcode::LootOpenReq) == 0x0500);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::Coins) == 0x0507);
    CHECK(vn::opcodeName(vn::Opcode::LootTakeReq) == "LOOT_TAKE_REQ");
    CHECK(vn::isKnownOpcode(0x0507));
    CHECK_FALSE(vn::isKnownOpcode(0x0508));
    CHECK(vn::lootResultName(vn::LootResult::PurseFull) == "PurseFull");
    CHECK(vn::kEntityTypeLoot == 3);
    CHECK(vn::kEntityStateKnown == 0x1F);
}

TEST_CASE("Loot message readers reject values and lists the contract forbids", "[net][messages][loot]")
{
    vn::LootOpenRes open;
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0, 9}, open)); // result 9
    vn::LootTakeRes take;
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0, 1, 0, 0, 0, 9}, take));
    vn::LootClosed closed;
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0, 0}, closed)); // reason 0
    CHECK_FALSE(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0, 4}, closed)); // reason 4
    vn::LootContents c;
    const auto contents = [](std::uint8_t count, std::vector<std::uint8_t> entries) {
        std::vector<std::uint8_t> b{1, 0, 0, 0, 0, 0, 0, 0, count};
        b.insert(b.end(), entries.begin(), entries.end());
        return b;
    };
    CHECK(vn::decodeMessage(contents(1, {7, 1, 0, 1, 0}), c));
    CHECK_FALSE(vn::decodeMessage(contents(1, {8, 1, 0, 1, 0}), c));                 // index 8
    CHECK_FALSE(vn::decodeMessage(contents(1, {0, 0, 0, 1, 0}), c));                 // template 0
    CHECK_FALSE(vn::decodeMessage(contents(1, {0, 1, 0, 0, 0}), c));                 // quantity 0
    CHECK_FALSE(vn::decodeMessage(contents(2, {3, 1, 0, 1, 0, 3, 1, 0, 1, 0}), c)); // index twice
    CHECK_FALSE(vn::decodeMessage(contents(2, {3, 1, 0, 1, 0, 2, 1, 0, 1, 0}), c)); // descending
    std::vector<std::uint8_t> nine;
    for (std::uint8_t i = 0; i < 9; ++i)
        nine.insert(nine.end(), {i, 1, 0, 1, 0});
    CHECK_FALSE(vn::decodeMessage(contents(9, nine), c)); // count 9
    // The writer refuses what the reader would refuse.
    CHECK_FALSE(vn::encodeMessage(vn::LootContents{1, 0, {{8, 1, 1}}}).has_value());
    CHECK_FALSE(vn::encodeMessage(vn::LootContents{1, 0, {{2, 1, 1}, {1, 1, 1}}}).has_value());
    CHECK_FALSE(vn::encodeMessage(vn::LootContents{1, 0, {{0, 0, 1}}}).has_value());
    CHECK_FALSE(vn::encodeMessage(vn::LootContents{1, 0, {{0, 1, 0}}}).has_value());
    // A loot state bit is valid now; 0x20 is not.
    vn::EntityState st;
    CHECK(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0, vn::kEntityStateLootMine}, st));
    CHECK(vn::decodeMessage(std::vector<std::uint8_t>{1, 0, 0, 0, vn::kEntityStateLootLocked}, st));
}
