// In-process server tests: a real Server on an ephemeral 127.0.0.1 port (own io thread) and
// ClientSession clients driven from the test thread. Covers what the botclient scenarios cannot
// easily reach: connection cap, version mismatch, maintenance, timeouts, duplicate-login kicks,
// shutdown, malformed payloads and the outbound queue cap.

#include "client/net/GameClient.h"
#include "server/auth/MemoryAccountService.h"
#include "server/net/Server.h"
#include "server/net/Session.h"
#include "server/world/World.h"
#include "tests/server/TestServer.h"
#include "shared/net/ClientSession.h"
#include "shared/net/Messages.h"

#include <catch2/catch_test_macros.hpp>

#include <asio/connect.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>

#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vn = vo::net;
namespace vs = vo::server::net;
namespace va = vo::server::auth;
using namespace std::chrono_literals;

using vo::tests::Msg;
using vo::tests::TestClient;
using vo::tests::TestServer;

TEST_CASE("Server: handshake, ping, login and character select", "[server][net]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    c.send(vn::Ping{1234});
    const auto pong = c.expect<vn::Pong>();
    CHECK(pong.clientTimeMs == 1234);

    CHECK(c.login("server_test") == vn::LoginResult::Ok);
    c.send(vn::CharListReq{});
    CHECK(c.expect<vn::CharListRes>().characters.empty());

    // Pipelined requests are answered in order even though the account service is asynchronous.
    c.send(vn::CharCreateReq{"Birinci", 1, 1, 0, 0});
    c.send(vn::CharCreateReq{"İkinci", 2, 2, 1, 1});
    c.send(vn::CharListReq{});
    CHECK(c.expect<vn::CharCreateRes>().result == vn::CharCreateResult::Ok);
    CHECK(c.expect<vn::CharCreateRes>().result == vn::CharCreateResult::Ok);
    const auto list = c.expect<vn::CharListRes>();
    REQUIRE(list.characters.size() == 2);
    CHECK(list.characters[1].name == "İkinci");

    c.send(vn::EnterWorldReq{999});
    CHECK(c.expect<vn::EnterWorldRes>().result == vn::EnterWorldResult::NotFound);
    c.send(vn::EnterWorldReq{list.characters[0].charId});
    const auto enter = c.expect<vn::EnterWorldRes>();
    CHECK(enter.result == vn::EnterWorldResult::Ok);
    CHECK(enter.runSpeed == 5.5f);
    // PROTOCOL_ITEMS.md §2: INVENTORY right after ENTER_WORLD_RES (here: the warrior's kit).
    const auto inv = c.expect<vn::Inventory>();
    CHECK(inv.bagSize == 28);
    CHECK(inv.items.size() == 3);

    // In world: character-select opcodes are a state violation.
    c.send(vn::CharListReq{});
    CHECK(c.expectError() == vn::ErrorCode::BadState);
    CHECK(c.waitClosed());
}

TEST_CASE("Server: second LOGIN_REQ after login is BadState", "[server][net]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    CHECK(c.login("twice_user") == vn::LoginResult::Ok);
    c.send(vn::LoginReq{"twice_user", "password1"});
    CHECK(c.expectError() == vn::ErrorCode::BadState);
    CHECK(c.waitClosed());
}

TEST_CASE("Server: malformed payload is a ProtocolViolation", "[server][net]")
{
    TestServer server({});
    SECTION("truncated LOGIN_REQ")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.sendRawPayload(vn::Opcode::LoginReq, {5, 'a', 'b'});
        CHECK(c.expectError() == vn::ErrorCode::ProtocolViolation);
        CHECK(c.waitClosed());
    }
    SECTION("PING with trailing bytes")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.sendRawPayload(vn::Opcode::Ping, {1, 2, 3, 4, 5});
        CHECK(c.expectError() == vn::ErrorCode::ProtocolViolation);
        CHECK(c.waitClosed());
    }
    SECTION("unknown opcode")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        c.sendRawPayload(static_cast<vn::Opcode>(0x7777), {});
        CHECK(c.expectError() == vn::ErrorCode::BadState);
        CHECK(c.waitClosed());
    }
}

