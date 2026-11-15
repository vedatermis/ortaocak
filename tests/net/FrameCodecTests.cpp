#include "shared/net/Crypto.h"
#include "shared/net/FrameCodec.h"
#include "shared/net/Opcodes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace vn = vo::net;
using Status = vn::FrameDecoder::Status;

namespace {

std::vector<std::uint8_t> payloadOf(std::size_t n, std::uint8_t seed = 1)
{
    std::vector<std::uint8_t> p(n);
    for (std::size_t i = 0; i < n; ++i)
        p[i] = static_cast<std::uint8_t>(seed + i);
    return p;
}

std::vector<std::uint8_t> header(std::size_t length)
{
    return {static_cast<std::uint8_t>(length & 0xFF), static_cast<std::uint8_t>(length >> 8)};
}

void makePair(vn::FrameCipher*& clientOut, vn::FrameCipher*& serverOut, std::vector<vn::FrameCipher>& storage)
{
    auto c = vn::generateKxKeyPair();
    auto s = vn::generateKxKeyPair();
    const auto cpk = c.publicKey;
    const auto spk = s.publicKey;
    vn::SessionKeys ck, sk;
    REQUIRE(vn::deriveClientSessionKeys(c, spk, ck));
    REQUIRE(vn::deriveServerSessionKeys(s, cpk, sk));
    storage.reserve(2);
    storage.emplace_back(std::move(ck));
    storage.emplace_back(std::move(sk));
    clientOut = &storage[0];
    serverOut = &storage[1];
}

} // namespace

TEST_CASE("Plain frames decode when fed one byte at a time", "[net][frame]")
{
    REQUIRE(vn::initCrypto());
    std::vector<std::uint8_t> stream;
    REQUIRE(vn::encodePlainFrame(0x0001, payloadOf(42), stream));
    REQUIRE(vn::encodePlainFrame(0x0002, {}, stream));
    REQUIRE(vn::encodePlainFrame(0x0010, payloadOf(vn::kMaxPlainPayload), stream));

    vn::FrameDecoder dec;
    vn::FrameDecoder::Frame f;
    std::vector<std::vector<std::uint8_t>> frames;
    for (const auto b : stream)
    {
        dec.feed(std::span<const std::uint8_t>(&b, 1));
        Status st;
        while ((st = dec.next(f)) == Status::Frame)
            frames.push_back(f.body);
        REQUIRE(st == Status::NeedMore);
    }
    REQUIRE(frames.size() == 3);

    std::uint16_t op = 0;
    std::span<const std::uint8_t> payload;
    REQUIRE(vn::splitFrameBody(frames[0], op, payload));
    CHECK(op == 0x0001);
    CHECK(std::vector<std::uint8_t>(payload.begin(), payload.end()) == payloadOf(42));
    REQUIRE(vn::splitFrameBody(frames[1], op, payload));
    CHECK(op == 0x0002);
    CHECK(payload.empty());
    REQUIRE(vn::splitFrameBody(frames[2], op, payload));
    CHECK(op == 0x0010);
    CHECK(payload.size() == vn::kMaxPlainPayload);
    CHECK(dec.buffered() == 0);
}

TEST_CASE("Plain frame length limits", "[net][frame]")
{
    std::vector<std::uint8_t> out;
    CHECK(vn::encodePlainFrame(1, payloadOf(510), out));
    CHECK(out.size() == 2 + 512);
    CHECK_FALSE(vn::encodePlainFrame(1, payloadOf(511), out));

    for (std::size_t bad : {std::size_t{0}, std::size_t{1}, std::size_t{513}, std::size_t{65535}})
    {
        vn::FrameDecoder dec;
        dec.feed(header(bad));
        vn::FrameDecoder::Frame f;
        INFO("length " << bad);
        CHECK(dec.next(f) == Status::Error);
        CHECK(dec.failed());
        CHECK(dec.next(f) == Status::Error); // stays failed
    }
    for (std::size_t good : {std::size_t{2}, std::size_t{512}})
    {
        vn::FrameDecoder dec;
        auto bytes = header(good);
        bytes.resize(2 + good, 0x55);
        dec.feed(bytes);
        vn::FrameDecoder::Frame f;
        CHECK(dec.next(f) == Status::Frame);
        CHECK(f.body.size() == good);
    }
}

