// vo_server entry point: config, logging, crypto/db library init, server key handling (--gen-keys),
// world data (zone 1 terrain from world.assets_dir), database preparation (create if missing,
// migrations, --migrate-only), account service selection (PostgreSQL when db.connection_string is
// set, in-memory otherwise), the world simulation thread, the TCP listener with its session state
// machine, and clean shutdown on Ctrl+C / --run-seconds (every session gets ERROR(ServerShutdown),
// the world saves every player still in it, queued DB jobs are drained).

#include "server/auth/MemoryAccountService.h"
#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "server/db/DbSettings.h"
#include "server/db/Migrations.h"
#include "server/net/Server.h"
#include "server/world/World.h"
#include "shared/core/Clock.h"
#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"
#include "shared/game/Character.h"
#include "shared/game/Items.h"
#include "shared/game/Loot.h"
#include "shared/game/Monsters.h"
#include "shared/game/Movement.h"
#include "shared/game/Skills.h"
#include "shared/game/ZoneData.h"
#include "shared/net/Crypto.h"

#include <asio.hpp>
#include <libpq-fe.h>
#include <pqxx/version>
#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitConfig = 2;
constexpr int kExitInit = 3;
constexpr int kExitRuntime = 4;
constexpr int kExitDatabase = 5;
constexpr int kExitWorldData = 6;

constexpr auto kForcedStopAfter = std::chrono::seconds(5);

namespace vdb = vo::server::db;
namespace vw = vo::server::world;

// [world] settings; false (logged) if a value is out of range.
bool loadWorldSettings(const vo::core::Config& config, vw::WorldSettings& out)
{
    const std::int64_t tickRate = config.getInt("world.tick_rate", 20);
    const std::int64_t saveInterval = config.getInt("world.save_interval_s", 60);
    const std::int64_t statsInterval = config.getInt("world.stats_interval_s", 60);
    const std::int64_t catchUp = config.getInt("world.max_catch_up_ticks", 5);
    const double bank = config.getDouble("world.move_bank_seconds", vo::game::kMoveDefaultBankSeconds);
    const double spread = config.getDouble("world.spawn_spread_m", 2.0);
    if (tickRate <= 0 || tickRate > 1000)
    {
        VO_LOG_FATAL("world.tick_rate {} out of range (1..1000)", tickRate);
        return false;
    }
    if (tickRate < 10)
        VO_LOG_WARN("world.tick_rate {} is below the 10 Hz ENTITY_MOVE relay rate", tickRate);
    if (saveInterval < 0 || saveInterval > 3600 || statsInterval < 0 || statsInterval > 86400)
    {
        VO_LOG_FATAL("world.save_interval_s (0..3600) or world.stats_interval_s (0..86400) out of range");
        return false;
    }
    if (catchUp < 1 || catchUp > 1000)
    {
        VO_LOG_FATAL("world.max_catch_up_ticks {} out of range (1..1000)", catchUp);
        return false;
    }
    if (!(bank >= 0.2 && bank <= 10.0))
    {
        VO_LOG_FATAL("world.move_bank_seconds {} out of range (0.2..10)", bank);
        return false;
    }
    if (!(spread >= 0.0 && spread <= 20.0))
    {
        VO_LOG_FATAL("world.spawn_spread_m {} out of range (0..20)", spread);
        return false;
    }
    // Testing (UI flows, integration runs): a fixed seed for the zone's RNG (spawn spread, monster
    // wandering, damage / critical / dodge rolls), so a run sees the same rolls for the same inputs.
    // 0 (default) = random.
    const std::int64_t seed = config.getInt("world.seed", 0);
    if (seed < 0 || seed > 0xFFFFFFFFll)
    {
        VO_LOG_FATAL("world.seed {} out of range (0..4294967295)", seed);
        return false;
    }
    out.tickRate = static_cast<int>(tickRate);
    out.saveInterval = std::chrono::seconds(saveInterval);
    out.statsInterval = std::chrono::seconds(statsInterval);
    out.maxCatchUpTicks = static_cast<int>(catchUp);
    out.zone.rules.bankSeconds = static_cast<float>(bank);
    out.zone.spawnSpread = static_cast<float>(spread);
    out.zone.seed = static_cast<std::uint32_t>(seed);
    if (seed != 0)
        VO_LOG_INFO("world: fixed RNG seed {} (world.seed, testing)", seed);
    return true;
}

