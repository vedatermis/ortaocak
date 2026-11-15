// vo::net::Connection over real loopback sockets: coalesced writes, frame order and per-direction
// AEAD counters, the 256 KB outbound cap, and closing while a write is in flight (the in-flight
// buffer must stay intact until its completion handler ran: the bug class hit in M1).
//
// Run repeatedly (Release) to shake out races, e.g. ctest --repeat until-fail:20 -R Connection.

#include "shared/net/Connection.h"
#include "shared/net/Crypto.h"
#include "shared/net/Messages.h"
#include "shared/net/Opcodes.h"

#include <catch2/catch_test_macros.hpp>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/socket_base.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace vn = vo::net;
using namespace std::chrono_literals;

namespace {

struct KeyPair
{
    vn::SessionKeys server;
    vn::SessionKeys client;
};

KeyPair makeKeys()
{
    auto serverKx = vn::generateKxKeyPair();
    auto clientKx = vn::generateKxKeyPair();
    const auto serverPk = serverKx.publicKey;
    const auto clientPk = clientKx.publicKey;
    KeyPair k;
    REQUIRE(vn::deriveServerSessionKeys(serverKx, clientPk, k.server));
    REQUIRE(vn::deriveClientSessionKeys(clientKx, serverPk, k.client));
    return k;
}

// Two connected sockets on one io_context.
struct SocketPair
{
    asio::io_context io;
    asio::ip::tcp::socket client{io};
    asio::ip::tcp::socket server{io};

    SocketPair()
    {
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        client.connect(acceptor.local_endpoint());
        server = acceptor.accept();
    }
};

template <class Pred>
bool runUntil(asio::io_context& io, Pred done, std::chrono::milliseconds timeout = 10s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        if (io.stopped())
            io.restart();
        io.run_for(5ms);
    }
    return true;
}

// Payload of frame `i`: its index plus deterministic filler of a length that varies with i.
std::vector<std::uint8_t> framePayload(std::uint32_t i, std::size_t maxLen = 3000)
{
    const std::size_t len = 4 + (static_cast<std::size_t>(i) * 7919u) % maxLen;
    std::vector<std::uint8_t> p(len);
    p[0] = static_cast<std::uint8_t>(i);
    p[1] = static_cast<std::uint8_t>(i >> 8);
    p[2] = static_cast<std::uint8_t>(i >> 16);
    p[3] = static_cast<std::uint8_t>(i >> 24);
    for (std::size_t k = 4; k < len; ++k)
        p[k] = static_cast<std::uint8_t>(i * 31u + k);
    return p;
}

bool isFramePayload(std::span<const std::uint8_t> p, std::uint32_t i)
{
    const auto expected = framePayload(i);
    return p.size() == expected.size() && std::equal(p.begin(), p.end(), expected.begin());
}

} // namespace

TEST_CASE("Connection coalesces queued frames into few writes; the peer decodes every frame",
          "[net][connection]")
{
    REQUIRE(vn::initCrypto());
    SocketPair s;
    auto keys = makeKeys();
    auto server = vn::Connection::create(std::move(s.server));
    auto client = vn::Connection::create(std::move(s.client));
    std::vector<std::uint32_t> received;
    client->start(
        [&](std::uint16_t op, std::span<const std::uint8_t> p) {
            vn::Ping ping;
            REQUIRE(op == static_cast<std::uint16_t>(vn::Opcode::Ping));
            REQUIRE(vn::decodeMessage(p, ping));
            received.push_back(ping.clientTimeMs);
        },
        [](vn::CloseReason) {});
    server->start([](std::uint16_t, std::span<const std::uint8_t>) {}, [](vn::CloseReason) {});
    server->enableEncryption(std::move(keys.server));
    client->enableEncryption(std::move(keys.client));

    constexpr std::uint32_t kFrames = 2000;
    server->beginBatch();
    for (std::uint32_t i = 0; i < kFrames / 2; ++i)
        REQUIRE(server->sendMessage(vn::Ping{i}));
    server->endBatch(); // one write for the first half
    for (std::uint32_t i = kFrames / 2; i < kFrames; ++i)
        REQUIRE(server->sendMessage(vn::Ping{i})); // the first starts a write, the rest join the next
    REQUIRE(runUntil(s.io, [&] { return received.size() >= kFrames; }));
    for (std::uint32_t i = 0; i < kFrames; ++i)
        REQUIRE(received[i] == i);
    CHECK(server->writeCount() <= 4);
    CHECK(server->framesSent() == kFrames);
    CHECK(client->framesReceived() == kFrames);
    CHECK(server->queuedBytes() == 0);
}

