// Robustness of the in-process server (M7): slowloris / idle / half-open connections are timed out
// and release their connection slots, and a forced stop (sessions that do not close within the
// shutdown window) tears everything down without hanging or crashing; the world still saves the
// players whose sessions never got to send their leave.

#include "server/net/Server.h"
#include "server/net/Session.h"
#include "server/world/World.h"
#include "tests/server/TestServer.h"
#include "shared/net/FrameCodec.h"
#include "shared/net/Messages.h"

#include <catch2/catch_test_macros.hpp>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace vn = vo::net;
namespace vs = vo::server::net;
namespace va = vo::server::auth;
using namespace std::chrono_literals;
using vo::tests::TestClient;
using vo::tests::TestServer;

namespace {

// A well-formed plaintext HELLO frame (what a slowloris client trickles in byte by byte).
std::vector<std::uint8_t> helloFrame()
{
    vn::Hello hello;
    hello.protocolVersion = vn::kProtocolVersion;
    hello.clientKxPk = vn::generateKxKeyPair().publicKey;
    auto payload = vn::encodeMessage(hello);
    REQUIRE(payload);
    std::vector<std::uint8_t> frame;
    REQUIRE(vn::encodePlainFrame(static_cast<std::uint16_t>(vn::Opcode::Hello), *payload, frame));
    return frame;
}

// True once the server has closed `sock` (EOF or reset on a read; the server never sends anything
// to these connections).
bool serverClosed(asio::ip::tcp::socket& sock)
{
    std::uint8_t b = 0;
    std::error_code ec;
    sock.non_blocking(true, ec);
    (void)sock.read_some(asio::buffer(&b, 1), ec);
    return ec && ec != asio::error::would_block;
}

} // namespace

TEST_CASE("Server: slowloris connections are dropped at the handshake timeout and free their slots",
          "[server][net][robust]")
{
    vs::ServerSettings st;
    st.maxConnections = 6;
    st.overflowConnections = 2;
    st.handshakeTimeout = 400ms;
    TestServer server(st);
    asio::io_context io;
    const asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), server.port());
    const auto hello = helloFrame();

    // All six slots: four trickle a HELLO one byte per 60 ms (never complete in time), two say nothing.
    std::vector<asio::ip::tcp::socket> socks;
    for (int i = 0; i < 6; ++i)
    {
        socks.emplace_back(io);
        socks.back().connect(ep);
    }
    REQUIRE(server.sessionCount() == 6);
    // While they hold the slots, a real client is turned away with "server full".
    TestClient full;
    CHECK(full.connect(server.clientSettings()) == vn::ConnectResult::ServerFull);

    const auto start = std::chrono::steady_clock::now();
    std::size_t sentBytes = 0;
    while (std::chrono::steady_clock::now() - start < 900ms)
    {
        for (int i = 0; i < 4; ++i)
        {
            std::error_code ec;
            if (sentBytes < hello.size() - 1) // never the last byte
                (void)asio::write(socks[static_cast<std::size_t>(i)], asio::buffer(&hello[sentBytes], 1), ec);
        }
        ++sentBytes;
        std::this_thread::sleep_for(60ms);
    }
    // The handshake timeout closed all of them and every slot is free again.
    REQUIRE(server.waitNoSessions(3s));
    for (auto& s : socks)
        CHECK(serverClosed(s));
    TestClient ok;
    CHECK(ok.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    CHECK(ok.login("after_loris") == vn::LoginResult::Ok);
}

TEST_CASE("Server: a half-closed connection (FIN before the handshake) is released at once",
          "[server][net][robust]")
{
    vs::ServerSettings st;
    st.handshakeTimeout = 20s; // the FIN, not the timeout, must close it
    TestServer server(st);
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    const auto hello = helloFrame();
    asio::write(sock, asio::buffer(hello.data(), 10)); // a partial HELLO ...
    sock.shutdown(asio::ip::tcp::socket::shutdown_send); // ... then the client's FIN
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(server.waitNoSessions(3s));
    CHECK(std::chrono::steady_clock::now() - start < 3s);
    std::error_code ec;
    std::uint8_t b = 0;
    asio::read(sock, asio::buffer(&b, 1), ec);
    CHECK(ec); // EOF: the server closed its side as well, nothing was sent
}

TEST_CASE("Server: a dead peer (logged in, never reads, never closes) is timed out and its slot released",
          "[server][net][robust]")
{
    vs::ServerSettings st;
    st.idleTimeout = 1000ms;
    st.maxConnections = 1;
    TestServer server(st);
    {
        TestClient dead;
        REQUIRE(dead.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        REQUIRE(dead.login("dead_peer") == vn::LoginResult::Ok);
        // From here on the client does nothing at all: no pings, no reads, no FIN (its io_context is
        // not run). The server sends ERROR(Timeout), lingers for the close timeout, then drops it.
        const auto start = std::chrono::steady_clock::now();
        REQUIRE(server.waitNoSessions(8s));
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(took >= 900ms);
        CHECK(took < 6s);
        // The only slot is free again.
        TestClient next;
        CHECK(next.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        CHECK(next.login("next_peer") == vn::LoginResult::Ok);
    }
}

TEST_CASE("Server: forced stop when sessions do not close within the shutdown window",
          "[server][net][robust][shutdown]")
{
    vs::ServerSettings st;
    auto server = std::make_unique<TestServer>(st);
    // Two players in the world whose clients stop reading and never close: shutdown() kicks them
    // (ERROR + graceful close), but the close lingers (2 s close timeout) beyond the 100 ms window
    // given here, so the network threads are stopped with the sessions still open.
    TestClient a;
    TestClient b;
    REQUIRE(a.connect(server->clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(b.connect(server->clientSettings()) == vn::ConnectResult::Ok);
    const auto ea = a.enterNew("forced_a", "Zorla");
    const auto eb = b.enterNew("forced_b", "Durdur", 2);
    std::this_thread::sleep_for(200ms);
    const auto start = std::chrono::steady_clock::now();
    server->stop(100ms);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took < 3s); // bounded: no wait for the lingering closes
    // The world removed both players with a final save although their sessions never closed (the
    // saved positions are the spread spawn positions, not the race spawn points they were created at).
    CHECK(server->world().playerCount() == 0);
    const auto charsA = server->accounts().charactersOf("forced_a");
    const auto charsB = server->accounts().charactersOf("forced_b");
    REQUIRE(charsA.size() == 1);
    REQUIRE(charsB.size() == 1);
    CHECK(charsA[0].position.x == ea.x);
    CHECK(charsA[0].position.z == ea.z);
    CHECK(charsB[0].position.x == eb.x);
    CHECK(charsB[0].position.z == eb.z);
    server.reset();
    // The clients see the connection go away.
    CHECK(a.waitClosed(5s));
    CHECK(b.waitClosed(5s));
}