// world.assets_dir: relative paths are relative to the repository root (the config directory's
// parent, like log.file); empty = search assets/<terrain> upwards from cwd and exe dir. `terrain` is
// the zone's path from zones.toml (e.g. terrain/zone2/zone2.vterrain).
std::optional<std::filesystem::path> locateZoneTerrain(const vo::core::Config& config,
                                                       const std::filesystem::path& configPath,
                                                       std::string_view terrain)
{
    const std::string dir = config.getString("world.assets_dir", "");
    const auto relative = vo::core::pathFromUtf8(std::string(terrain));
    if (!dir.empty())
    {
        auto base = vo::core::pathFromUtf8(dir);
        if (base.is_relative())
            base = std::filesystem::absolute(configPath).parent_path().parent_path() / base;
        return base / relative; // a missing file is reported by the load ("cannot load zone N")
    }
    return vo::core::locateDataFile(std::filesystem::path("assets") / relative);
}

// world.items_file: like world.assets_dir (relative = to the repository root); empty = search
// gamedata/items.toml upwards from cwd and exe dir.
std::optional<std::filesystem::path> locateItemsFile(const vo::core::Config& config,
                                                     const std::filesystem::path& configPath)
{
    const std::string file = config.getString("world.items_file", "");
    if (!file.empty())
    {
        auto path = vo::core::pathFromUtf8(file);
        if (path.is_relative())
            path = std::filesystem::absolute(configPath).parent_path().parent_path() / path;
        return path;
    }
    return vo::game::locateItemsFile();
}

// Like locateItemsFile for any gamedata file: `key` overrides (relative = to the repository root),
// else `defaultPath` searched upwards from cwd and exe dir.
std::optional<std::filesystem::path> locateGameDataFile(const vo::core::Config& config,
                                                        const std::filesystem::path& configPath, const char* key,
                                                        std::string_view defaultPath)
{
    const std::string file = config.getString(key, "");
    if (!file.empty())
    {
        auto path = vo::core::pathFromUtf8(file);
        if (path.is_relative())
            path = std::filesystem::absolute(configPath).parent_path().parent_path() / path;
        return path;
    }
    return vo::core::locateDataFile(std::filesystem::path(defaultPath));
}

struct Options
{
    std::optional<std::filesystem::path> configPath;
    std::optional<long long> runSeconds;
    std::optional<std::uint16_t> port;
    std::optional<std::filesystem::path> portFile;
    bool genKeys = false;
    bool force = false;
    bool migrateOnly = false;
    bool help = false;
};

void printUsage()
{
    std::printf(
        "usage: vo_server [--config <path/server.toml>] [--run-seconds <n>] [--port <n>] [--port-file <path>]\n"
        "       vo_server --gen-keys [--force] [--config <path/server.toml>]\n"
        "       vo_server --migrate-only [--config <path/server.toml>]\n"
        "  --config <path>     base config file (default: search config/server.toml upwards from\n"
        "                      the working directory, then from the executable directory).\n"
        "                      <name>.local.toml next to it is merged on top if present.\n"
        "  --run-seconds <n>   stop automatically after n seconds (automation/testing)\n"
        "  --port <n>          override net.listen_port (0 = any free port)\n"
        "  --port-file <path>  write the actual listening port to this file once listening\n"
        "  --gen-keys          create the server's Ed25519 key pair in <config dir>/keys/\n"
        "                      (server_ed25519.key + .pub), print the public key hex and exit\n"
        "  --force             with --gen-keys: overwrite existing key files\n"
        "  --migrate-only      create the database if missing, apply pending migrations and exit\n"
        "environment:\n"
        "  VEDAT_DB_URL        overrides db.connection_string\n"
        "  VEDAT_DB_ADMIN_URL  overrides db.admin_connection_string\n"
        "exit codes: 0 ok, 1 usage, 2 config, 3 init, 4 runtime, 5 database unavailable/migration failed,\n"
        "            6 world data (zone terrains, gamedata/zones.toml, items.toml, skills.toml, monsters.toml or\n"
        "            loot.toml) missing or invalid\n");
}

