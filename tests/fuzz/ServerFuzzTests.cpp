// Protocol fuzzing of a real in-process server (M7): a Server + World on an ephemeral 127.0.0.1 port
// is driven over real sockets with
//   A) garbage before the handshake: random bytes, bad length fields, wrong opcodes, bad magic,
//      truncated HELLO, wrong version, garbage right after a valid HELLO, silent connections;
//   B) after a valid handshake (Auth state): pipelined scripts of valid PINGs mixed with mutated /
//      truncated / extended / oversized payloads, out-of-state opcodes, unknown opcodes and raw
//      bytes that break the AEAD stream;
//   C) in the world: valid and mutated MOVE_REQ and ITEM_MOVE_REQ, out-of-state opcodes, LEAVE_WORLD_REQ with moves
//      behind it, garbage.
// For A and B every connection's outcome is checked against the spec (§2, §3, §4, §6): exactly the
// PONGs for the valid PINGs before the first violation, then ERROR(ProtocolViolation | BadState)
// and a close, or a silent close for framing/AEAD errors; nothing but HELLO_ACK in plaintext.
// For C an ERROR, if any, must be ProtocolViolation, BadState or Flood. Overall: no crash, every
// connection ends within its deadline (no hang), no session is left at the end (no leak), the
// process memory does not keep growing between the two halves of the run, and a fresh client can
// still log in and play afterwards.
//
// Deterministic for a given seed (the RNG decides every byte sent; the oracle does not depend on
// timing). Environment: VO_FUZZ_SEED (default 1), VO_FUZZ_SCALE (default 1 = the short slice run
// by ctest, ~45k frames; 20 = ~1M frames, a few minutes in Release). See docs/BUILDING.md.

#include "server/net/Session.h"
#include "shared/core/Log.h"
#include "shared/net/ClientSession.h"
#include "shared/net/FrameCodec.h"
#include "shared/net/Messages.h"
#include "tests/server/TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace vn = vo::net;
namespace vs = vo::server::net;
using namespace std::chrono_literals;
using Rng = std::mt19937_64;
using Bytes = std::vector<std::uint8_t>;

namespace {

int envInt(const char* name, int fallback)
{
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
        return fallback;
    const int v = std::atoi(value);
    std::free(value);
    return v > 0 ? v : fallback;
}

std::uint64_t privateBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return pmc.PrivateUsage;
#endif
    return 0;
}

std::uint32_t pick(Rng& rng, std::uint32_t n)
{
    return static_cast<std::uint32_t>(rng() % n);
}

Bytes randomBytes(Rng& rng, std::size_t n)
{
    Bytes b(n);
    for (auto& x : b)
        x = static_cast<std::uint8_t>(rng());
    return b;
}

template <class T>
Bytes encode(const T& msg)
{
    auto p = vn::encodeMessage(msg);
    REQUIRE(p.has_value());
    return *p;
}

// ITEM_MOVE_REQ between mostly valid places (equipment 0..7, bag 0..27), sometimes out of range.
vn::ItemMoveReq randomItemMove(Rng& rng)
{
    vn::ItemMoveReq m;
    m.seq = static_cast<std::uint32_t>(rng());
    m.fromContainer = static_cast<std::uint8_t>(pick(rng, 3));
    m.fromSlot = static_cast<std::uint8_t>(pick(rng, 30));
    m.toContainer = static_cast<std::uint8_t>(pick(rng, 3));
    m.toSlot = static_cast<std::uint8_t>(pick(rng, 30));
    return m;
}

