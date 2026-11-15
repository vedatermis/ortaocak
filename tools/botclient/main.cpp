// botclient - headless test client for end-to-end server scenarios, built on vo::net::ClientSession.
//
//   botclient --scenario <name> [--host H] [--port P] [--server-key HEX] [--config client.toml]
//             [--user U] [--password P] [--count N] [--expect RESULT] [--timeout-ms N]
//
// Defaults for host, port and server key come from config/client.toml (server.host, server.port,
// server_public_key). Prints one "PASS ..." or "FAIL ..." summary line; exit code 0 only on PASS.
// Exit codes: 0 pass, 1 scenario failed, 2 bad arguments/config.

#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"
#include "shared/net/ClientSession.h"
#include "shared/net/Crypto.h"
#include "shared/net/Messages.h"
#include "shared/net/Opcodes.h"

#include <asio/io_context.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vo::tools::botclient {

namespace vn = vo::net;
using Ms = std::chrono::milliseconds;

constexpr int kExitPass = 0;
constexpr int kExitFail = 1;
constexpr int kExitUsage = 2;

struct Options
{
    std::string scenario;
    std::optional<std::string> host;
    std::optional<std::uint16_t> port;
    std::optional<std::string> serverKeyHex;
    std::optional<std::string> configPath;
    std::optional<std::string> user;
    std::string password = "botpassword";
    int count = 5;
    std::string expect = "ok";
    Ms timeout{10000};
    bool help = false;
};

void printUsage()
{
    std::printf(
        "usage: botclient --scenario <name> [options]\n"
        "scenarios:\n"
        "  ping       handshake + N pings, prints RTT\n"
        "  login      LOGIN_REQ, expects --expect result (default ok)\n"
        "  chars      login, list, create, list, delete, list (+ deleted name stays reserved)\n"
        "  world      login, create, enter world, move, ping, leave world, logout\n"
        "  badstate   CHAR_LIST_REQ before login, expects ERROR(BadState) and close\n"
        "  garbage    corrupted encrypted frame, expects a silent close\n"
        "  wrongkey   pins a wrong server key, expects local verification failure\n"
        "  waitshutdown  login, then wait for ERROR(ServerShutdown) and close\n"
        "options:\n"
        "  --host <h>          server host           (default: client.toml server.host)\n"
        "  --port <n>          server port           (default: client.toml server.port)\n"
        "  --server-key <hex>  pinned server key     (default: client.toml server_public_key)\n"
        "  --config <path>     client.toml to read defaults from (default: search config/client.toml)\n"
        "  --user <name>       username (default: random bot_NNNNNN)\n"
        "  --password <pw>     password (default: botpassword)\n"
        "  --count <n>         pings for 'ping' (default 5)\n"
        "  --expect <r>        login: ok|invalid_credentials|banned|too_many_attempts|server_error\n"
        "  --timeout-ms <n>    per-step timeout (default 10000)\n");
}

template <class T>
bool parseNumber(std::string_view v, T& out)
{
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc{} && ptr == v.data() + v.size();
}

bool parseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--help" || a == "-h")
            o.help = true;
        else if (a == "--scenario" && hasValue)
            o.scenario = argv[++i];
        else if (a == "--host" && hasValue)
            o.host = argv[++i];
        else if (a == "--port" && hasValue)
        {
            std::uint16_t p = 0;
            if (!parseNumber(std::string_view(argv[++i]), p) || p == 0)
                return false;
            o.port = p;
        }
        else if (a == "--server-key" && hasValue)
            o.serverKeyHex = argv[++i];
        else if (a == "--config" && hasValue)
            o.configPath = argv[++i];
        else if (a == "--user" && hasValue)
            o.user = argv[++i];
        else if (a == "--password" && hasValue)
            o.password = argv[++i];
        else if (a == "--count" && hasValue)
        {
            if (!parseNumber(std::string_view(argv[++i]), o.count) || o.count <= 0)
                return false;
        }
        else if (a == "--expect" && hasValue)
            o.expect = argv[++i];
        else if (a == "--timeout-ms" && hasValue)
        {
            int ms = 0;
            if (!parseNumber(std::string_view(argv[++i]), ms) || ms <= 0)
                return false;
            o.timeout = Ms(ms);
        }
        else
        {
            std::fprintf(stderr, "unknown or incomplete argument '%.*s'\n", static_cast<int>(a.size()), a.data());
            return false;
        }
    }
    return true;
}