TEST_CASE("Connection: both directions stream concurrently; frame order and AEAD counters stay in step",
          "[net][connection]")
{
    REQUIRE(vn::initCrypto());
    SocketPair s;
    auto keys = makeKeys();
    auto a = vn::Connection::create(std::move(s.server));
    auto b = vn::Connection::create(std::move(s.client));
    constexpr std::uint32_t kFrames = 3000;
    constexpr std::uint32_t kReplyBase = 100000;
    std::uint32_t nextA = kReplyBase; // a only receives b's replies
    std::uint32_t nextB = 0;
    std::uint32_t repliesSent = 0;
    bool corrupt = false;
    std::optional<vn::CloseReason> closedA, closedB;
    a->start(
        [&](std::uint16_t op, std::span<const std::uint8_t> p) {
            corrupt = corrupt || op != 0x0500 || !isFramePayload(p, nextA);
            ++nextA;
        },
        [&](vn::CloseReason r) { closedA = r; });
    b->start(
        [&](std::uint16_t op, std::span<const std::uint8_t> p) {
            corrupt = corrupt || op != 0x0500 || !isFramePayload(p, nextB);
            ++nextB;
            // Replies sent from inside the read handler join that read's batch.
            if (nextB % 7 == 0)
            {
                REQUIRE(b->send(0x0500, framePayload(kReplyBase + repliesSent)));
                ++repliesSent;
            }
        },
        [&](vn::CloseReason r) { closedB = r; });
    a->enableEncryption(std::move(keys.server));
    b->enableEncryption(std::move(keys.client));

    // a -> b in random chunk sizes, some batched, interleaved with running the io_context, so
    // frames are appended both while a write is in flight and while none is.
    std::mt19937 rng(1234);
    std::uint32_t sentA = 0;
    while (sentA < kFrames)
    {
        const std::uint32_t n = std::min<std::uint32_t>(kFrames - sentA, 1 + static_cast<std::uint32_t>(rng() % 40));
        const bool batch = rng() % 2 == 0;
        if (batch)
            a->beginBatch();
        for (std::uint32_t k = 0; k < n; ++k)
        {
            REQUIRE(a->send(0x0500, framePayload(sentA)));
            ++sentA;
        }
        if (batch)
            a->endBatch();
        if (rng() % 3 == 0)
            s.io.poll();
        // Stay below the outbound cap: let the peer drain.
        REQUIRE(runUntil(s.io, [&] { return a->queuedBytes() < 96 * 1024; }));
    }
    REQUIRE(runUntil(s.io, [&] { return nextB >= kFrames && nextA >= kReplyBase + kFrames / 7; }));
    CHECK_FALSE(corrupt);
    CHECK_FALSE(closedA.has_value());
    CHECK_FALSE(closedB.has_value());
    CHECK(a->framesSent() == kFrames);
    CHECK(b->framesReceived() == kFrames);
    CHECK(b->framesSent() == kFrames / 7);
    CHECK(a->framesReceived() == kFrames / 7);
    CHECK(a->writeCount() < kFrames); // coalesced
}