TEST_CASE("Server: HELLO_ACK rejections", "[server][net]")
{
    SECTION("server full")
    {
        vs::ServerSettings st;
        st.maxConnections = 1;
        TestServer server(st);
        TestClient first;
        REQUIRE(first.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        TestClient second;
        CHECK(second.connect(server.clientSettings()) == vn::ConnectResult::ServerFull);
        // The first connection keeps working.
        first.send(vn::Ping{1});
        CHECK(first.expect<vn::Pong>().clientTimeMs == 1);
    }
    SECTION("version mismatch")
    {
        TestServer server({});
        TestClient c;
        auto s = server.clientSettings();
        s.protocolVersion = 1; // a v1 client (before items) is refused
        CHECK(c.connect(s) == vn::ConnectResult::VersionMismatch);
    }
    SECTION("maintenance")
    {
        vs::ServerSettings st;
        st.maintenance = true;
        TestServer server(st);
        TestClient c;
        CHECK(c.connect(server.clientSettings()) == vn::ConnectResult::Maintenance);
    }
}

TEST_CASE("Server: handshake, login and idle timeouts", "[server][net]")
{
    vs::ServerSettings st;
    st.handshakeTimeout = 300ms;
    st.loginTimeout = 400ms;
    st.idleTimeout = 2000ms; // > one Argon2id hash in Debug builds

    SECTION("no HELLO: closed after the handshake timeout")
    {
        TestServer server(st);
        asio::io_context io;
        asio::ip::tcp::socket sock(io);
        sock.connect({asio::ip::make_address("127.0.0.1"), server.clientSettings().port});
        const auto start = std::chrono::steady_clock::now();
        std::uint8_t b = 0;
        std::error_code ec;
        asio::read(sock, asio::buffer(&b, 1), ec); // blocks until the server closes
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(ec); // EOF or reset: nothing is ever sent in plaintext except HELLO_ACK
        CHECK(elapsed >= 250ms);
        CHECK(elapsed < 5s);
    }
    SECTION("no LOGIN_REQ: ERROR(Timeout)")
    {
        TestServer server(st);
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        CHECK(c.expectError() == vn::ErrorCode::Timeout);
        CHECK(c.waitClosed());
    }
    SECTION("PING keeps an authenticated session alive; silence times out")
    {
        TestServer server(st);
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        CHECK(c.login("idle_user") == vn::LoginResult::Ok);
        // 5 pings 700 ms apart: 2.8 s in total, longer than the 2 s idle timeout.
        for (int i = 0; i < 5; ++i)
        {
            c.send(vn::Ping{static_cast<std::uint32_t>(i)});
            CHECK(c.expect<vn::Pong>().clientTimeMs == static_cast<std::uint32_t>(i));
            std::this_thread::sleep_for(700ms);
        }
        const auto silentFrom = std::chrono::steady_clock::now();
        CHECK(c.expectError() == vn::ErrorCode::Timeout);
        CHECK(std::chrono::steady_clock::now() - silentFrom >= 1000ms);
        CHECK(c.waitClosed());
    }
}

TEST_CASE("Server: duplicate login kicks the old session", "[server][net]")
{
    TestServer server({});
    TestClient a;
    REQUIRE(a.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(a.login("dup_user") == vn::LoginResult::Ok);
    TestClient b;
    REQUIRE(b.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(b.login("DUP_USER") == vn::LoginResult::Ok);
    CHECK(a.expectError() == vn::ErrorCode::KickedDuplicateLogin);
    CHECK(a.waitClosed());
    b.send(vn::CharListReq{});
    CHECK(b.expect<vn::CharListRes>().characters.empty());
}

TEST_CASE("Server: failed logins are rate limited per IP", "[server][net]")
{
    TestServer server({}, false);
    REQUIRE(server.accounts().createAccount("limited", "password1").has_value());
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    for (int i = 0; i < 5; ++i)
        CHECK(c.login("limited", "wrongpass") == vn::LoginResult::InvalidCredentials);
    CHECK(c.login("limited", "password1") == vn::LoginResult::TooManyAttempts);
    TestClient other; // same IP, new connection
    REQUIRE(other.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    CHECK(other.login("limited", "password1") == vn::LoginResult::TooManyAttempts);
}

TEST_CASE("Server: shutdown sends ERROR(ServerShutdown) to every session", "[server][net]")
{
    TestServer server({});
    TestClient a;
    TestClient b;
    REQUIRE(a.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(b.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(b.login("shutdown_user") == vn::LoginResult::Ok);
    server.shutdown();
    CHECK(a.expectError() == vn::ErrorCode::ServerShutdown);
    CHECK(b.expectError() == vn::ErrorCode::ServerShutdown);
    CHECK(a.waitClosed());
    CHECK(b.waitClosed());
}

// ---- World (M6): the session <-> world hand-off over real sockets ----

TEST_CASE("Server: two players in the world see each other spawn, move and leave", "[server][net][world]")
{
    TestServer server({});
    TestClient a;
    TestClient b;
    REQUIRE(a.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(b.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto ea = a.enterNew("world_a", "Dunyaci");
    // New characters start next to their race spawn (test terrain: Aydor at 100, 100).
    CHECK(std::hypot(ea.x - 100.0f, ea.z - 100.0f) <= 2.0f + 1e-3f);
    CHECK(ea.walkSpeed == 1.4f);
    CHECK(ea.runSpeed == 5.5f);
    CHECK(ea.zoneId == 1);
    const auto eb = b.enterNew("world_b", "Gezgin");
    CHECK(eb.entityId != ea.entityId);
    // B: ENTER_WORLD_RES is followed by ENTITY_SPAWN of A (expectNew already consumed the former).
    const auto spawnA = b.expect<vn::EntitySpawn>();
    CHECK(spawnA.entityId == ea.entityId);
    CHECK(spawnA.name == "Dunyaci");
    CHECK(spawnA.x == ea.x);
    const auto spawnB = a.waitFor<vn::EntitySpawn>([&](const vn::EntitySpawn& s) { return s.entityId == eb.entityId; });
    REQUIRE(spawnB);
    CHECK(spawnB->name == "Gezgin");

    std::this_thread::sleep_for(300ms);
    vn::MoveReq m;
    m.seq = 1;
    m.flags = vn::kMoveFlagMoving;
    m.x = eb.x + 0.5f;
    m.y = 0.0f;
    m.z = eb.z;
    m.yaw = 7.0f;
    b.send(m);
    const auto moved = a.waitFor<vn::EntityMove>([&](const vn::EntityMove& e) { return e.entityId == eb.entityId; });
    REQUIRE(moved);
    CHECK(moved->x == m.x);
    CHECK((moved->flags & vn::kMoveFlagMoving) != 0);
    CHECK(moved->yaw < 3.1416f); // normalised

    b.send(vn::LeaveWorldReq{});
    CHECK(b.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    const auto gone = a.waitFor<vn::EntityDespawn>([&](const vn::EntityDespawn& d) { return d.entityId == eb.entityId; });
    CHECK(gone.has_value());
    b.send(vn::CharListReq{}); // back in character select
    CHECK(b.expectSkippingWorld<vn::CharListRes>().characters.size() == 1);
}

TEST_CASE("Server: malformed or implausible MOVE_REQ is a ProtocolViolation", "[server][net][world]")
{
    TestServer server({});
    TestClient watcher;
    REQUIRE(watcher.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto ew = watcher.enterNew("move_watch", "Bekci");

    SECTION("NaN")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        const auto e = c.enterNew("move_nan", "Nanci");
        REQUIRE(watcher.waitFor<vn::EntitySpawn>([&](const vn::EntitySpawn& s) { return s.entityId == e.entityId; }));
        vn::MoveReq m;
        m.seq = 1;
        m.x = std::numeric_limits<float>::quiet_NaN();
        c.send(m);
        CHECK(c.expectSkippingWorld<vn::Error>().errorCode == vn::ErrorCode::ProtocolViolation);
        CHECK(c.waitClosed());
        // Disconnecting despawns the player for everyone who saw it.
        CHECK(watcher.waitFor<vn::EntityDespawn>([&](const vn::EntityDespawn& d) { return d.entityId == e.entityId; }));
    }
    SECTION("seq not increasing")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        const auto e = c.enterNew("move_seq", "Sirali");
        vn::MoveReq m;
        m.seq = 5;
        m.x = e.x;
        m.z = e.z;
        c.send(m);
        c.send(m); // same seq again
        CHECK(c.expectSkippingWorld<vn::Error>().errorCode == vn::ErrorCode::ProtocolViolation);
        CHECK(c.waitClosed());
    }
    SECTION("more than 30 per second")
    {
        TestClient c;
        REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
        const auto e = c.enterNew("move_flood", "Selci");
        vn::MoveReq m;
        m.x = e.x;
        m.z = e.z;
        for (std::uint32_t i = 1; i <= 31; ++i)
        {
            m.seq = i;
            c.send(m);
        }
        CHECK(c.expectSkippingWorld<vn::Error>().errorCode == vn::ErrorCode::Flood);
        CHECK(c.waitClosed());
    }
    (void)ew;
}

TEST_CASE("Server: duplicate login kicks the in-world session; the new one enters at the live position",
          "[server][net][world]")
{
    TestServer server({});
    TestClient first;
    REQUIRE(first.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = first.enterNew("dup_world", "Ikizler");
    std::this_thread::sleep_for(600ms); // enough movement allowance for 1 m
    vn::MoveReq m;
    m.seq = 1;
    m.flags = vn::kMoveFlagMoving;
    m.x = e.x + 1.0f;
    m.z = e.z;
    first.send(m);
    std::this_thread::sleep_for(200ms); // processed by the world

    TestClient second;
    REQUIRE(second.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    REQUIRE(second.login("dup_world") == vn::LoginResult::Ok);
    CHECK(first.expectSkippingWorld<vn::Error>().errorCode == vn::ErrorCode::KickedDuplicateLogin);
    CHECK(first.waitClosed());
    second.send(vn::CharListReq{});
    const auto list = second.expect<vn::CharListRes>();
    REQUIRE(list.characters.size() == 1);
    second.send(vn::EnterWorldReq{list.characters[0].charId});
    const auto again = second.expect<vn::EnterWorldRes>();
    REQUIRE(again.result == vn::EnterWorldResult::Ok);
    CHECK(again.x == m.x);
    CHECK(again.z == m.z);
    // Exactly one entity for the character: re-entering again after a leave works as well.
    second.send(vn::LeaveWorldReq{});
    CHECK(second.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    second.send(vn::EnterWorldReq{list.characters[0].charId});
    const auto third = second.expectSkippingWorld<vn::EnterWorldRes>();
    CHECK(third.x == m.x);
}

// ---- M7: leave-world race (PROTOCOL §5.4 "Leaving") ----

TEST_CASE("Server: MOVE_REQ in flight when leaving the world is dropped, later ones are BadState",
          "[server][net][world][leave]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("leave_race", "Ayrilan");
    std::this_thread::sleep_for(300ms); // movement allowance for the small steps below
    vn::MoveReq m;
    m.flags = vn::kMoveFlagMoving;
    m.z = e.z;
    std::uint32_t seq = 0;
    auto move = [&](float dx) {
        m.seq = ++seq;
        m.x = e.x + dx;
        c.send(m);
    };
    move(0.1f);
    // LEAVE_WORLD_REQ with moves pipelined right behind it, as a client that keeps walking would.
    c.send(vn::LeaveWorldReq{});
    move(0.2f);
    move(0.3f);
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    // Still within the 2 s grace window in CharSelect: dropped silently as well.
    move(0.4f);
    c.send(vn::CharListReq{});
    const auto list = c.expectSkippingWorld<vn::CharListRes>();
    REQUIRE(list.characters.size() == 1);
    CHECK_FALSE(c.closed());
    // The character re-enters (fresh stay) and leaves again.
    c.send(vn::EnterWorldReq{list.characters[0].charId});
    const auto again = c.expectSkippingWorld<vn::EnterWorldRes>();
    REQUIRE(again.result == vn::EnterWorldResult::Ok);
    c.send(vn::LeaveWorldReq{});
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    // After the grace window a MOVE_REQ in CharSelect is a state violation again.
    std::this_thread::sleep_for(vs::kLeaveGrace + 200ms);
    c.send(vn::Ping{7});
    CHECK(c.expectSkippingWorld<vn::Pong>().clientTimeMs == 7);
    m.seq = 1;
    c.send(m);
    CHECK(c.expectSkippingWorld<vn::Error>().errorCode == vn::ErrorCode::BadState);
    CHECK(c.waitClosed());
}

TEST_CASE("Server: MOVE_REQ after LOGOUT in the world does not produce an ERROR", "[server][net][world][leave]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("logout_race", "Cikan");
    vn::MoveReq m;
    m.seq = 1;
    m.x = e.x;
    m.z = e.z;
    c.send(vn::Logout{});
    c.send(m);
    m.seq = 2;
    c.send(m);
    // The server saves and closes; nothing but world traffic arrives before the close.
    for (;;)
    {
        auto msg = c.next(5s);
        if (!msg)
            break;
        INFO("got opcode " << vn::opcodeName(static_cast<vn::Opcode>(msg->opcode)));
        CHECK(TestClient::isWorldOpcode(msg->opcode));
    }
    CHECK(c.waitClosed());
}

TEST_CASE("Server: grace-window MOVE_REQ still count against the flood cap", "[server][net][world][leave]")
{
    TestServer server({});
    TestClient c;
    REQUIRE(c.connect(server.clientSettings()) == vn::ConnectResult::Ok);
    const auto e = c.enterNew("leave_flood", "Selaci");
    c.send(vn::LeaveWorldReq{});
    CHECK(c.expectSkippingWorld<vn::LeaveWorldRes>().result == vn::kLeaveWorldOk);
    vn::MoveReq m;
    m.x = e.x;
    m.z = e.z;
    for (std::uint32_t i = 1; i <= 31; ++i)
    {
        m.seq = i;
        c.send(m);
    }
    CHECK(c.expectSkippingWorld<vn::Error>().errorCode == vn::ErrorCode::Flood);
    CHECK(c.waitClosed());
}

TEST_CASE("GameClient: no MOVE_REQ leaves the client between LEAVE_WORLD_REQ and the next ENTER_WORLD_RES",
          "[client][net][world][leave]")
{
    TestServer server({});
    vo::client::net::GameClient::Settings gs;
    gs.host = "127.0.0.1";
    gs.port = server.clientSettings().port;
    gs.serverKey = server.clientSettings().serverPublicKey;
    vo::client::net::GameClient client(gs);
    std::deque<vo::client::net::NetEvent> events;
    auto waitEvent = [&](const std::function<bool(const vo::client::net::NetEvent&)>& pred) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            client.poll();
            while (auto e = client.nextEvent())
            {
                if (pred(*e))
                    return std::optional<vo::client::net::NetEvent>(std::move(*e));
                events.push_back(std::move(*e));
            }
            std::this_thread::sleep_for(2ms);
        }
        return std::optional<vo::client::net::NetEvent>();
    };
    auto waitOp = [&](vn::Opcode op) {
        return waitEvent([op](const vo::client::net::NetEvent& e) {
            return e.is(op) || e.type == vo::client::net::NetEventType::Disconnected;
        });
    };
    REQUIRE(client.connect());
    REQUIRE(waitEvent([](const auto& e) { return e.type == vo::client::net::NetEventType::Connected; }));
    REQUIRE(client.send(vn::LoginReq{"gc_leave", "password1"}));
    REQUIRE(waitOp(vn::Opcode::LoginRes));
    REQUIRE(client.send(vn::CharCreateReq{"Istemci", 2, 1, 0, 0}));
    auto created = waitOp(vn::Opcode::CharCreateRes);
    REQUIRE(created);
    vn::CharCreateRes cres;
    REQUIRE(vn::decodeMessage(created->payload, cres));
    REQUIRE(client.send(vn::EnterWorldReq{cres.character.charId}));
    auto entered = waitOp(vn::Opcode::EnterWorldRes);
    REQUIRE(entered);
    REQUIRE(entered->is(vn::Opcode::EnterWorldRes));
    vn::EnterWorldRes enter;
    REQUIRE(vn::decodeMessage(entered->payload, enter));

    vn::MoveReq m;
    m.seq = 1;
    m.x = enter.x;
    m.z = enter.z;
    CHECK(client.send(m));
    CHECK_FALSE(client.movesBlocked());
    REQUIRE(client.request(vn::LeaveWorldReq{}, vn::Opcode::LeaveWorldRes));
    CHECK(client.movesBlocked());
    m.seq = 2;
    CHECK_FALSE(client.send(m)); // refused locally
    CHECK(client.movesSuppressed() == 1);
    auto left = waitOp(vn::Opcode::LeaveWorldRes);
    REQUIRE(left);
    REQUIRE(left->is(vn::Opcode::LeaveWorldRes));
    CHECK(client.movesBlocked()); // still blocked in character select
    CHECK_FALSE(client.send(m));

    REQUIRE(client.send(vn::EnterWorldReq{cres.character.charId}));
    auto again = waitOp(vn::Opcode::EnterWorldRes);
    REQUIRE(again);
    REQUIRE(again->is(vn::Opcode::EnterWorldRes));
    CHECK_FALSE(client.movesBlocked());
    m.seq = 1;
    CHECK(client.send(m));
    // The session survived all of it.
    REQUIRE(client.send(vn::Ping{5}));
    client.poll();
    CHECK(client.isConnected());
}