TEST_CASE("Encrypted frame length limits", "[net][frame]")
{
    for (std::size_t bad : {std::size_t{0}, std::size_t{2}, std::size_t{17}, std::size_t{8209}, std::size_t{65535}})
    {
        vn::FrameDecoder dec(vn::FrameMode::Encrypted);
        dec.feed(header(bad));
        vn::FrameDecoder::Frame f;
        INFO("length " << bad);
        CHECK(dec.next(f) == Status::Error);
    }
    for (std::size_t good : {std::size_t{18}, std::size_t{8208}})
    {
        vn::FrameDecoder dec(vn::FrameMode::Encrypted);
        auto bytes = header(good);
        bytes.resize(2 + good, 0x33);
        // Feed in odd-sized chunks.
        for (std::size_t i = 0; i < bytes.size(); i += 7)
            dec.feed(std::span<const std::uint8_t>(bytes.data() + i, std::min<std::size_t>(7, bytes.size() - i)));
        vn::FrameDecoder::Frame f;
        CHECK(dec.next(f) == Status::Frame);
        CHECK(f.body.size() == good);
    }
}

TEST_CASE("Encrypted frames decode byte by byte and the mode can switch mid-buffer", "[net][frame]")
{
    REQUIRE(vn::initCrypto());
    std::vector<vn::FrameCipher> storage;
    vn::FrameCipher* client = nullptr;
    vn::FrameCipher* server = nullptr;
    makePair(client, server, storage);

    std::vector<std::uint8_t> stream;
    REQUIRE(vn::encodePlainFrame(0x0002, payloadOf(10), stream)); // e.g. HELLO_ACK
    REQUIRE(vn::encodeEncryptedFrame(*server, 0x0011, payloadOf(8), stream));
    REQUIRE(vn::encodeEncryptedFrame(*server, 0x0111, payloadOf(vn::kMaxPayloadPerFrame), stream));
    std::vector<std::uint8_t> tooBig;
    CHECK_FALSE(vn::encodeEncryptedFrame(*server, 1, payloadOf(vn::kMaxPayloadPerFrame + 1), tooBig));

    vn::FrameDecoder dec;
    dec.feed(stream); // everything at once: plain frame followed by encrypted bytes
    vn::FrameDecoder::Frame f;
    REQUIRE(dec.next(f) == Status::Frame);
    CHECK(f.body.size() == 12);
    dec.setMode(vn::FrameMode::Encrypted);

    std::vector<std::uint8_t> plain;
    REQUIRE(dec.next(f) == Status::Frame);
    REQUIRE(client->open(f.body, plain));
    CHECK(plain.size() == 10);
    REQUIRE(dec.next(f) == Status::Frame);
    REQUIRE(client->open(f.body, plain));
    CHECK(plain.size() == vn::kMaxPlaintextPerFrame);
    std::uint16_t op = 0;
    std::span<const std::uint8_t> payload;
    REQUIRE(vn::splitFrameBody(plain, op, payload));
    CHECK(op == 0x0111);
    CHECK(dec.next(f) == Status::NeedMore);

    // Same encrypted stream, one byte at a time.
    std::vector<std::uint8_t> enc;
    REQUIRE(vn::encodeEncryptedFrame(*server, 0x0011, payloadOf(3), enc));
    REQUIRE(vn::encodeEncryptedFrame(*server, 0x0011, payloadOf(4), enc));
    vn::FrameDecoder dec2(vn::FrameMode::Encrypted);
    int got = 0;
    for (const auto b : enc)
    {
        dec2.feed(std::span<const std::uint8_t>(&b, 1));
        while (dec2.next(f) == Status::Frame)
        {
            REQUIRE(client->open(f.body, plain));
            ++got;
        }
    }
    CHECK(got == 2);
}

TEST_CASE("splitFrameBody rejects bodies without an opcode", "[net][frame]")
{
    std::uint16_t op = 0;
    std::span<const std::uint8_t> payload;
    const std::vector<std::uint8_t> one = {1};
    CHECK_FALSE(vn::splitFrameBody(one, op, payload));
}
