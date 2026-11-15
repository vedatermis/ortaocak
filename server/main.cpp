// vo_server entry point: config, logging, crypto/db library init, server key handling (--gen-keys),
// the TCP listener with its session state machine, and clean shutdown on Ctrl+C / --run-seconds
// (every session gets ERROR(ServerShutdown)).

#include "server/auth/MemoryAccountService.h"
#include "server/net/Server.h"
#include "shared/core/Clock.h"
#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"
#include "shared/net/Crypto.h"

#include <asio.hpp>
#include <libpq-fe.h>
#include <pqxx/version>
#include <sodium.h>

#include <charconv>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitConfig = 2;
constexpr int kExitInit = 3;
constexpr int kExitRuntime = 4;

constexpr auto kForcedStopAfter = std::chrono::seconds(5);

struct Options
{
    std::optional<std::filesystem::path> configPath;
    std::optional<long long> runSeconds;
    std::optional<std::uint16_t> port;
    std::optional<std::filesystem::path> portFile;
    bool genKeys = false;
    bool force = false;
    bool help = false;
};

void printUsage()
{
    std::printf(
        "usage: vo_server [--config <path/server.toml>] [--run-seconds <n>] [--port <n>] [--port-file <path>]\n"
        "       vo_server --gen-keys [--force] [--config <path/server.toml>]\n"
        "  --config <path>     base config file (default: search config/server.toml upwards from\n"
        "                      the working directory, then from the executable directory).\n"
        "                      <name>.local.toml next to it is merged on top if present.\n"
        "  --run-seconds <n>   stop automatically after n seconds (automation/testing)\n"
        "  --port <n>          override net.listen_port (0 = any free port)\n"
        "  --port-file <path>  write the actual listening port to this file once listening\n"
        "  --gen-keys          create the server's Ed25519 key pair in <config dir>/keys/\n"
        "                      (server_ed25519.key + .pub), print the public key hex and exit\n"
        "  --force             with --gen-keys: overwrite existing key files\n"
        "environment:\n"
        "  VEDAT_DB_URL        overrides db.connection_string\n");
}

template <class T>
bool parseNumber(std::string_view v, T& out)
{
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc{} && ptr == v.data() + v.size();
}

bool parseArgs(int argc, char** argv, Options& out)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            out.help = true;
        }
        else if (arg == "--config" && i + 1 < argc)
        {
            out.configPath = vo::core::pathFromUtf8(argv[++i]);
        }
        else if (arg == "--run-seconds" && i + 1 < argc)
        {
            const std::string_view v = argv[++i];
            long long n = 0;
            if (!parseNumber(v, n) || n < 0)
            {
                std::fprintf(stderr, "invalid --run-seconds value '%.*s'\n", static_cast<int>(v.size()), v.data());
                return false;
            }
            out.runSeconds = n;
        }
        else if (arg == "--port" && i + 1 < argc)
        {
            const std::string_view v = argv[++i];
            std::uint16_t n = 0;
            if (!parseNumber(v, n))
            {
                std::fprintf(stderr, "invalid --port value '%.*s'\n", static_cast<int>(v.size()), v.data());
                return false;
            }
            out.port = n;
        }
        else if (arg == "--port-file" && i + 1 < argc)
        {
            out.portFile = vo::core::pathFromUtf8(argv[++i]);
        }
        else if (arg == "--gen-keys")
        {
            out.genKeys = true;
        }
        else if (arg == "--force")
        {
            out.force = true;
        }
        else
        {
            std::fprintf(stderr, "unknown or incomplete argument '%.*s'\n", static_cast<int>(arg.size()),
                         arg.data());
            return false;
        }
    }
    if (out.force && !out.genKeys)
    {
        std::fprintf(stderr, "--force is only valid together with --gen-keys\n");
        return false;
    }
    return true;
}

int generateKeys(const std::filesystem::path& keyDir, bool force)
{
    auto identity = vo::net::ServerIdentity::generate();
    std::string error;
    if (!identity.save(keyDir, force, error))
    {
        VO_LOG_FATAL("--gen-keys: {}", error);
        return kExitRuntime;
    }
    VO_LOG_INFO("wrote {} and {} in {}", vo::net::kServerSecretKeyFile, vo::net::kServerPublicKeyFile,
                vo::core::pathToUtf8(keyDir));
    VO_LOG_INFO("put this public key into config/client.toml (server_public_key):");
    vo::log::flush();
    std::printf("%s\n", identity.publicKeyHex().c_str());
    std::fflush(stdout);
    return kExitOk;
}