TEST_CASE("Connection closes when the outbound queue exceeds 256 KB", "[net][connection]")
{
    REQUIRE(vn::initCrypto());
    SECTION("io_context not running: the first write is in flight, the rest pile up")
    {
        SocketPair s;
        auto conn = vn::Connection::create(std::move(s.server));
        std::optional<vn::CloseReason> reason;
        conn->start([](std::uint16_t, std::span<const std::uint8_t>) {}, [&](vn::CloseReason r) { reason = r; });
        conn->enableEncryption(makeKeys().server);
        const std::vector<std::uint8_t> payload(8000, 0x42);
        std::size_t sent = 0;
        while (conn->send(0x0011, payload))
            ++sent;
        CHECK(sent * (8000 + 2 + 2 + 16) <= vn::kMaxOutboundQueueBytes);
        CHECK(sent >= 30);
        CHECK_FALSE(conn->isOpen());
        s.io.run_for(1s);
        REQUIRE(reason.has_value());
        CHECK(*reason == vn::CloseReason::OutboundOverflow);
    }
    SECTION("inside a batch nothing is written at all")
    {
        SocketPair s;
        auto conn = vn::Connection::create(std::move(s.server));
        std::optional<vn::CloseReason> reason;
        conn->start([](std::uint16_t, std::span<const std::uint8_t>) {}, [&](vn::CloseReason r) { reason = r; });
        conn->enableEncryption(makeKeys().server);
        const std::vector<std::uint8_t> payload(8000, 0x42);
        std::size_t sent = 0;
        conn->beginBatch();
        while (conn->send(0x0011, payload))
            ++sent;
        conn->endBatch();
        CHECK(sent * (8000 + 2 + 2 + 16) <= vn::kMaxOutboundQueueBytes);
        CHECK(sent >= 30);
        CHECK(conn->writeCount() == 0);
        s.io.run_for(1s);
        REQUIRE(reason.has_value());
        CHECK(*reason == vn::CloseReason::OutboundOverflow);
    }
    SECTION("a slow reader: the cap counts in-flight and pending bytes")
    {
        SocketPair s;
        // Tiny socket buffers so a write really stays in flight.
        s.server.set_option(asio::socket_base::send_buffer_size(4096));
        s.client.set_option(asio::socket_base::receive_buffer_size(4096));
        auto conn = vn::Connection::create(std::move(s.server));
        std::optional<vn::CloseReason> reason;
        conn->start([](std::uint16_t, std::span<const std::uint8_t>) {}, [&](vn::CloseReason r) { reason = r; });
        conn->enableEncryption(makeKeys().server);
        const std::vector<std::uint8_t> payload(4000, 0x17);
        std::size_t sent = 0;
        bool open = true;
        while (open && sent < 1000)
        {
            open = conn->send(0x0011, payload);
            sent += open ? 1 : 0;
            s.io.poll(); // writes complete as far as the kernel takes them; nobody reads
        }
        CHECK_FALSE(open);
        REQUIRE(runUntil(s.io, [&] { return reason.has_value(); }));
        CHECK(*reason == vn::CloseReason::OutboundOverflow);
    }
}

TEST_CASE("Connection: abort while a write is in flight keeps the in-flight buffer intact",
          "[net][connection]")
{
    REQUIRE(vn::initCrypto());
    for (int round = 0; round < 8; ++round)
    {
        SocketPair s;
        s.server.set_option(asio::socket_base::send_buffer_size(4096));
        s.client.set_option(asio::socket_base::receive_buffer_size(4096));
        auto keys = makeKeys();
        auto server = vn::Connection::create(std::move(s.server));
        auto client = vn::Connection::create(std::move(s.client));
        std::uint32_t next = 0;
        bool corrupt = false;
        std::optional<vn::CloseReason> serverClosed, clientClosed;
        server->start([](std::uint16_t, std::span<const std::uint8_t>) {}, [&](vn::CloseReason r) { serverClosed = r; });
        server->enableEncryption(std::move(keys.server));
        client->enableEncryption(std::move(keys.client));

        // ~200 KB: one big write in flight, then more frames pending behind it (m_pending grows and
        // reallocates while the write references m_inflight).
        for (std::uint32_t i = 0; i < 40; ++i)
            REQUIRE(server->send(0x0500, framePayload(i)));
        s.io.poll(); // start the write: it cannot finish, the peer is not reading yet
        for (std::uint32_t i = 40; i < 120; ++i)
            REQUIRE(server->send(0x0500, framePayload(i)));
        REQUIRE(server->writeCount() >= 1);
        if (round % 2 == 0)
            server->abort(); // local abort with the write in flight
        // Now the peer reads: whatever arrives must be an intact, correctly encrypted prefix.
        client->start(
            [&](std::uint16_t op, std::span<const std::uint8_t> p) {
                corrupt = corrupt || op != 0x0500 || !isFramePayload(p, next);
                ++next;
            },
            [&](vn::CloseReason r) { clientClosed = r; });
        if (round % 2 == 1)
        {
            // Peer side aborts (RST) while the server's write is in flight.
            s.io.poll();
            client->abort();
        }
        REQUIRE(runUntil(s.io, [&] { return serverClosed.has_value() && clientClosed.has_value(); }));
        CHECK_FALSE(corrupt);
        CHECK(next <= 120);
        // Sends after the close are refused; nothing is left queued.
        CHECK_FALSE(server->send(0x0500, framePayload(0)));
        s.io.run_for(20ms);
        CHECK(server->queuedBytes() == 0);
        if (round % 2 == 0)
            CHECK(*serverClosed == vn::CloseReason::LocalClose);
        // Release the connections while the io_context still holds their handlers (if any).
        server.reset();
        client.reset();
    }
}