// A valid payload of a random message type (client or server direction) and its opcode.
std::pair<std::uint16_t, Bytes> randomMessage(Rng& rng)
{
    auto op = [](vn::Opcode o) { return static_cast<std::uint16_t>(o); };
    switch (pick(rng, 38))
    {
    // v6 skills: learn requests for any id (mostly the shipped 1..28), whole bars, skills 1..30
    // with targets (areas, leaps, heals, buffs), and a server packet sent by a client.
    case 25:
        return {op(vn::Opcode::SkillLearnReq),
                encode(vn::SkillLearnReq{static_cast<std::uint32_t>(rng()),
                                         static_cast<std::uint16_t>(pick(rng, 8) == 0 ? rng() : pick(rng, 30))})};
    case 26:
    {
        vn::SkillBarSetReq m;
        m.seq = static_cast<std::uint32_t>(rng());
        for (auto& s : m.slots)
            s = static_cast<std::uint16_t>(pick(rng, 8) == 0 ? rng() : pick(rng, 30));
        return {op(vn::Opcode::SkillBarSetReq), encode(m)};
    }
    case 27:
        return {op(vn::Opcode::SkillReq),
                encode(vn::SkillReq{static_cast<std::uint16_t>(1 + pick(rng, 30)), pick(rng, 3) == 0 ? 0 : pick(rng, 40)})};
    case 28: return {op(vn::Opcode::EffectApply), encode(vn::EffectApply{1, {5, 1, 1, 0, 10, 5, 0}})};
    // v8 shop: shop requests for mostly small entity ids (players, monsters, NPCs), buys / sells of
    // any template / slot / quantity, consumable uses (mostly the potions 16..19, any slot), bars with
    // item slots, and a server packet sent by a client.
    case 31: return {op(vn::Opcode::ShopOpenReq), encode(vn::ShopOpenReq{pick(rng, 40)})};
    case 32: return {op(vn::Opcode::ShopCloseReq), encode(vn::ShopCloseReq{pick(rng, 40)})};
    case 33:
        return {op(vn::Opcode::ShopBuyReq),
                encode(vn::ShopBuyReq{static_cast<std::uint32_t>(rng()), pick(rng, 40), static_cast<std::uint16_t>(pick(rng, 22)),
                                      static_cast<std::uint16_t>(pick(rng, 8) == 0 ? rng() : pick(rng, 25))})};
    case 34:
        return {op(vn::Opcode::ShopSellReq),
                encode(vn::ShopSellReq{static_cast<std::uint32_t>(rng()), pick(rng, 40), static_cast<std::uint8_t>(pick(rng, 32)),
                                       static_cast<std::uint16_t>(pick(rng, 4))})};
    case 35:
        return {op(vn::Opcode::ItemUseReq),
                encode(vn::ItemUseReq{static_cast<std::uint32_t>(rng()), static_cast<std::uint16_t>(14 + pick(rng, 8)),
                                      static_cast<std::uint8_t>(pick(rng, 4) == 0 ? 0xFF : pick(rng, 30))})};
    case 36:
    {
        vn::SkillBarSetReq m;
        m.seq = static_cast<std::uint32_t>(rng());
        for (auto& s : m.slots)
            s = static_cast<std::uint16_t>(pick(rng, 2) == 0 ? (0x8000u | pick(rng, 22)) : pick(rng, 30));
        return {op(vn::Opcode::SkillBarSetReq), encode(m)};
    }
    case 37: return {op(vn::Opcode::ItemUsed), encode(vn::ItemUsed{1, 16, 70, 0})};
    // v7 zones: ZONE_LOADED_REQ for any zone (mostly out of state), and a server packet sent by a client.
    case 29: return {op(vn::Opcode::ZoneLoadedReq), encode(vn::ZoneLoadedReq{static_cast<std::uint16_t>(pick(rng, 5))})};
    case 30: return {op(vn::Opcode::ZoneChangeBegin), encode(vn::ZoneChangeBegin{1, 2})};
    // v5 stat allocations: mostly small amounts (often valid after a level-up), sometimes huge.
    case 23:
    {
        vn::StatAllocateReq m;
        m.seq = static_cast<std::uint32_t>(rng());
        for (auto& p : m.points)
            p = static_cast<std::uint16_t>(pick(rng, 8) == 0 ? rng() : pick(rng, 3));
        return {op(vn::Opcode::StatAllocateReq), encode(m)};
    }
    case 24: return {op(vn::Opcode::CharStats), encode(vn::CharStats{})}; // a server packet sent by a client
    // v3 combat requests: entity ids mostly small (existing players / monsters), skills 0..5.
    case 14: return {op(vn::Opcode::AttackReq), encode(vn::AttackReq{pick(rng, 40)})};
    case 15:
        return {op(vn::Opcode::SkillReq),
                encode(vn::SkillReq{static_cast<std::uint16_t>(pick(rng, 6)), pick(rng, 40)})};
    case 16: return {op(vn::Opcode::TargetReq), encode(vn::TargetReq{pick(rng, 40)})};
    case 17: return {op(vn::Opcode::StanceReq), encode(vn::StanceReq{static_cast<std::uint8_t>(pick(rng, 2))})};
    case 18: return {op(vn::Opcode::RespawnReq), encode(vn::RespawnReq{})};
    case 19: return {op(vn::Opcode::AttackStopReq), encode(vn::AttackStopReq{})};
    // v4 loot requests: container ids mostly small, every `what` (entries, 0xFE, 0xFF and the rest).
    case 20: return {op(vn::Opcode::LootOpenReq), encode(vn::LootOpenReq{pick(rng, 60)})};
    case 21:
        return {op(vn::Opcode::LootTakeReq),
                encode(vn::LootTakeReq{static_cast<std::uint32_t>(rng()), pick(rng, 60), static_cast<std::uint8_t>(rng())})};
    case 22: return {op(vn::Opcode::LootCloseReq), encode(vn::LootCloseReq{pick(rng, 60)})};
    case 11: return {op(vn::Opcode::ItemMoveReq), encode(randomItemMove(rng))};
    case 12: return {op(vn::Opcode::Inventory), encode(vn::Inventory{28, {{1, 2, 3, 4, 1}}})};
    case 0: return {op(vn::Opcode::Ping), encode(vn::Ping{static_cast<std::uint32_t>(rng())})};
    case 1: return {op(vn::Opcode::LoginReq), encode(vn::LoginReq{"fuzz_user", "fuzz_password"})};
    case 2: return {op(vn::Opcode::CharListReq), encode(vn::CharListReq{})};
    case 3: return {op(vn::Opcode::CharCreateReq), encode(vn::CharCreateReq{"Fuzzer", 1, 1, 0, 0})};
    case 4: return {op(vn::Opcode::CharDeleteReq), encode(vn::CharDeleteReq{static_cast<std::uint32_t>(rng())})};
    case 5: return {op(vn::Opcode::EnterWorldReq), encode(vn::EnterWorldReq{static_cast<std::uint32_t>(rng() % 4)})};
    case 6: return {op(vn::Opcode::LeaveWorldReq), encode(vn::LeaveWorldReq{})};
    case 7: return {op(vn::Opcode::Logout), encode(vn::Logout{})};
    case 8:
    {
        vn::MoveReq m;
        m.seq = static_cast<std::uint32_t>(rng());
        m.flags = static_cast<std::uint8_t>(rng() % 4);
        m.x = static_cast<float>(rng() % 512);
        m.z = static_cast<float>(rng() % 512);
        return {op(vn::Opcode::MoveReq), encode(m)};
    }
    case 9: return {op(vn::Opcode::Pong), encode(vn::Pong{1, 2})};
    case 10: return {op(vn::Opcode::EntityDespawn), encode(vn::EntityDespawn{7})};
    default: return {static_cast<std::uint16_t>(rng()), randomBytes(rng, pick(rng, 40))};
    }
}

