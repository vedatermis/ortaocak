// vo_server entry point (M0 skeleton): config, logging, crypto/db library init, asio event loop
// that exits cleanly on Ctrl+C (or after --run-seconds N).

#include "shared/core/Clock.h"
#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"

#include <asio.hpp>
#include <libpq-fe.h>
#include <pqxx/version>
#include <sodium.h>

#include <charconv>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitConfig = 2;
constexpr int kExitInit = 3;
constexpr int kExitRuntime = 4;

struct Options
{
    std::optional<std::filesystem::path> configPath;
    std::optional<long long> runSeconds;
    bool help = false;
};

void printUsage()
{
    std::printf("usage: vo_server [--config <path/server.toml>] [--run-seconds <n>] [--help]\n"
                "  --config <path>     base config file (default: search config/server.toml upwards from\n"
                "                      the working directory, then from the executable directory).\n"
                "                      <name>.local.toml next to it is merged on top if present.\n"
                "  --run-seconds <n>   stop automatically after n seconds (automation/testing)\n"
                "environment:\n"
                "  VEDAT_DB_URL        overrides db.connection_string\n");
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
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
            if (ec != std::errc{} || ptr != v.data() + v.size() || n < 0)
            {
                std::fprintf(stderr, "invalid --run-seconds value '%.*s'\n", static_cast<int>(v.size()), v.data());
                return false;
            }
            out.runSeconds = n;
        }
        else
        {
            std::fprintf(stderr, "unknown or incomplete argument '%.*s'\n", static_cast<int>(arg.size()),
                         arg.data());
            return false;
        }
    }
    return true;
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

    const std::string logFile = config.getString("log.file", "");
    if (!logFile.empty())
    {
        auto logPath = vo::core::pathFromUtf8(logFile);
        if (logPath.is_relative())
            logPath = configPath.parent_path().parent_path() / logPath;
        if (!vo::log::openFile(logPath))
            VO_LOG_WARN("cannot open log file '{}'", vo::core::pathToUtf8(logPath));
    }

    const std::string listenAddress = config.getString("net.listen_address", "0.0.0.0");
    const std::int64_t listenPort = config.getInt("net.listen_port", 15001);
    const std::int64_t tickRate = config.getInt("world.tick_rate", 20);
    if (listenPort <= 0 || listenPort > 65535)
    {
        VO_LOG_FATAL("net.listen_port {} out of range", listenPort);
        return kExitConfig;
    }
    if (tickRate <= 0 || tickRate > 1000)
    {
        VO_LOG_FATAL("world.tick_rate {} out of range", tickRate);
        return kExitConfig;
    }

    // ---- Banner ----
    VO_LOG_INFO("==============================================");
    VO_LOG_INFO(" {} server v{} ({})", vo::version::kProductName, vo::version::kString,
                vo::version::kBuildConfig);
    VO_LOG_INFO("==============================================");
    for (const auto& src : config.sources())
        VO_LOG_INFO("config: {}", vo::core::pathToUtf8(src));
    VO_LOG_INFO("listen {}:{}  tick_rate {} Hz  log level {}", listenAddress, listenPort, tickRate,
                vo::log::toString(vo::log::level()));

    // ---- Libraries ----
    const int sodiumResult = sodium_init();
    if (sodiumResult < 0)
    {
        VO_LOG_FATAL("sodium_init failed ({})", sodiumResult);
        return kExitInit;
    }
    VO_LOG_INFO("libsodium {} initialised (sodium_init={})", sodium_version_string(), sodiumResult);

    const int pqVersion = PQlibVersion();
    VO_LOG_INFO("libpqxx {}, libpq {}.{}", PQXX_VERSION, pqVersion / 10000, pqVersion % 10000);
    const bool dbConfigured = !config.getString("db.connection_string", "").empty();
    VO_LOG_INFO("database: {} (not connecting in M0)", dbConfigured ? "connection string configured" : "not configured");

    // ---- Event loop ----
    asio::io_context io;
    asio::signal_set signals(io, SIGINT, SIGTERM);
#if defined(SIGBREAK)
    signals.add(SIGBREAK);
#endif
    asio::steady_timer runTimer(io);

    signals.async_wait([&](const std::error_code& ec, int signo) {
        if (ec)
            return;
        VO_LOG_INFO("received signal {}, shutting down", signo);
        runTimer.cancel();
        io.stop();
    });

    if (opts.runSeconds)
    {
        VO_LOG_INFO("--run-seconds {}: will stop automatically", *opts.runSeconds);
        runTimer.expires_after(std::chrono::seconds(*opts.runSeconds));
        runTimer.async_wait([&](const std::error_code& ec) {
            if (ec)
                return;
            VO_LOG_INFO("run time elapsed, shutting down");
            signals.cancel();
            io.stop();
        });
    }

    VO_LOG_INFO("server running - press Ctrl+C to stop");
    const vo::core::Stopwatch uptime;
    io.run();
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