std::optional<vn::LoginResult> parseLoginResult(std::string_view s)
{
    if (s == "ok")
        return vn::LoginResult::Ok;
    if (s == "invalid_credentials")
        return vn::LoginResult::InvalidCredentials;
    if (s == "banned")
        return vn::LoginResult::Banned;
    if (s == "too_many_attempts")
        return vn::LoginResult::TooManyAttempts;
    if (s == "server_error")
        return vn::LoginResult::ServerError;
    return std::nullopt;
}

std::uint32_t nowMs()
{
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Thrown by scenario steps; caught in runScenario and turned into a FAIL line.
struct Failure
{
    std::string reason;
};

[[noreturn]] void fail(std::string reason)
{
    throw Failure{std::move(reason)};
}

struct Received
{
    std::uint16_t opcode = 0;
    std::vector<std::uint8_t> payload;
};

// Drives one ClientSession synchronously by running the io_context until a condition holds.
class Bot
{
public:
    explicit Bot(Ms timeout) : m_session(m_io), m_timeout(timeout)
    {
        m_session.setMessageHandler([this](std::uint16_t op, std::span<const std::uint8_t> payload) {
            m_inbox.push_back({op, std::vector<std::uint8_t>(payload.begin(), payload.end())});
        });
        m_session.setClosedHandler([this](vn::CloseReason r) {
            m_closed = true;
            m_closeReason = r;
        });
    }

    vn::ConnectResult connect(const vn::ClientSettings& settings, std::string& detail)
    {
        std::optional<vn::ConnectResult> result;
        m_session.connect(settings, [&](vn::ConnectResult r, const std::string& d) {
            result = r;
            detail = d;
        });
        if (!pump([&] { return result.has_value(); }, settings.handshakeTimeout + Ms(2000)))
            fail("connect callback never fired");
        return *result;
    }

    void connectOrFail(const vn::ClientSettings& settings)
    {
        std::string detail;
        const auto r = connect(settings, detail);
        if (r != vn::ConnectResult::Ok)
            fail(std::format("connect failed: {} ({})", vn::connectResultName(r), detail));
    }

    template <class T>
    void send(const T& msg)
    {
        if (!m_session.sendMessage(msg))
            fail(std::format("cannot send {}", vn::opcodeName(T::kOpcode)));
    }

    bool sendRaw(std::vector<std::uint8_t> bytes) { return m_session.sendRawForTesting(std::move(bytes)); }

    // Next message of type T; fails on timeout, close, ERROR or any other opcode.
    template <class T>
    T expect()
    {
        const Received m = next(std::format("waiting for {}", vn::opcodeName(T::kOpcode)));
        if (m.opcode == static_cast<std::uint16_t>(vn::Opcode::Error))
        {
            vn::Error err;
            (void)vn::decodeMessage(m.payload, err);
            fail(std::format("got ERROR({} '{}') while waiting for {}", vn::errorCodeName(err.errorCode),
                             err.debugMessage, vn::opcodeName(T::kOpcode)));
        }
        if (m.opcode != static_cast<std::uint16_t>(T::kOpcode))
            fail(std::format("got {} while waiting for {}", vn::opcodeName(static_cast<vn::Opcode>(m.opcode)),
                             vn::opcodeName(T::kOpcode)));
        T out;
        if (!vn::decodeMessage(m.payload, out))
            fail(std::format("malformed {}", vn::opcodeName(T::kOpcode)));
        return out;
    }

    Received next(const std::string& what)
    {
        if (!pump([&] { return !m_inbox.empty() || m_closed; }, m_timeout))
            fail("timeout " + what);
        if (m_inbox.empty())
            fail(std::format("connection closed ({}) {}", vn::closeReasonName(m_closeReason), what));
        Received m = std::move(m_inbox.front());
        m_inbox.pop_front();
        return m;
    }

    // Waits for the server to close the connection; fails if any message arrives first and
    // `allowMessages` is false.
    void expectClosed(bool allowMessages, Ms timeout)
    {
        if (!pump([&] { return m_closed || (!allowMessages && !m_inbox.empty()); }, timeout))
            fail("server did not close the connection");
        if (!m_closed)
            fail(std::format("unexpected {} before close",
                             vn::opcodeName(static_cast<vn::Opcode>(m_inbox.front().opcode))));
    }

    vn::LoginRes login(const std::string& user, const std::string& password)
    {
        send(vn::LoginReq{user, password});
        return expect<vn::LoginRes>();
    }

    [[nodiscard]] Ms timeout() const noexcept { return m_timeout; }

private:
    bool pump(const std::function<bool()>& done, Ms timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            if (m_io.stopped())
                m_io.restart();
            if (m_io.run_one_for(std::chrono::duration_cast<Ms>(deadline - now)) == 0)
                std::this_thread::sleep_for(Ms(5)); // no work at all: avoid spinning
        }
        return true;
    }

    asio::io_context m_io;
    vn::ClientSession m_session;
    Ms m_timeout;
    std::deque<Received> m_inbox;
    bool m_closed = false;
    vn::CloseReason m_closeReason = vn::CloseReason::LocalClose;
};