// Creates the database if missing, applies migrations and logs what the connection looks like.
bool prepareDatabase(const vdb::DbSettings& settings, std::string& error)
{
    bool created = false;
    if (!vdb::ensureDatabaseExists(settings, created, error))
        return false;
    if (auto conn = vdb::openConnection(settings, settings.connectionString, error))
    {
        vdb::ServerInfo info;
        if (vdb::queryServerInfo(*conn, info, error))
        {
            VO_LOG_INFO("database: connected to '{}' - {}", info.database, info.version);
            if (info.ssl)
                VO_LOG_INFO("database: connection encrypted ({})", info.sslVersion);
            else
                VO_LOG_WARN("database: connection is NOT encrypted (set sslmode=require or verify-full in the "
                            "connection string for remote servers)");
        }
    }
    else
    {
        error = "cannot connect: " + error;
        return false;
    }
    vdb::MigrationReport report;
    return vdb::migrateDatabase(settings, std::nullopt, report, error);
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
        else if (arg == "--migrate-only")
        {
            out.migrateOnly = true;
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
    if (out.migrateOnly && out.genKeys)
    {
        std::fprintf(stderr, "--migrate-only and --gen-keys cannot be combined\n");
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
    vdb::bindDbEnv(config);
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
    vw::WorldSettings worldSettings;
    if (!loadWorldSettings(config, worldSettings))
        return kExitConfig;
    const int tickRate = worldSettings.tickRate;
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
    settings.listenPort = static_cast<std::uint16_t>(listenPort);
    settings.maxConnections = static_cast<std::size_t>(maxConnections);
    const std::int64_t ioThreads = config.getInt("net.io_threads", 0);
    if (ioThreads < 0 || ioThreads > 64)
    {
        VO_LOG_FATAL("net.io_threads {} out of range (0 = auto, 1..64)", ioThreads);
        return kExitConfig;
    }
    settings.ioThreads = ioThreads > 0 ? static_cast<std::size_t>(ioThreads)
                                       : std::clamp<std::size_t>(std::thread::hardware_concurrency() / 4, 1, 4);

    vdb::DbSettings dbSettings;
    if (!vdb::loadDbSettings(config, dbSettings, error))
    {
        VO_LOG_FATAL("invalid [db] configuration: {}", error);
        return kExitConfig;
    }
    const bool dbConfigured = !dbSettings.connectionString.empty();

    // ---- --migrate-only (needs neither server keys nor the listener) ----
    if (opts.migrateOnly)
    {
        if (!dbConfigured)
        {
            VO_LOG_FATAL("--migrate-only: db.connection_string is not configured");
            return kExitConfig;
        }
        VO_LOG_INFO("database: {} (schema {})", vdb::describeConnectionString(dbSettings.connectionString),
                    dbSettings.schema);
        if (!prepareDatabase(dbSettings, error))
        {
            VO_LOG_FATAL("database: {}", error);
            return kExitDatabase;
        }
        VO_LOG_INFO("--migrate-only: done");
        return kExitOk;
    }

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

    // ---- World data ----
    // Declaration order matters: io outlives the account service (whose workers post into it), the
    // pool outlives the service using it, the service outlives the world (which saves through it)
    // and the world outlives the server (whose sessions post into it).
    asio::io_context io;
    std::unique_ptr<vdb::ConnectionPool> pool;
    std::unique_ptr<vo::server::auth::IAccountService> accounts;
    vo::server::auth::IAccountService* accountsForWorld = nullptr; // set once `accounts` exists
    vw::World world(worldSettings, [&accountsForWorld](vo::server::auth::AccountId account,
                                                       vo::server::auth::CharacterId character,
                                                       vo::server::auth::CharacterPosition position,
                                                       vo::server::auth::CharacterProgress progress,
                                                       std::function<void(bool)> done) {
        if (accountsForWorld)
            accountsForWorld->saveCharacterState(account, character, position, progress, std::move(done));
        else if (done)
            done(false);
    });
    // ---- Zones (docs/PROTOCOL_ZONES.md §1: every zone of gamedata/zones.toml, or refuse to start) ----
    std::shared_ptr<vo::game::ZoneCatalog> zoneCatalog;
    {
        const auto zonesPath = locateGameDataFile(config, configPath, "world.zones_file", vo::game::kZonesFile);
        if (!zonesPath)
        {
            VO_LOG_FATAL("zones: {} not found (searched upwards from the working directory and the executable "
                         "directory; or set world.zones_file)",
                         vo::game::kZonesFile);
            return kExitWorldData;
        }
        zoneCatalog = std::make_shared<vo::game::ZoneCatalog>();
        if (!vo::game::loadZoneCatalog(*zonesPath, *zoneCatalog, error))
        {
            VO_LOG_FATAL("zones: {}", error);
            return kExitWorldData;
        }
        VO_LOG_INFO("zones: {} zone(s) from {}", zoneCatalog->size(), vo::core::pathToUtf8(*zonesPath));
        // Testing hook (UI flows, older integration scenarios): new characters of every race that may
        // enter this zone start there instead of in their homeland. 0 = off.
        const std::int64_t overrideZone = config.getInt("world.start_zone_override", 0);
        const vo::game::ZoneDef* overrideDef = nullptr;
        if (overrideZone != 0)
        {
            overrideDef = overrideZone > 0 && overrideZone <= 65535
                              ? zoneCatalog->find(static_cast<std::uint16_t>(overrideZone))
                              : nullptr;
            if (!overrideDef)
            {
                VO_LOG_FATAL("world.start_zone_override {} is not a zone of {}", overrideZone,
                             vo::core::pathToUtf8(*zonesPath));
                return kExitConfig;
            }
            VO_LOG_WARN("world.start_zone_override = {}: new characters start in zone {} ({}) (testing)",
                        overrideZone, overrideDef->id, overrideDef->name);
        }
        for (const vo::game::ZoneDef& def : zoneCatalog->zones())
        {
            const auto terrainPath = locateZoneTerrain(config, configPath, def.terrain);
            if (!terrainPath)
            {
                VO_LOG_FATAL("world: zone {} ({}) terrain not found (searched assets/{} upwards from the working "
                             "directory and the executable directory)",
                             def.id, def.name, def.terrain);
                VO_LOG_FATAL("build it with: assetc build assets_src assets   (or set world.assets_dir)");
                return kExitWorldData;
            }
            vw::ZoneLayout layout;
            layout.raceMask = def.raceMask;
            for (const vo::game::GateDef& g : def.gates)
            {
                const vo::game::ZoneDef* target = zoneCatalog->find(g.toZone);
                layout.gates.push_back({g, target ? target->raceMask : std::uint8_t{0}});
            }
            if (!world.loadZone(def.id, *terrainPath, error, std::move(layout)))
            {
                VO_LOG_FATAL("world: cannot load zone {} ({}) from {}: {}", def.id, def.name,
                             vo::core::pathToUtf8(*terrainPath), error);
                VO_LOG_FATAL("rebuild it with: assetc build assets_src assets   (or fix world.assets_dir)");
                return kExitWorldData;
            }
            const auto& t = world.zone(def.id)->terrain();
            VO_LOG_INFO("world: zone {} ({}) from {} ({:.0f} m, heights {}^2, walk mask {}^2, {} objects, {} gate(s))",
                        def.id, def.name, vo::core::pathToUtf8(*terrainPath), t.worldSize, t.resolution,
                        t.maskResolution, t.objects.size(), def.gates.size());
        }
        const auto homeOf = [&](vo::game::Race race) -> std::uint16_t {
            const auto r = static_cast<std::uint8_t>(race);
            if (overrideDef && overrideDef->allows(r))
                return overrideDef->id;
            return zoneCatalog->homeZone(r);
        };
        if (!world.finalizeZones(homeOf(vo::game::Race::Aydor), homeOf(vo::game::Race::Kargan), error))
        {
            VO_LOG_FATAL("world: {} ({})", error, vo::core::pathToUtf8(*zonesPath));
            return kExitWorldData;
        }
        vo::game::installZoneCatalog(zoneCatalog); // before any session or the world thread runs
        const auto spawns = world.newCharacterSpawns();
        VO_LOG_INFO("world: new characters: Aydor in zone {} ({:.1f}, {:.1f}), Kargan in zone {} ({:.1f}, {:.1f})",
                    spawns.aydor.zoneId, spawns.aydor.x, spawns.aydor.z, spawns.kargan.zoneId, spawns.kargan.x,
                    spawns.kargan.z);
        VO_LOG_INFO("world: tick {} Hz, save every {} s, move bank {:.2f} s, stats every {} s", tickRate,
                    worldSettings.saveInterval.count(), worldSettings.zone.rules.bankSeconds,
                    worldSettings.statsInterval.count());
    }

    // ---- Item templates (docs/PROTOCOL_ITEMS.md §1: refuse to start without a valid file) ----
    {
        const auto itemsPath = locateItemsFile(config, configPath);
        if (!itemsPath)
        {
            VO_LOG_FATAL("items: {} not found (searched upwards from the working directory and the executable "
                         "directory; or set world.items_file)",
                         vo::game::kItemsFile);
            return kExitWorldData;
        }
        auto catalog = std::make_shared<vo::game::ItemCatalog>();
        if (!vo::game::loadItemCatalog(*itemsPath, *catalog, error))
        {
            VO_LOG_FATAL("items: {}", error);
            return kExitWorldData;
        }
        VO_LOG_INFO("items: {} template(s) from {}", catalog->size(), vo::core::pathToUtf8(*itemsPath));
        settings.items = std::move(catalog);
    }

    // ---- Skills (docs/PROTOCOL_SKILLS.md: refuse to start without a valid file) ----
    {
        const auto skillsPath = locateGameDataFile(config, configPath, "world.skills_file", vo::game::kSkillsFile);
        if (!skillsPath)
        {
            VO_LOG_FATAL("skills: {} not found (searched upwards from the working directory and the executable "
                         "directory; or set world.skills_file)",
                         vo::game::kSkillsFile);
            return kExitWorldData;
        }
        auto catalog = std::make_shared<vo::game::SkillCatalog>();
        if (!vo::game::loadSkillCatalog(*skillsPath, *catalog, error))
        {
            VO_LOG_FATAL("skills: {}", error);
            return kExitWorldData;
        }
        VO_LOG_INFO("skills: {} skill(s) in {} branch(es) from {}", catalog->size(), catalog->branches().size(),
                    vo::core::pathToUtf8(*skillsPath));
        vo::game::installSkillCatalog(std::move(catalog)); // before any session or the world thread runs
    }

    // ---- Monsters, spawn points, safe zones (docs/PROTOCOL_COMBAT.md §1.3: refuse to start without) ----
    std::shared_ptr<vo::game::MonsterData> monsterData;
    {
        const auto monstersPath = locateGameDataFile(config, configPath, "world.monsters_file",
                                                     vo::game::kMonstersFile);
        if (!monstersPath)
        {
            VO_LOG_FATAL("monsters: {} not found (searched upwards from the working directory and the executable "
                         "directory; or set world.monsters_file)",
                         vo::game::kMonstersFile);
            return kExitWorldData;
        }
        auto data = std::make_shared<vo::game::MonsterData>();
        if (!vo::game::loadMonsterData(*monstersPath, *data, error))
        {
            VO_LOG_FATAL("monsters: {}", error);
            return kExitWorldData;
        }
        if (!world.installMonsters(data, error))
        {
            VO_LOG_FATAL("monsters: {} ({})", error, vo::core::pathToUtf8(*monstersPath));
            return kExitWorldData;
        }
        {
            std::vector<std::uint16_t> unloaded; // PROTOCOL_ZONES.md §1.2: spawns of zones not loaded are ignored
            for (const auto& s : data->spawns())
                if (!world.zone(s.zone) && std::find(unloaded.begin(), unloaded.end(), s.zone) == unloaded.end())
                    unloaded.push_back(s.zone);
            for (const std::uint16_t z : unloaded)
                VO_LOG_WARN("monsters: spawn points of zone {} are ignored (the zone is not in zones.toml)", z);
        }
        VO_LOG_INFO("monsters: {} template(s), {} spawn point(s), {} safe zone(s) from {}; {} monster(s) in the world",
                    data->templates().size(), data->spawns().size(), data->safeZones().size(),
                    vo::core::pathToUtf8(*monstersPath), world.monsterCount());
        monsterData = std::move(data);
    }

    // ---- Loot tables (docs/PROTOCOL_LOOT.md §2: refuse to start without a valid file) ----
    {
        const auto lootPath = locateGameDataFile(config, configPath, "world.loot_file", vo::game::kLootFile);
        if (!lootPath)
        {
            VO_LOG_FATAL("loot: {} not found (searched upwards from the working directory and the executable "
                         "directory; or set world.loot_file)",
                         vo::game::kLootFile);
            return kExitWorldData;
        }
        auto tables = std::make_shared<vo::game::LootTables>();
        if (!vo::game::loadLootTables(*lootPath, *settings.items, monsterData.get(), *tables, error))
        {
            VO_LOG_FATAL("loot: {}", error);
            return kExitWorldData;
        }
        world.installLoot(tables);
        VO_LOG_INFO("loot: {} table(s) from {}", tables->tables().size(), vo::core::pathToUtf8(*lootPath));
    }

    // ---- NPCs / merchants (docs/PROTOCOL_SHOP.md §1: refuse to start without a valid file) ----
    {
        const auto npcsPath = locateGameDataFile(config, configPath, "world.npcs_file", vo::game::kNpcsFile);
        if (!npcsPath)
        {
            VO_LOG_FATAL("npcs: {} not found (searched upwards from the working directory and the executable "
                         "directory; or set world.npcs_file)",
                         vo::game::kNpcsFile);
            return kExitWorldData;
        }
        auto catalog = std::make_shared<vo::game::NpcCatalog>();
        if (!vo::game::loadNpcCatalog(*npcsPath, settings.items.get(), *catalog, error))
        {
            VO_LOG_FATAL("npcs: {}", error);
            return kExitWorldData;
        }
        if (!world.installNpcs(catalog, *settings.items, error))
        {
            VO_LOG_FATAL("npcs: {} ({})", error, vo::core::pathToUtf8(*npcsPath));
            return kExitWorldData;
        }
        std::vector<std::uint16_t> unloaded; // §1.2: NPCs of zones not loaded are ignored
        for (const auto& n : catalog->npcs())
            if (!world.zone(n.zone) && std::find(unloaded.begin(), unloaded.end(), n.zone) == unloaded.end())
                unloaded.push_back(n.zone);
        for (const std::uint16_t z : unloaded)
            VO_LOG_WARN("npcs: NPCs of zone {} are ignored (the zone is not in zones.toml)", z);
        VO_LOG_INFO("npcs: {} NPC(s) from {}; {} in the world", catalog->size(), vo::core::pathToUtf8(*npcsPath),
                    world.npcCount());
        vo::game::installNpcCatalog(std::move(catalog)); // before any session or the world thread runs
    }

    // ---- Account service ----
    if (dbConfigured)
    {
        VO_LOG_INFO("database: {} (schema {}, pool {}, {} worker thread(s), statement timeout {} ms, required {})",
                    vdb::describeConnectionString(dbSettings.connectionString), dbSettings.schema,
                    dbSettings.poolSize, dbSettings.workerThreads, dbSettings.statementTimeout.count(),
                    dbSettings.required);
        bool ready = prepareDatabase(dbSettings, error);
        if (ready)
        {
            pool = std::make_unique<vdb::ConnectionPool>(dbSettings);
            ready = pool->warmUp(error);
        }
        if (ready)
        {
            accounts = std::make_unique<vo::server::auth::PostgresAccountService>(
                *pool, vo::server::auth::PostgresAccountService::Options{autoRegister, dbSettings.workerThreads,
                                                                         world.newCharacterSpawns()});
            VO_LOG_INFO("database: using the PostgreSQL account service");
        }
        else if (dbSettings.required)
        {
            VO_LOG_FATAL("database unavailable: {}", error);
            VO_LOG_FATAL("fix [db] in config/server.local.toml (or set db.required = false to start without it)");
            return kExitDatabase;
        }
        else
        {
            pool.reset();
            VO_LOG_ERROR("database unavailable: {}", error);
            VO_LOG_WARN("db.required = false: falling back to the in-memory account service (data is lost on exit)");
        }
    }
    else
    {
        VO_LOG_INFO("database: not configured, using the in-memory account service (data is lost on exit)");
    }
    if (!accounts)
        accounts = std::make_unique<vo::server::auth::MemoryAccountService>(
            vo::server::auth::MemoryAccountService::Options{autoRegister, world.newCharacterSpawns()});
    if (autoRegister)
        VO_LOG_WARN("auth.dev_auto_register = true: unknown usernames are registered on first login");
    accountsForWorld = accounts.get();

    vo::server::net::Server server(io, settings, std::move(identity), *accounts, world);
    if (!server.start(error))
    {
        VO_LOG_FATAL("{}", error);
        return kExitRuntime;
    }
    world.start();
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
    vdb::markThreadNoDb(); // this thread runs the io_context from here on: no blocking DB calls
    // Once shutdown() has closed every session (they run on the network threads), the only remaining
    // work here is the force-stop timer. Poll until the sessions are gone, then cancel it.
    auto lastLogFlush = std::chrono::steady_clock::now();
    while (!io.stopped())
    {
        io.run_one_for(std::chrono::milliseconds(50));
        // INFO lines are buffered (only WARN+ flush at once): flush once a second, so a log file
        // being watched, or one cut short by a hard kill, is at most a second behind.
        if (const auto now = std::chrono::steady_clock::now(); now - lastLogFlush >= std::chrono::seconds(1))
        {
            lastLogFlush = now;
            vo::log::flush();
        }
        if (server.context().shuttingDown && server.sessionCount() == 0)
        {
            forceStopTimer.cancel();
            io.run(); // drain remaining handlers (posted close notifications)
            break;
        }
    }
    // Every session is closed (or the io loop was force-stopped): the world runs the leaves still
    // queued and saves whoever is left (bounded: one tick), then the DB queue is drained.
    world.stop();
    accounts->stop(); // drains queued DB jobs (e.g. position saves of the closed sessions)
    server.stopNetwork(std::chrono::milliseconds(0)); // after the DB callbacks that post into it
    if (pool)
        pool->close();
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
