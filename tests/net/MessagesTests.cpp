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
    CHECK(bytes[4] == 1);
    CHECK(bytes[5] == 0); // protocol version 1
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
    CHECK(vn::encodeMessage(vn::EnterWorldRes{})->size() == 1 + 4 + 2 + 6 * 4 + 4);
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