std::string randomDigits(int n)
{
    std::random_device rd;
    std::uniform_int_distribution<int> digit(0, 9);
    std::string s;
    for (int i = 0; i < n; ++i)
        s.push_back(static_cast<char>('0' + digit(rd)));
    return s;
}

std::string randomCharacterName()
{
    std::random_device rd;
    std::uniform_int_distribution<int> letter(0, 25);
    std::string s = "Bot";
    for (int i = 0; i < 6; ++i)
        s.push_back(static_cast<char>('a' + letter(rd)));
    return s;
}

// ---- Scenarios ----

struct Context
{
    vn::ClientSettings settings;
    Options opts;
    std::string user;
};

std::string scenarioPing(const Context& c)
{
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    double total = 0.0;
    double worst = 0.0;
    for (int i = 0; i < c.opts.count; ++i)
    {
        const auto sent = std::chrono::steady_clock::now();
        const std::uint32_t stamp = nowMs() + static_cast<std::uint32_t>(i);
        bot.send(vn::Ping{stamp});
        const auto pong = bot.expect<vn::Pong>();
        const double rtt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sent).count();
        if (pong.clientTimeMs != stamp)
            fail(std::format("PONG echoed {} instead of {}", pong.clientTimeMs, stamp));
        std::printf("ping %d: rtt %.2f ms (server time %u ms)\n", i + 1, rtt, pong.serverTimeMs);
        total += rtt;
        worst = std::max(worst, rtt);
    }
    return std::format("{} pings, avg rtt {:.2f} ms, max {:.2f} ms", c.opts.count, total / c.opts.count, worst);
}

std::string scenarioLogin(const Context& c)
{
    const auto expected = parseLoginResult(c.opts.expect);
    if (!expected)
        fail("unknown --expect value '" + c.opts.expect + "'");
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    const auto res = bot.login(c.user, c.opts.password);
    if (res.result != *expected)
        fail(std::format("LOGIN_RES result {} (expected {})", static_cast<int>(res.result), c.opts.expect));
    if (res.result == vn::LoginResult::Ok && res.accountId == 0)
        fail("LOGIN_RES ok with account id 0");
    if (res.result != vn::LoginResult::Ok && res.accountId != 0)
        fail("LOGIN_RES failure with non-zero account id");
    return std::format("user '{}' -> {} (account {})", c.user, c.opts.expect, res.accountId);
}

void loginOk(Bot& bot, const Context& c)
{
    const auto res = bot.login(c.user, c.opts.password);
    if (res.result != vn::LoginResult::Ok)
        fail(std::format("login failed with result {}", static_cast<int>(res.result)));
}

std::vector<vn::CharSummary> listChars(Bot& bot)
{
    bot.send(vn::CharListReq{});
    return bot.expect<vn::CharListRes>().characters;
}

vn::CharCreateRes createChar(Bot& bot, const std::string& name, std::uint8_t race = 1, std::uint8_t cls = 1)
{
    vn::CharCreateReq req;
    req.name = name;
    req.race = race;
    req.charClass = cls;
    req.face = 2;
    req.hair = 3;
    bot.send(req);
    return bot.expect<vn::CharCreateRes>();
}