TEST_CASE("Connection: graceful close flushes in-flight and pending frames to a slow reader",
          "[net][connection]")
{
    REQUIRE(vn::initCrypto());
    SocketPair s;
    s.server.set_option(asio::socket_base::send_buffer_size(4096));
    s.client.set_option(asio::socket_base::receive_buffer_size(4096));
    auto keys = makeKeys();
    auto server = vn::Connection::create(std::move(s.server));
    auto client = vn::Connection::create(std::move(s.client));
    std::uint32_t next = 0;
    bool corrupt = false;
    std::optional<vn::CloseReason> serverClosed, clientClosed;
    server->start([](std::uint16_t, std::span<const std::uint8_t>) {}, [&](vn::CloseReason r) { serverClosed = r; });
    server->enableEncryption(std::move(keys.server));
    client->enableEncryption(std::move(keys.client));
    for (std::uint32_t i = 0; i < 40; ++i)
        REQUIRE(server->send(0x0500, framePayload(i)));
    s.io.poll();
    for (std::uint32_t i = 40; i < 80; ++i)
        REQUIRE(server->send(0x0500, framePayload(i)));
    server->setCloseTimeout(10s);
    server->close();
    CHECK_FALSE(server->send(0x0500, framePayload(0))); // closing: refused
    client->start(
        [&](std::uint16_t op, std::span<const std::uint8_t> p) {
            corrupt = corrupt || op != 0x0500 || !isFramePayload(p, next);
            ++next;
        },
        [&](vn::CloseReason r) { clientClosed = r; });
    REQUIRE(runUntil(s.io, [&] { return clientClosed.has_value(); }));
    CHECK(*clientClosed == vn::CloseReason::PeerClosed);
    CHECK(next == 80);
    CHECK_FALSE(corrupt);
    client.reset();
    REQUIRE(runUntil(s.io, [&] { return serverClosed.has_value(); }));
    CHECK(*serverClosed == vn::CloseReason::LocalClose);
}

TEST_CASE("Connection: a frame that cannot be encoded does not advance the AEAD counter", "[net][connection]")
{
    REQUIRE(vn::initCrypto());
    SocketPair s;
    auto keys = makeKeys();
    auto server = vn::Connection::create(std::move(s.server));
    auto client = vn::Connection::create(std::move(s.client));
    std::vector<std::uint32_t> got;
    std::optional<vn::CloseReason> clientClosed;
    client->start(
        [&](std::uint16_t, std::span<const std::uint8_t> p) {
            vn::Ping ping;
            if (vn::decodeMessage(p, ping))
                got.push_back(ping.clientTimeMs);
        },
        [&](vn::CloseReason r) { clientClosed = r; });
    server->start([](std::uint16_t, std::span<const std::uint8_t>) {}, [](vn::CloseReason) {});
    server->enableEncryption(std::move(keys.server));
    client->enableEncryption(std::move(keys.client));
    REQUIRE(server->sendMessage(vn::Ping{1}));
    const std::vector<std::uint8_t> tooLarge(vn::kMaxPlaintextPerFrame, 0); // + 2 opcode bytes > limit
    CHECK_FALSE(server->send(0x0011, tooLarge));
    CHECK(server->isOpen());
    REQUIRE(server->sendMessage(vn::Ping{2}));
    REQUIRE(runUntil(s.io, [&] { return got.size() >= 2 || clientClosed.has_value(); }));
    CHECK_FALSE(clientClosed.has_value());
    REQUIRE(got.size() == 2);
    CHECK(got[1] == 2);
}
