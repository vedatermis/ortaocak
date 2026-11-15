#include "shared/net/PacketReader.h"
#include "shared/net/PacketWriter.h"
#include "shared/net/Utf8.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using vo::net::PacketReader;
using vo::net::PacketWriter;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> v)
{
    std::vector<std::uint8_t> out;
    for (int b : v)
        out.push_back(static_cast<std::uint8_t>(b));
    return out;
}

} // namespace

TEST_CASE("PacketWriter encodes little-endian", "[net][packet]")
{
    PacketWriter w;
    w.writeU8(0xAB);
    w.writeU16(0x1234);
    w.writeU32(0x4C4E4F56);
    w.writeU64(0x0102030405060708ull);
    w.writeBool(true);
    w.writeBool(false);
    REQUIRE(w.ok());
    CHECK(w.buffer() == bytes({0xAB, 0x34, 0x12, 0x56, 0x4F, 0x4E, 0x4C, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
                               0x01, 0x01, 0x00}));
}

TEST_CASE("PacketWriter f32 is IEEE-754 single, little-endian", "[net][packet]")
{
    PacketWriter w;
    w.writeF32(1.0f);  // 0x3F800000
    w.writeF32(-2.5f); // 0xC0200000
    CHECK(w.buffer() == bytes({0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x20, 0xC0}));
}

TEST_CASE("PacketReader round-trips every field type", "[net][packet]")
{
    PacketWriter w;
    w.writeU8(200);
    w.writeU16(65535);
    w.writeU32(0xDEADBEEF);
    w.writeU64(std::numeric_limits<std::uint64_t>::max() - 5);
    w.writeF32(3.25f);
    w.writeBool(true);
    w.writeStr8("Çağrı");
    w.writeStr16(std::string(300, 'x'));
    const std::vector<std::uint8_t> raw = {9, 8, 7};
    w.writeBytes(raw);
    REQUIRE(w.ok());

    PacketReader r(w.data());
    CHECK(r.readU8() == 200);
    CHECK(r.readU16() == 65535);
    CHECK(r.readU32() == 0xDEADBEEF);
    CHECK(r.readU64() == std::numeric_limits<std::uint64_t>::max() - 5);
    CHECK(r.readF32() == 3.25f);
    CHECK(r.readBool());
    CHECK(r.readStr8() == "Çağrı");
    CHECK(r.readStr16() == std::string(300, 'x'));
    std::vector<std::uint8_t> got(3);
    CHECK(r.readBytes(got));
    CHECK(got == raw);
    CHECK(r.ok());
    CHECK(r.finished());
}

TEST_CASE("PacketReader truncation sets a sticky error", "[net][packet]")
{
    SECTION("u16 with one byte")
    {
        const auto data = bytes({0x01});
        PacketReader r(data);
        CHECK(r.readU16() == 0);
        CHECK_FALSE(r.ok());
        CHECK_FALSE(r.finished());
    }
    SECTION("u32 with three bytes")
    {
        const auto data = bytes({1, 2, 3});
        PacketReader r(data);
        CHECK(r.readU32() == 0);
        CHECK_FALSE(r.ok());
    }
    SECTION("u64 with seven bytes")
    {
        const auto data = bytes({1, 2, 3, 4, 5, 6, 7});
        PacketReader r(data);
        CHECK(r.readU64() == 0);
        CHECK_FALSE(r.ok());
    }
    SECTION("string longer than the remaining bytes")
    {
        const auto data = bytes({5, 'a', 'b'});
        PacketReader r(data);
        CHECK(r.readStr8().empty());
        CHECK_FALSE(r.ok());
    }
    SECTION("str16 length prefix truncated")
    {
        const auto data = bytes({5});
        PacketReader r(data);
        CHECK(r.readStr16().empty());
        CHECK_FALSE(r.ok());
    }
    SECTION("raw bytes")
    {
        const auto data = bytes({1, 2});
        PacketReader r(data);
        std::vector<std::uint8_t> out(4, 0xEE);
        CHECK_FALSE(r.readBytes(out));
        CHECK(out == std::vector<std::uint8_t>(4, 0));
    }
    SECTION("error is sticky: later reads return zero even if bytes remain")
    {
        const auto data = bytes({2, 0xFF, 0x07, 0x07});
        PacketReader r(data);
        CHECK(r.readU8() == 2);
        CHECK_FALSE(r.readBool()); // 0xFF is not a bool
        CHECK_FALSE(r.ok());
        CHECK(r.readU8() == 0); // a byte remains, but the reader stays failed
        CHECK(r.remaining() == 0);
        CHECK_FALSE(r.finished());
    }
}