std::string scenarioChars(const Context& c)
{
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginOk(bot, c);

    const auto before = listChars(bot);
    const std::string name = randomCharacterName();
    const auto created = createChar(bot, name, 2, 3);
    if (created.result != vn::CharCreateResult::Ok)
        fail(std::format("CHAR_CREATE_RES result {}", static_cast<int>(created.result)));
    const auto& ch = created.character;
    if (ch.name != name || ch.race != 2 || ch.charClass != 3 || ch.face != 2 || ch.hair != 3 || ch.level != 1 ||
        ch.zoneId != 1 || ch.charId == 0)
        fail("created character summary does not match the request");

    const auto afterCreate = listChars(bot);
    if (afterCreate.size() != before.size() + 1)
        fail(std::format("list has {} characters after create (expected {})", afterCreate.size(), before.size() + 1));
    if (std::find(afterCreate.begin(), afterCreate.end(), ch) == afterCreate.end())
        fail("created character missing from the list");

    bot.send(vn::CharDeleteReq{ch.charId});
    const auto del = bot.expect<vn::CharDeleteRes>();
    if (del.result != vn::CharDeleteResult::Ok || del.charId != ch.charId)
        fail(std::format("CHAR_DELETE_RES result {} char {}", static_cast<int>(del.result), del.charId));

    const auto afterDelete = listChars(bot);
    if (afterDelete.size() != before.size())
        fail(std::format("list has {} characters after delete (expected {})", afterDelete.size(), before.size()));

    // Soft delete keeps the name reserved (case-insensitive).
    std::string upper = name;
    for (auto& ch2 : upper)
        ch2 = static_cast<char>(ch2 >= 'a' && ch2 <= 'z' && ch2 != 'i' ? ch2 - 'a' + 'A' : ch2);
    const auto again = createChar(bot, upper);
    if (again.result != vn::CharCreateResult::NameTaken)
        fail(std::format("re-creating a deleted name gave result {} (expected NameTaken)",
                         static_cast<int>(again.result)));
    return std::format("created, listed and deleted '{}' (id {}); name stays reserved", name, ch.charId);
}

std::string scenarioWorld(const Context& c)
{
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginOk(bot, c);
    const auto created = createChar(bot, randomCharacterName());
    if (created.result != vn::CharCreateResult::Ok)
        fail(std::format("CHAR_CREATE_RES result {}", static_cast<int>(created.result)));

    bot.send(vn::EnterWorldReq{created.character.charId});
    const auto enter = bot.expect<vn::EnterWorldRes>();
    if (enter.result != vn::EnterWorldResult::Ok || enter.entityId == 0 || enter.zoneId != 1 || enter.x != 256.0f ||
        enter.y != 0.0f || enter.z != 256.0f || enter.walkSpeed != 2.0f || enter.runSpeed != 5.5f)
        fail("unexpected ENTER_WORLD_RES");

    vn::MoveReq move;
    move.seq = 1;
    move.flags = vn::kMoveFlagMoving;
    move.x = 257.0f;
    move.z = 256.0f;
    bot.send(move);
    bot.send(vn::Ping{42});
    if (bot.expect<vn::Pong>().clientTimeMs != 42)
        fail("bad PONG in world");

    bot.send(vn::LeaveWorldReq{});
    if (bot.expect<vn::LeaveWorldRes>().result != vn::kLeaveWorldOk)
        fail("LEAVE_WORLD_RES not ok");
    if (listChars(bot).empty())
        fail("character list empty after leaving the world");

    bot.send(vn::Logout{});
    bot.expectClosed(false, bot.timeout());
    return std::format("entered world as entity {}, left and logged out", enter.entityId);
}

std::string scenarioBadState(const Context& c)
{
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    bot.send(vn::CharListReq{});
    const Received m = bot.next("waiting for ERROR(BadState)");
    vn::Error err;
    if (m.opcode != static_cast<std::uint16_t>(vn::Opcode::Error) || !vn::decodeMessage(m.payload, err))
        fail(std::format("got {} instead of ERROR", vn::opcodeName(static_cast<vn::Opcode>(m.opcode))));
    if (err.errorCode != vn::ErrorCode::BadState)
        fail(std::format("ERROR code {} instead of BadState", vn::errorCodeName(err.errorCode)));
    bot.expectClosed(false, bot.timeout());
    return std::format("ERROR(BadState '{}') then close", err.debugMessage);
}

std::string scenarioGarbage(const Context& c)
{
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    // A well-formed encrypted frame header (ct_length 32) followed by bytes that cannot authenticate.
    std::vector<std::uint8_t> frame = {32, 0};
    std::random_device rd;
    for (int i = 0; i < 32; ++i)
        frame.push_back(static_cast<std::uint8_t>(rd() & 0xFF));
    if (!bot.sendRaw(std::move(frame)))
        fail("cannot send raw bytes");
    bot.expectClosed(false, bot.timeout());
    return "server closed silently";
}

std::string scenarioWrongKey(const Context& c)
{
    vn::ClientSettings s = c.settings;
    s.serverPublicKey[0] ^= 0x01; // any other key must fail verification
    Bot bot(c.opts.timeout);
    std::string detail;
    const auto r = bot.connect(s, detail);
    if (r != vn::ConnectResult::VerificationFailed)
        fail(std::format("connect result '{}' (expected verification failure)", vn::connectResultName(r)));
    return "sunucu doğrulanamadı (signature rejected locally)";
}