int run(const Options& opts)
{
    // ---- Config ----
    std::filesystem::path configPath;
    if (opts.configPath)
    {
        configPath = *opts.configPath;
    }
    else if (auto found = vo::core::locateDataFile("config/server.toml"))
    {
        configPath = *found;
    }
    else
    {
        VO_LOG_FATAL("config/server.toml not found (searched upwards from cwd and exe dir); use --config");
        return kExitConfig;
    }

    vo::core::Config config;
    config.bindEnv("db.connection_string", "VEDAT_DB_URL");
    std::string error;
    if (!config.loadWithOverlay(configPath, error))
    {
        VO_LOG_FATAL("failed to load config: {}", error);
        return kExitConfig;
    }

    const std::string levelText = config.getString("log.level", "info");
    if (auto lvl = vo::log::parseLevel(levelText))
        vo::log::setLevel(*lvl);
    else
        VO_LOG_WARN("unknown log.level '{}', keeping '{}'", levelText, vo::log::toString(vo::log::level()));

    // Keys live in <config dir>/keys (config/keys/ in the repository, git-ignored).
    const std::filesystem::path keyDir = std::filesystem::absolute(configPath).parent_path() / "keys";

    if (!vo::net::initCrypto())
    {
        VO_LOG_FATAL("sodium_init failed");
        return kExitInit;
    }
    if (opts.genKeys)
        return generateKeys(keyDir, opts.force);

    const std::string logFile = config.getString("log.file", "");
    if (!logFile.empty())
    {
        auto logPath = vo::core::pathFromUtf8(logFile);
        if (logPath.is_relative())
            logPath = configPath.parent_path().parent_path() / logPath;
        if (!vo::log::openFile(logPath))
            VO_LOG_WARN("cannot open log file '{}'", vo::core::pathToUtf8(logPath));
    }

    vo::server::net::ServerSettings settings;
    settings.listenAddress = config.getString("net.listen_address", "0.0.0.0");
    const std::int64_t listenPort = opts.port ? *opts.port : config.getInt("net.listen_port", 15001);
    const std::int64_t maxConnections = config.getInt("net.max_connections", 2000);
    const std::int64_t tickRate = config.getInt("world.tick_rate", 20);
    settings.maintenance = config.getBool("server.maintenance", false);
    const bool autoRegister = config.getBool("auth.dev_auto_register", false);
    if (listenPort < 0 || listenPort > 65535)
    {
        VO_LOG_FATAL("net.listen_port {} out of range", listenPort);
        return kExitConfig;
    }
    if (maxConnections <= 0 || maxConnections > 1'000'000)
    {
        VO_LOG_FATAL("net.max_connections {} out of range", maxConnections);
        return kExitConfig;
    }
    if (tickRate <= 0 || tickRate > 1000)
    {
        VO_LOG_FATAL("world.tick_rate {} out of range", tickRate);
        return kExitConfig;
    }
    settings.listenPort = static_cast<std::uint16_t>(listenPort);
    settings.maxConnections = static_cast<std::size_t>(maxConnections);

    // ---- Banner ----
    VO_LOG_INFO("==============================================");
    VO_LOG_INFO(" {} server v{} ({})", vo::version::kProductName, vo::version::kString,
                vo::version::kBuildConfig);
    VO_LOG_INFO("==============================================");
    for (const auto& src : config.sources())
        VO_LOG_INFO("config: {}", vo::core::pathToUtf8(src));
    VO_LOG_INFO("listen {}:{}  max_connections {}  tick_rate {} Hz  log level {}", settings.listenAddress,
                settings.listenPort, settings.maxConnections, tickRate, vo::log::toString(vo::log::level()));
    if (settings.maintenance)
        VO_LOG_WARN("maintenance mode: every handshake is answered with 'maintenance'");

    // ---- Libraries ----
    VO_LOG_INFO("libsodium {} initialised", sodium_version_string());
    const int pqVersion = PQlibVersion();
    VO_LOG_INFO("libpqxx {}, libpq {}.{}", PQXX_VERSION, pqVersion / 10000, pqVersion % 10000);

    // ---- Server identity ----
    vo::net::ServerIdentity identity;
    if (!vo::net::ServerIdentity::load(keyDir, identity, error))
    {
        VO_LOG_FATAL("server key not available: {}", error);
        VO_LOG_FATAL("create it with: vo_server --gen-keys   (then put the printed public key into "
                     "config/client.toml server_public_key)");
        return kExitConfig;
    }
    VO_LOG_INFO("server public key {}", identity.publicKeyHex());

    // ---- Account service ----
    const bool dbConfigured = !config.getString("db.connection_string", "").empty();
    if (dbConfigured)
        VO_LOG_WARN("database: connection string configured, but the PostgreSQL account service arrives in M2; "
                    "using the in-memory account service");
    else
        VO_LOG_INFO("database: not configured, using the in-memory account service (data is lost on exit)");
    if (autoRegister)
        VO_LOG_WARN("auth.dev_auto_register = true: unknown usernames are registered on first login");

    // Declaration order matters: io outlives the account service (whose worker posts into it), which
    // outlives the server.
    asio::io_context io;
    vo::server::auth::MemoryAccountService accounts({autoRegister});
    vo::server::net::Server server(io, settings, std::move(identity), accounts);
    if (!server.start(error))
    {
        VO_LOG_FATAL("{}", error);
        return kExitRuntime;
    }
    if (opts.portFile)
    {
        std::ofstream pf(*opts.portFile, std::ios::trunc);
        pf << server.port() << "\n";
        if (!pf)
            VO_LOG_WARN("cannot write port file '{}'", vo::core::pathToUtf8(*opts.portFile));
    }

    // ---- Event loop ----
    asio::signal_set signals(io, SIGINT, SIGTERM);
#if defined(SIGBREAK)
    signals.add(SIGBREAK);
#endif
    asio::steady_timer runTimer(io);
    asio::steady_timer forceStopTimer(io);

    const auto beginShutdown = [&](const char* why) {
        VO_LOG_INFO("{}, shutting down", why);
        std::error_code ignored;
        signals.cancel(ignored);
        runTimer.cancel();
        server.shutdown();
        forceStopTimer.expires_after(kForcedStopAfter);
        forceStopTimer.async_wait([&](const std::error_code& ec) {
            if (ec)
                return;
            VO_LOG_WARN("sessions did not close within {} s, forcing stop", kForcedStopAfter.count());
            io.stop();
        });
    };

    signals.async_wait([&](const std::error_code& ec, int signo) {
        if (ec)
            return;
        VO_LOG_INFO("received signal {}", signo);
        beginShutdown("signal received");
    });

    if (opts.runSeconds)
    {
        VO_LOG_INFO("--run-seconds {}: will stop automatically", *opts.runSeconds);
        runTimer.expires_after(std::chrono::seconds(*opts.runSeconds));
        runTimer.async_wait([&](const std::error_code& ec) {
            if (ec)
                return;
            beginShutdown("run time elapsed");
        });
    }

    VO_LOG_INFO("server running - press Ctrl+C to stop");
    const vo::core::Stopwatch uptime;
    // Once shutdown() has closed every session, the only remaining work is the force-stop timer.
    // Poll until the sessions are gone, then cancel it so run() returns.
    while (!io.stopped())
    {
        io.run_one();
        if (server.context().shuttingDown && server.sessionCount() == 0)
        {
            forceStopTimer.cancel();
            io.run(); // drain remaining handlers (posted close notifications)
            break;
        }
    }
    accounts.stop();
    VO_LOG_INFO("shutdown complete (uptime {:.1f} s)", uptime.elapsedSeconds());
    return kExitOk;
}

} // namespace

int main(int argc, char** argv)
{
    Options opts;
    if (!parseArgs(argc, argv, opts))
    {
        printUsage();
        return kExitUsage;
    }
    if (opts.help)
    {
        printUsage();
        return kExitOk;
    }

    int code = kExitRuntime;
    try
    {
        code = run(opts);
    }
    catch (const std::exception& e)
    {
        VO_LOG_FATAL("unhandled exception: {}", e.what());
        code = kExitRuntime;
    }
    vo::log::flush();
    vo::log::closeFile();
    return code;
}
