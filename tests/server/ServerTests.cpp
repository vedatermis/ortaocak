// In-process server tests: a real Server on an ephemeral 127.0.0.1 port (own io thread) and
// ClientSession clients driven from the test thread. Covers what the botclient scenarios cannot
// easily reach: connection cap, version mismatch, maintenance, timeouts, duplicate-login kicks,
// shutdown, malformed payloads and the outbound queue cap.

#include "server/auth/MemoryAccountService.h"
#include "server/net/Server.h"
#include "shared/net/ClientSession.h"
#include "shared/net/Connection.h"
#include "shared/net/Messages.h"

#include <catch2/catch_test_macros.hpp>

#include <asio/connect.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>

#include <chrono>
#include <deque>
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

namespace {

class TestServer
{
public:
    explicit TestServer(vs::ServerSettings settings, bool autoRegister = true)
        : m_accounts({autoRegister}), m_work(asio::make_work_guard(m_io))
    {
        REQUIRE(vn::initCrypto());
        settings.listenAddress = "127.0.0.1";
        settings.listenPort = 0;
        auto identity = vn::ServerIdentity::generate();
        m_publicKey = identity.publicKey();
        m_server = std::make_unique<vs::Server>(m_io, settings, std::move(identity), m_accounts);
        std::string error;
        REQUIRE(m_server->start(error));
        m_thread = std::thread([this] { m_io.run(); });
    }

    ~TestServer()
    {
        std::promise<void> done;
        asio::post(m_io, [&] {
            m_server->shutdown();
            done.set_value();
        });
        done.get_future().wait();
        m_work.reset();
        // Sessions close gracefully (at most the connection close timeout); then run() returns.
        m_thread.join();
        m_accounts.stop();
        m_server.reset();
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    void shutdown()
    {
        asio::post(m_io, [this] { m_server->shutdown(); });
    }

    [[nodiscard]] std::size_t sessionCount()
    {
        std::promise<std::size_t> p;
        asio::post(m_io, [&] { p.set_value(m_server->sessionCount()); });
        return p.get_future().get();
    }

    vn::ClientSettings clientSettings() const
    {
        vn::ClientSettings s;
        s.host = "127.0.0.1";
        s.port = m_server->port();
        s.serverPublicKey = m_publicKey;
        s.handshakeTimeout = 5s;
        return s;
    }

    va::MemoryAccountService& accounts() { return m_accounts; }

private:
    asio::io_context m_io;
    va::MemoryAccountService m_accounts;
    asio::executor_work_guard<asio::io_context::executor_type> m_work;
    std::unique_ptr<vs::Server> m_server;
    vn::SignPublicKey m_publicKey{};
    std::thread m_thread;
};

struct Msg
{
    std::uint16_t opcode = 0;
    std::vector<std::uint8_t> payload;
};

class TestClient
{
public:
    TestClient() : m_session(m_io)
    {
        m_session.setMessageHandler([this](std::uint16_t op, std::span<const std::uint8_t> p) {
            m_inbox.push_back({op, std::vector<std::uint8_t>(p.begin(), p.end())});
        });
        m_session.setClosedHandler([this](vn::CloseReason) { m_closed = true; });
    }

    vn::ConnectResult connect(const vn::ClientSettings& s)
    {
        std::optional<vn::ConnectResult> result;
        REQUIRE(m_session.connect(s, [&](vn::ConnectResult r, const std::string&) { result = r; }));
        REQUIRE(pump([&] { return result.has_value(); }, 10s));
        return *result;
    }

    template <class T>
    void send(const T& msg)
    {
        REQUIRE(m_session.sendMessage(msg));
    }

    void sendRawPayload(vn::Opcode op, std::vector<std::uint8_t> payload)
    {
        REQUIRE(m_session.send(static_cast<std::uint16_t>(op), payload));
    }

    std::optional<Msg> next(std::chrono::milliseconds timeout = 10s)
    {
        if (!pump([&] { return !m_inbox.empty() || m_closed; }, timeout) || m_inbox.empty())
            return std::nullopt;
        Msg m = std::move(m_inbox.front());
        m_inbox.pop_front();
        return m;
    }

    template <class T>
    T expect()
    {
        auto m = next();
        REQUIRE(m.has_value());
        INFO("got opcode " << vn::opcodeName(static_cast<vn::Opcode>(m->opcode)));
        REQUIRE(m->opcode == static_cast<std::uint16_t>(T::kOpcode));
        T out;
        REQUIRE(vn::decodeMessage(m->payload, out));
        return out;
    }

    vn::ErrorCode expectError()
    {
        return expect<vn::Error>().errorCode;
    }

    bool waitClosed(std::chrono::milliseconds timeout = 10s)
    {
        return pump([&] { return m_closed; }, timeout);
    }

    vn::LoginResult login(const std::string& user, const std::string& pw = "password1")
    {
        send(vn::LoginReq{user, pw});
        return expect<vn::LoginRes>().result;
    }

    bool closed() const { return m_closed; }

private:
    bool pump(const std::function<bool()>& done, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            if (m_io.stopped())
                m_io.restart();
            if (m_io.run_one_for(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)) == 0)
                std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    asio::io_context m_io;
    vn::ClientSession m_session;
    std::deque<Msg> m_inbox;
    bool m_closed = false;
};

} // namespace

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
        s.protocolVersion = 2;
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

TEST_CASE("Connection closes when the outbound queue exceeds 256 KB", "[net][connection]")
{
    REQUIRE(vn::initCrypto());
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket clientSock(io);
    clientSock.connect(acceptor.local_endpoint());
    asio::ip::tcp::socket serverSock = acceptor.accept();

    auto conn = vn::Connection::create(std::move(serverSock));
    std::optional<vn::CloseReason> reason;
    conn->start([](std::uint16_t, std::span<const std::uint8_t>) {}, [&](vn::CloseReason r) { reason = r; });
    auto kx = vn::generateKxKeyPair();
    auto peer = vn::generateKxKeyPair();
    vn::SessionKeys keys;
    REQUIRE(vn::deriveServerSessionKeys(kx, peer.publicKey, keys));
    conn->enableEncryption(std::move(keys));

    // Without running the io_context nothing is written, so the queue only grows.
    const std::vector<std::uint8_t> payload(8000, 0x42);
    std::size_t sent = 0;
    while (conn->send(0x0011, payload))
        ++sent;
    CHECK(sent * (8000 + 2 + 2 + 16) <= vn::kMaxOutboundQueueBytes);
    CHECK(sent >= 30);
    CHECK_FALSE(conn->isOpen());
    io.run_for(1s);
    REQUIRE(reason.has_value());
    CHECK(*reason == vn::CloseReason::OutboundOverflow);
}