// Bit flips, truncation, extension or replacement.
void mutate(Rng& rng, Bytes& p)
{
    switch (pick(rng, 5))
    {
    case 0:
        if (!p.empty())
            for (std::uint32_t k = 0, n = 1 + pick(rng, 4); k < n; ++k)
                p[pick(rng, static_cast<std::uint32_t>(p.size()))] ^= static_cast<std::uint8_t>(1u << pick(rng, 8));
        break;
    case 1: p.resize(p.empty() ? 0 : pick(rng, static_cast<std::uint32_t>(p.size()))); break;
    case 2:
    {
        const auto extra = randomBytes(rng, 1 + pick(rng, 64));
        p.insert(p.end(), extra.begin(), extra.end());
        break;
    }
    case 3: p = randomBytes(rng, pick(rng, 300)); break;
    default:
        if (!p.empty())
            p[0] = static_cast<std::uint8_t>(rng()); // e.g. a str8 length or seq byte
        break;
    }
}

struct Totals
{
    std::uint64_t connections = 0;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::uint64_t pongs = 0;
    std::uint64_t errorPv = 0;
    std::uint64_t errorBadState = 0;
    std::uint64_t errorFlood = 0;
    std::uint64_t silentCloses = 0;
    std::uint64_t corrections = 0;
};

// ---------------------------------------------------------------------------------------------
// A) Before the handshake: raw sockets
// ---------------------------------------------------------------------------------------------

struct RawConn
{
    explicit RawConn(asio::io_context& io) : sock(io) {}
    asio::ip::tcp::socket sock;
    Bytes out;
    std::array<std::uint8_t, 4096> buf{};
    Bytes in;
    bool done = false;
    int pattern = 0;
    bool expectAck = false; // a HELLO_ACK may legitimately come back
    std::optional<vn::HelloResult> expectedResult;
};

void startRead(const std::shared_ptr<RawConn>& c)
{
    c->sock.async_read_some(asio::buffer(c->buf), [c](const std::error_code& ec, std::size_t n) {
        if (ec)
        {
            c->done = true;
            return;
        }
        c->in.insert(c->in.end(), c->buf.begin(), c->buf.begin() + static_cast<std::ptrdiff_t>(n));
        startRead(c);
    });
}

Bytes plainFrame(std::uint16_t opcode, const Bytes& payload)
{
    Bytes f;
    REQUIRE(vn::encodePlainFrame(opcode, payload, f));
    return f;
}

Bytes helloPayload(Rng& rng, std::uint32_t magic, std::uint16_t version)
{
    vn::Hello h;
    h.magic = magic;
    h.protocolVersion = version;
    h.clientBuild = static_cast<std::uint32_t>(rng());
    for (auto& b : h.clientKxPk)
        b = static_cast<std::uint8_t>(rng());
    return encode(h);
}