TEST_CASE("PacketReader bool accepts only 0 and 1", "[net][packet]")
{
    for (int v : {0, 1})
    {
        const auto data = bytes({v});
        PacketReader r(data);
        CHECK(r.readBool() == (v == 1));
        CHECK(r.finished());
    }
    for (int v : {2, 0x7F, 0xFF})
    {
        const auto data = bytes({v});
        PacketReader r(data);
        (void)r.readBool();
        CHECK_FALSE(r.ok());
    }
}

TEST_CASE("PacketReader finished() detects trailing bytes", "[net][packet]")
{
    const auto data = bytes({1, 0, 0xAA});
    PacketReader r(data);
    CHECK(r.readU16() == 1);
    CHECK(r.ok());
    CHECK_FALSE(r.finished());
    CHECK(r.remaining() == 1);
    (void)r.readU8();
    CHECK(r.finished());

    PacketReader empty;
    CHECK(empty.finished());
}

TEST_CASE("PacketReader rejects invalid UTF-8 strings", "[net][packet][utf8]")
{
    const std::vector<std::vector<std::uint8_t>> bad = {
        bytes({2, 0xC0, 0x80}),             // overlong NUL
        bytes({3, 0xE0, 0x80, 0xAF}),       // overlong '/'
        bytes({3, 0xED, 0xA0, 0x80}),       // UTF-16 surrogate U+D800
        bytes({4, 0xF4, 0x90, 0x80, 0x80}), // > U+10FFFF
        bytes({1, 0xFF}),                   // invalid lead byte
        bytes({1, 0x80}),                   // lone continuation byte
        bytes({2, 0xC3, 0x41}),             // bad continuation
        bytes({1, 0xC3}),                   // truncated sequence
    };
    for (const auto& data : bad)
    {
        PacketReader r(data);
        CHECK(r.readStr8().empty());
        CHECK_FALSE(r.ok());
    }

    const auto good = bytes({4, 0xF0, 0x9F, 0x98, 0x80}); // U+1F600
    PacketReader r(good);
    CHECK(r.readStr8() == "\xF0\x9F\x98\x80");
    CHECK(r.finished());
}

TEST_CASE("PacketWriter rejects strings that do not fit or are not UTF-8", "[net][packet]")
{
    PacketWriter w;
    w.writeStr8(std::string(255, 'a'));
    CHECK(w.ok());
    w.writeStr8(std::string(256, 'a'));
    CHECK_FALSE(w.ok());
    const auto size = w.size();
    w.writeU32(1); // ignored after failure
    CHECK(w.size() == size);

    PacketWriter w2;
    w2.writeStr16(std::string(65535, 'b'));
    CHECK(w2.ok());
    w2.writeStr16(std::string(65536, 'b'));
    CHECK_FALSE(w2.ok());

    PacketWriter w3;
    w3.writeStr8("\xC0\x80");
    CHECK_FALSE(w3.ok());

    w3.clear();
    CHECK(w3.ok());
    CHECK(w3.size() == 0);
}

TEST_CASE("UTF-8 decode and encode round-trip", "[net][utf8]")
{
    std::u32string cps;
    REQUIRE(vo::net::decodeUtf8("aİı€\xF0\x9F\x98\x80", cps));
    REQUIRE(cps == std::u32string{U'a', U'İ', U'ı', U'€', U'\U0001F600'});
    std::string back;
    for (char32_t cp : cps)
        vo::net::appendUtf8(cp, back);
    CHECK(back == "aİı€\xF0\x9F\x98\x80");
    CHECK(vo::net::isValidUtf8(""));
    CHECK_FALSE(vo::net::isValidUtf8("\xE2\x82"));
}