std::string scenarioWaitShutdown(const Context& c)
{
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    loginOk(bot, c);
    std::printf("logged in, waiting for server shutdown\n");
    std::fflush(stdout);
    const Received m = bot.next("waiting for ERROR(ServerShutdown)");
    vn::Error err;
    if (m.opcode != static_cast<std::uint16_t>(vn::Opcode::Error) || !vn::decodeMessage(m.payload, err) ||
        err.errorCode != vn::ErrorCode::ServerShutdown)
        fail("expected ERROR(ServerShutdown)");
    bot.expectClosed(false, bot.timeout());
    return "ERROR(ServerShutdown) then close";
}

int runScenario(const Context& c)
{
    using Fn = std::string (*)(const Context&);
    struct Entry
    {
        std::string_view name;
        Fn fn;
    };
    static constexpr Entry kScenarios[] = {
        {"ping", &scenarioPing},         {"login", &scenarioLogin},       {"chars", &scenarioChars},
        {"world", &scenarioWorld},       {"badstate", &scenarioBadState}, {"garbage", &scenarioGarbage},
        {"wrongkey", &scenarioWrongKey}, {"waitshutdown", &scenarioWaitShutdown},
    };
    for (const auto& e : kScenarios)
    {
        if (e.name != c.opts.scenario)
            continue;
        try
        {
            const std::string summary = e.fn(c);
            std::printf("PASS scenario=%s %s\n", c.opts.scenario.c_str(), summary.c_str());
            return kExitPass;
        }
        catch (const Failure& f)
        {
            std::printf("FAIL scenario=%s %s\n", c.opts.scenario.c_str(), f.reason.c_str());
            return kExitFail;
        }
    }
    std::fprintf(stderr, "unknown scenario '%s'\n", c.opts.scenario.c_str());
    return kExitUsage;
}

int run(int argc, char** argv)
{
    Options opts;
    if (!parseArgs(argc, argv, opts))
    {
        printUsage();
        return kExitUsage;
    }
    if (opts.help)
    {
        std::printf("botclient %s (%s %s)\n", vo::version::kString, vo::version::kProductName,
                    vo::version::kBuildConfig);
        printUsage();
        return kExitPass;
    }
    if (opts.scenario.empty())
    {
        std::fprintf(stderr, "--scenario is required\n");
        printUsage();
        return kExitUsage;
    }
    vo::log::setLevel(vo::log::Level::Warn);

    // Defaults from client.toml (optional when everything is given on the command line).
    vo::core::Config config;
    std::optional<std::filesystem::path> configPath;
    if (opts.configPath)
        configPath = vo::core::pathFromUtf8(*opts.configPath);
    else if (!(opts.host && opts.port && opts.serverKeyHex))
        configPath = vo::core::locateDataFile("config/client.toml");
    if (configPath)
    {
        std::string error;
        if (!config.loadWithOverlay(*configPath, error))
        {
            std::fprintf(stderr, "cannot load %s: %s\n", vo::core::pathToUtf8(*configPath).c_str(), error.c_str());
            return kExitUsage;
        }
    }

    Context c;
    c.opts = opts;
    c.settings.host = opts.host.value_or(config.getString("server.host", "127.0.0.1"));
    const std::int64_t port = opts.port ? *opts.port : config.getInt("server.port", vn::kDefaultPort);
    if (port <= 0 || port > 65535)
    {
        std::fprintf(stderr, "invalid port %lld\n", static_cast<long long>(port));
        return kExitUsage;
    }
    c.settings.port = static_cast<std::uint16_t>(port);
    const std::string keyHex = opts.serverKeyHex.value_or(config.getString("server_public_key", ""));
    const auto key = vn::parsePublicKeyHex(keyHex);
    if (!key)
    {
        std::fprintf(stderr, "no valid server public key (64 hex chars) from --server-key or "
                             "config/client.toml server_public_key\n");
        return kExitUsage;
    }
    c.settings.serverPublicKey = *key;
    c.settings.clientBuild = static_cast<std::uint32_t>(vo::version::kMajor * 10000 + vo::version::kMinor * 100 +
                                                        vo::version::kPatch);
    c.settings.handshakeTimeout = opts.timeout;
    c.user = opts.user.value_or("bot_" + randomDigits(8));

    if (!vn::initCrypto())
    {
        std::fprintf(stderr, "sodium_init failed\n");
        return kExitUsage;
    }
    return runScenario(c);
}

} // namespace vo::tools::botclient

int main(int argc, char** argv)
{
    try
    {
        return vo::tools::botclient::run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL unhandled exception: %s\n", e.what());
        return 1;
    }
}