void fuzzBeforeHandshake(vo::tests::TestServer& server, Rng& rng, int connections, Totals& t)
{
    asio::io_context io;
    const asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), server.port());
    constexpr int kBatch = 24;
    for (int base = 0; base < connections; base += kBatch)
    {
        std::vector<std::shared_ptr<RawConn>> batch;
        for (int i = base; i < std::min(connections, base + kBatch); ++i)
        {
            auto c = std::make_shared<RawConn>(io);
            c->sock.connect(ep);
            c->pattern = static_cast<int>(pick(rng, 9));
            switch (c->pattern)
            {
            case 0: c->out = randomBytes(rng, 1 + pick(rng, 600)); break;
            case 1:
            {
                static constexpr std::uint16_t kBad[] = {0, 1, 513, 1000, 8210, 65535};
                const std::uint16_t len = kBad[pick(rng, 6)];
                c->out = {static_cast<std::uint8_t>(len), static_cast<std::uint8_t>(len >> 8)};
                const auto more = randomBytes(rng, pick(rng, 100));
                c->out.insert(c->out.end(), more.begin(), more.end());
                break;
            }
            case 2:
            {
                std::uint16_t op = static_cast<std::uint16_t>(rng());
                if (op == static_cast<std::uint16_t>(vn::Opcode::Hello))
                    op = 0x7777;
                c->out = plainFrame(op, randomBytes(rng, pick(rng, 200)));
                break;
            }
            case 3: c->out = plainFrame(0x0001, helloPayload(rng, vn::kMagic ^ 0x01010101u, vn::kProtocolVersion)); break;
            case 4:
            {
                auto f = plainFrame(0x0001, helloPayload(rng, vn::kMagic, vn::kProtocolVersion));
                f.resize(2 + pick(rng, static_cast<std::uint32_t>(f.size() - 2))); // truncated: never completes
                c->out = std::move(f);
                break;
            }
            case 5:
            {
                // Any other version (v8: the random pick could hit the current version itself).
                auto version = static_cast<std::uint16_t>(2 + pick(rng, 100));
                if (version == vn::kProtocolVersion)
                    ++version;
                c->out = plainFrame(0x0001, helloPayload(rng, vn::kMagic, version));
                c->expectAck = true;
                c->expectedResult = vn::HelloResult::VersionMismatch;
                break;
            }
            case 6:
            {
                // A valid HELLO, then garbage where the first encrypted frame should be.
                c->out = plainFrame(0x0001, helloPayload(rng, vn::kMagic, vn::kProtocolVersion));
                const auto more = randomBytes(rng, 1 + pick(rng, 300));
                c->out.insert(c->out.end(), more.begin(), more.end());
                c->expectAck = true;
                c->expectedResult = vn::HelloResult::Ok;
                break;
            }
            case 7:
            {
                // HELLO with a mutated payload (may still decode).
                auto p = helloPayload(rng, vn::kMagic, vn::kProtocolVersion);
                mutate(rng, p);
                c->out = plainFrame(0x0001, p);
                c->expectAck = true; // if it happens to decode
                break;
            }
            default: break; // connect and say nothing
            }
            t.bytes += c->out.size();
            ++t.frames;
            ++t.connections;
            if (!c->out.empty())
                asio::async_write(c->sock, asio::buffer(c->out), [](const std::error_code&, std::size_t) {});
            startRead(c);
            batch.push_back(std::move(c));
        }
        // Every connection must be closed by the server (handshake timeout at the latest).
        const auto deadline = std::chrono::steady_clock::now() + 6s;
        auto allDone = [&] {
            for (const auto& c : batch)
                if (!c->done)
                    return false;
            return true;
        };
        while (!allDone() && std::chrono::steady_clock::now() < deadline)
            io.run_for(10ms);
        for (const auto& c : batch)
        {
            INFO("pre-handshake pattern " << c->pattern << ", " << c->out.size() << " bytes sent");
            REQUIRE(c->done); // otherwise the server never closed it: hang
            if (c->in.empty())
                continue;
            // Nothing but one HELLO_ACK is ever sent in plaintext.
            REQUIRE(c->expectAck);
            vn::FrameDecoder d;
            d.feed(c->in);
            vn::FrameDecoder::Frame f;
            REQUIRE(d.next(f) == vn::FrameDecoder::Status::Frame);
            std::uint16_t op = 0;
            std::span<const std::uint8_t> payload;
            REQUIRE(vn::splitFrameBody(f.body, op, payload));
            REQUIRE(op == static_cast<std::uint16_t>(vn::Opcode::HelloAck));
            vn::HelloAck ack;
            REQUIRE(vn::decodeMessage(payload, ack));
            if (c->expectedResult)
                CHECK(ack.result == *c->expectedResult);
            // After an OK HELLO_ACK the stream is encrypted: garbage that completes a frame fails the
            // AEAD check and is closed silently; a partial frame waits for the login timeout, whose
            // ERROR(Timeout) is the only thing that may follow (one encrypted frame we cannot read).
            d.setMode(vn::FrameMode::Encrypted);
            const auto after = d.next(f);
            if (after == vn::FrameDecoder::Status::Frame)
            {
                CHECK(ack.result == vn::HelloResult::Ok);
                CHECK(d.next(f) == vn::FrameDecoder::Status::NeedMore);
            }
            else
            {
                CHECK(after == vn::FrameDecoder::Status::NeedMore);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// B/C) After the handshake: ClientSession with scripted frames
// ---------------------------------------------------------------------------------------------

enum class Outcome : std::uint8_t
{
    AllPongs,   // no violation: every PING answered, the connection stays open (we close it)
    ProtocolViolation,
    BadState,
    Silent,     // framing / AEAD error: closed without ERROR (queued PONGs may be dropped)
    Unknown,    // a frame whose effect the oracle does not model (mutated LOGIN_REQ, in-world)
};

struct Step
{
    bool raw = false;
    std::uint16_t opcode = 0;
    Bytes payload;
};

struct FuzzConn
{
    explicit FuzzConn(asio::io_context& io) : session(io) {}
    vn::ClientSession session;
    std::vector<Step> script;
    std::vector<std::uint32_t> pingIds; // valid PINGs before the first violation, in order
    Outcome outcome = Outcome::AllPongs;
    bool world = false;
    // State
    bool connected = false;
    bool failed = false;
    bool closed = false;
    bool scriptSent = false;
    bool checked = false;
    std::vector<vo::tests::Msg> inbox;
    std::chrono::steady_clock::time_point deadline{};
};

// Auth-state script: the oracle follows the server's rules exactly (Session::dispatch).
void buildAuthScript(Rng& rng, FuzzConn& c, std::uint32_t& pingCounter)
{
    const std::uint32_t length = 1 + pick(rng, 120);
    bool violated = false;
    auto violate = [&](Outcome o) {
        if (!violated)
        {
            violated = true;
            c.outcome = o;
        }
    };
    for (std::uint32_t i = 0; i < length; ++i)
    {
        Step s;
        const std::uint32_t kind = pick(rng, 100);
        if (kind < 70)
        {
            const std::uint32_t id = ++pingCounter;
            s.opcode = static_cast<std::uint16_t>(vn::Opcode::Ping);
            s.payload = encode(vn::Ping{id});
            if (!violated)
                c.pingIds.push_back(id);
        }
        else if (kind < 90)
        {
            auto [op, p] = randomMessage(rng);
            mutate(rng, p);
            if (pick(rng, 20) == 0)
                p = randomBytes(rng, vn::kMaxPayloadPerFrame - pick(rng, 16)); // oversized (still legal frame)
            s.opcode = op;
            s.payload = std::move(p);
            if (!violated)
            {
                if (op == static_cast<std::uint16_t>(vn::Opcode::Ping))
                {
                    vn::Ping ping;
                    if (vn::decodeMessage(s.payload, ping))
                        c.pingIds.push_back(ping.clientTimeMs); // a mutated PING that is still a PING
                    else
                        violate(Outcome::ProtocolViolation);
                }
                else if (op == static_cast<std::uint16_t>(vn::Opcode::LoginReq))
                {
                    violate(Outcome::Unknown); // decodes or not, the account service decides
                }
                else
                {
                    REQUIRE_FALSE(vs::isOpcodeAllowed(vs::Session::State::Auth, op));
                    violate(Outcome::BadState);
                }
            }
        }
        else if (kind < 96)
        {
            // Out-of-state but well-formed.
            static constexpr vn::Opcode kOps[] = {vn::Opcode::CharListReq, vn::Opcode::EnterWorldReq,
                                                  vn::Opcode::MoveReq,     vn::Opcode::LeaveWorldReq,
                                                  vn::Opcode::Logout,      vn::Opcode::Hello,
                                                  vn::Opcode::ItemMoveReq, vn::Opcode::LootTakeReq};
            s.opcode = static_cast<std::uint16_t>(kOps[pick(rng, 8)]);
            s.payload = randomBytes(rng, pick(rng, 30));
            violate(Outcome::BadState);
        }
        else
        {
            // Raw bytes into the encrypted stream: a bad length or an AEAD failure.
            s.raw = true;
            s.payload = randomBytes(rng, 1 + pick(rng, 200));
            if (pick(rng, 2) == 0 && s.payload.size() >= 20)
            {
                const std::size_t len = s.payload.size() - 2; // plausible length, garbage ciphertext
                s.payload[0] = static_cast<std::uint8_t>(len);
                s.payload[1] = static_cast<std::uint8_t>(len >> 8);
            }
            violate(Outcome::Silent);
        }
        c.script.push_back(std::move(s));
    }
}

void buildWorldScript(Rng& rng, FuzzConn& c, const vn::EnterWorldRes& enter)
{
    c.outcome = Outcome::Unknown;
    std::uint32_t seq = 0;
    float x = enter.x;
    for (std::uint32_t i = 0, n = 20 + pick(rng, 150); i < n; ++i)
    {
        Step s;
        const std::uint32_t kind = pick(rng, 100);
        vn::MoveReq m;
        m.seq = ++seq;
        m.flags = vn::kMoveFlagMoving | vn::kMoveFlagRunning;
        x += 0.05f;
        m.x = x;
        m.y = enter.y;
        m.z = enter.z;
        m.yaw = 0.5f;
        if (i < 10 || kind < 40) // a few valid moves first, so the world sees the player walk
        {
            s.opcode = static_cast<std::uint16_t>(vn::Opcode::MoveReq);
            s.payload = encode(m);
        }
        else if (kind < 70)
        {
            s.opcode = static_cast<std::uint16_t>(vn::Opcode::MoveReq);
            s.payload = encode(m);
            mutate(rng, s.payload);
        }
        else if (kind < 74)
        {
            s.opcode = static_cast<std::uint16_t>(vn::Opcode::Ping);
            s.payload = encode(vn::Ping{i});
        }
        else if (kind < 80)
        {
            // Item moves, valid or mutated (more than 10/s ends in ERROR(Flood)).
            s.opcode = static_cast<std::uint16_t>(vn::Opcode::ItemMoveReq);
            s.payload = encode(randomItemMove(rng));
            if (pick(rng, 3) == 0)
                mutate(rng, s.payload);
        }
        else if (kind < 88)
        {
            auto [op, p] = randomMessage(rng);
            if (pick(rng, 2) == 0)
                mutate(rng, p);
            s.opcode = op;
            s.payload = std::move(p);
        }
        else if (kind < 94)
        {
            s.opcode = static_cast<std::uint16_t>(vn::Opcode::LeaveWorldReq);
        }
        else
        {
            s.raw = true;
            s.payload = randomBytes(rng, 1 + pick(rng, 100));
        }
        c.script.push_back(std::move(s));
    }
}

void checkAuthOutcome(const FuzzConn& c, Totals& t)
{
    std::size_t i = 0;
    std::size_t pongs = 0;
    for (; i < c.inbox.size() && c.inbox[i].opcode == static_cast<std::uint16_t>(vn::Opcode::Pong); ++i)
    {
        vn::Pong p;
        REQUIRE(vn::decodeMessage(c.inbox[i].payload, p));
        if (c.outcome != Outcome::Unknown)
        {
            REQUIRE(pongs < c.pingIds.size());
            CHECK(p.clientTimeMs == c.pingIds[pongs]);
        }
        ++pongs;
    }
    t.pongs += pongs;
    const auto* rest = i < c.inbox.size() ? &c.inbox[i] : nullptr;
    auto errorCode = [&]() -> std::optional<vn::ErrorCode> {
        if (rest == nullptr || rest->opcode != static_cast<std::uint16_t>(vn::Opcode::Error))
            return std::nullopt;
        vn::Error e;
        REQUIRE(vn::decodeMessage(rest->payload, e));
        return e.errorCode;
    };
    switch (c.outcome)
    {
    case Outcome::AllPongs:
        CHECK(pongs == c.pingIds.size());
        CHECK(rest == nullptr);
        CHECK_FALSE(c.closed); // the server had no reason to close it
        break;
    case Outcome::ProtocolViolation:
    case Outcome::BadState:
    {
        CHECK(pongs == c.pingIds.size());
        const auto code = errorCode();
        REQUIRE(code.has_value());
        CHECK(*code == (c.outcome == Outcome::BadState ? vn::ErrorCode::BadState : vn::ErrorCode::ProtocolViolation));
        CHECK(i + 1 == c.inbox.size()); // nothing after the ERROR
        CHECK(c.closed);
        (*code == vn::ErrorCode::BadState ? t.errorBadState : t.errorPv) += 1;
        break;
    }
    case Outcome::Silent:
        CHECK(pongs <= c.pingIds.size());
        // No ERROR for framing/AEAD errors. Garbage that ends in an incomplete frame leaves the
        // server waiting for the rest of it: then the login timeout's ERROR(Timeout) closes it.
        if (rest != nullptr)
        {
            const auto code = errorCode();
            REQUIRE(code.has_value());
            CHECK(*code == vn::ErrorCode::Timeout);
            CHECK(i + 1 == c.inbox.size());
        }        CHECK(c.closed);
        ++t.silentCloses;
        break;
    case Outcome::Unknown: break;
    }
}

void checkWorldOutcome(const FuzzConn& c, Totals& t)
{
    for (const auto& m : c.inbox)
    {
        if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::PositionCorrect))
            ++t.corrections;
        if (m.opcode != static_cast<std::uint16_t>(vn::Opcode::Error))
            continue;
        vn::Error e;
        REQUIRE(vn::decodeMessage(m.payload, e));
        const bool expected = e.errorCode == vn::ErrorCode::ProtocolViolation || e.errorCode == vn::ErrorCode::BadState ||
                              e.errorCode == vn::ErrorCode::Flood;
        INFO("in-world ERROR " << vn::errorCodeName(e.errorCode) << " '" << e.debugMessage << "'");
        CHECK(expected);
        (e.errorCode == vn::ErrorCode::Flood ? t.errorFlood
                                             : (e.errorCode == vn::ErrorCode::BadState ? t.errorBadState : t.errorPv)) += 1;
    }
}

void sendScript(FuzzConn& c, Totals& t)
{
    for (const Step& s : c.script)
    {
        const bool ok = s.raw ? c.session.sendRawForTesting(s.payload) : c.session.send(s.opcode, s.payload);
        if (!ok)
            break; // closed meanwhile (cannot happen before pumping, but harmless)
        ++t.frames;
        t.bytes += s.payload.size();
    }
    c.scriptSent = true;
}

// Runs `count` Auth-state connections, `parallel` at a time.
void fuzzAfterHandshake(vo::tests::TestServer& server, Rng& rng, int count, Totals& t)
{
    asio::io_context io;
    std::uint32_t pingCounter = 0;
    constexpr int kParallel = 32;
    for (int base = 0; base < count; base += kParallel)
    {
        std::vector<std::unique_ptr<FuzzConn>> conns;
        for (int i = base; i < std::min(count, base + kParallel); ++i)
        {
            auto c = std::make_unique<FuzzConn>(io);
            buildAuthScript(rng, *c, pingCounter);
            FuzzConn* raw = c.get();
            raw->session.setMessageHandler([raw](std::uint16_t op, std::span<const std::uint8_t> p) {
                raw->inbox.push_back({op, Bytes(p.begin(), p.end())});
            });
            raw->session.setClosedHandler([raw](vn::CloseReason) { raw->closed = true; });
            REQUIRE(raw->session.connect(server.clientSettings(), [raw](vn::ConnectResult r, const std::string&) {
                raw->connected = r == vn::ConnectResult::Ok;
                raw->failed = r != vn::ConnectResult::Ok;
            }));
            raw->deadline = std::chrono::steady_clock::now() + 10s;
            ++t.connections;
            conns.push_back(std::move(c));
        }
        auto finished = [](const FuzzConn& c) {
            if (c.failed || c.closed)
                return true;
            if (!c.scriptSent)
                return false;
            if (c.outcome == Outcome::AllPongs)
                return c.inbox.size() >= c.pingIds.size();
            return false; // waiting for the server to close it
        };
        for (;;)
        {
            io.run_for(5ms);
            bool all = true;
            for (auto& c : conns)
            {
                if (c->checked)
                    continue;
                if (c->connected && !c->scriptSent)
                    sendScript(*c, t);
                const bool looseDone = c->outcome == Outcome::Unknown && c->scriptSent &&
                                       std::chrono::steady_clock::now() > c->deadline - 9s; // modelled loosely
                if (finished(*c) || looseDone)
                {
                    // Check and close at once: a finished connection left open would run into the
                    // login timeout while the rest of the batch is still busy.
                    REQUIRE_FALSE(c->failed);
                    checkAuthOutcome(*c, t);
                    c->session.close();
                    c->checked = true;
                    continue;
                }
                all = false;
                INFO("connection did not finish: outcome " << static_cast<int>(c->outcome) << ", "
                                                            << c->inbox.size() << " message(s)");
                REQUIRE(std::chrono::steady_clock::now() < c->deadline); // the server hangs otherwise
            }
            if (all)
                break;
        }
        io.run_for(10ms);

    }
}

void fuzzInWorld(vo::tests::TestServer& server, Rng& rng, int count, Totals& t, int& worldCounter)
{
    for (int i = 0; i < count; ++i)
    {
        // Log in and enter normally, then fire the whole script pipelined and collect until the end.
        const int n = worldCounter++;
        vo::tests::TestClient client;
        REQUIRE(client.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        std::string name = "Fuzzci";
        name += static_cast<char>('a' + n % 26);
        name += static_cast<char>('a' + (n / 26) % 26);
        name += static_cast<char>('a' + (n / 676) % 26);
        const auto enter = client.enterNew("fuzz_w" + std::to_string(n), name, static_cast<std::uint8_t>(1 + n % 2));
        ++t.connections;
        asio::io_context unused;
        FuzzConn c(unused);
        c.world = true;
        buildWorldScript(rng, c, enter);
        for (const Step& s : c.script)
        {
            const bool ok = s.raw ? client.sendRawBytes(s.payload) : client.trySend(s.opcode, s.payload);
            if (!ok)
                break;
            ++t.frames;
            t.bytes += s.payload.size();
        }
        // Until the server closes it or nothing arrives for a second (then the session must still answer).
        while (auto m = client.next(1000ms))
            c.inbox.push_back(std::move(*m));
        checkWorldOutcome(c, t);
        if (!client.closed())
        {
            client.close();
            client.pumpFor(20ms);
        }
    }
}
} // namespace

TEST_CASE("Fuzz: the server survives garbage, mutated and out-of-state frames and closes per the spec",
          "[fuzz][server][net]")
{
    vo::log::setLevel(vo::log::Level::Warn); // a protocol violation per connection is logged at INFO
    const int seed = envInt("VO_FUZZ_SEED", 1);
    const int scale = envInt("VO_FUZZ_SCALE", 1);
    Rng rng(static_cast<std::uint64_t>(seed));
    vs::ServerSettings st;
    st.handshakeTimeout = 300ms;
    st.loginTimeout = 800ms; // a HELLO followed by a partial frame waits for this
    // Mutated LOGIN_REQs count as failed logins from 127.0.0.1; the per-IP limit (tested elsewhere)
    // would otherwise turn the in-world part's logins into TooManyAttempts on long runs.
    st.maxFailedLoginsPerMinute = 1000000;
    st.maxConnections = 200;
    st.overflowConnections = 64;
    // v8: a merchant 4.2 m from the Aydor spawn, so random shop requests reach the world's and the
    // session's checks (PROTOCOL_SHOP.md §2.2).
    vo::tests::TestServer::Options options;
    {
        auto npcs = std::make_shared<vo::game::NpcCatalog>();
        std::string error;
        INFO(error);
        REQUIRE(vo::game::parseNpcCatalog("[[npc]]\nid = 1\nname = \"S\"\ntitle = \"T\"\ngreeting = \"G\"\n"
                                          "model = \"npc_fuzz\"\nzone = 1\nx = 104.2\nz = 100.0\nyaw = 0.0\n"
                                          "kind = \"merchant\"\nstock = [16, 18, 1]\n",
                                          "npcs.toml", vo::tests::testItemCatalog().get(), *npcs, error));
        options.npcs = std::move(npcs);
    }
    vo::tests::TestServer server(st, options);
    Totals t;
    int worldCounter = 0;

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t memMid = 0;
    for (int half = 0; half < 2; ++half)
    {
        fuzzBeforeHandshake(server, rng, 40 * scale, t);
        fuzzAfterHandshake(server, rng, 200 * scale, t);
        fuzzInWorld(server, rng, 3 * scale, t, worldCounter);
        REQUIRE(server.waitNoSessions(10s)); // nothing leaks between the rounds
        if (half == 0)
            memMid = privateBytes();
    }
    const std::uint64_t memEnd = privateBytes();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("fuzz: seed %d scale %d: %llu connections, %llu frames (%llu bytes) in %.1f s; %llu PONG, "
                "ERROR %llu ProtocolViolation / %llu BadState / %llu Flood, %llu silent closes, %llu corrections; "
                "private bytes %.1f MB after the first half, %.1f MB at the end\n",
                seed, scale, static_cast<unsigned long long>(t.connections), static_cast<unsigned long long>(t.frames),
                static_cast<unsigned long long>(t.bytes), seconds, static_cast<unsigned long long>(t.pongs),
                static_cast<unsigned long long>(t.errorPv), static_cast<unsigned long long>(t.errorBadState),
                static_cast<unsigned long long>(t.errorFlood), static_cast<unsigned long long>(t.silentCloses),
                static_cast<unsigned long long>(t.corrections), static_cast<double>(memMid) / 1048576.0,
                static_cast<double>(memEnd) / 1048576.0);
    // Memory must not keep growing: the second half may not cost more than a small allowance.
    if (memMid > 0)
        CHECK(memEnd < memMid + 48ull * 1024 * 1024);
    CHECK(t.errorPv > 0);
    CHECK(t.errorBadState > 0);
    CHECK(t.silentCloses > 0);

    // The server still works for a normal player.
    vo::tests::TestClient ok;
    REQUIRE(ok.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = ok.enterNew("after_fuzz", "Sonra");
    ok.send(vn::Ping{99});
    CHECK(ok.expectSkippingWorld<vn::Pong>().clientTimeMs == 99);
    (void)e;
}
