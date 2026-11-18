// botclient - headless test client for end-to-end server scenarios, built on vo::net::ClientSession.
//
//   botclient --scenario <name> [--host H] [--port P] [--server-key HEX] [--config client.toml]
//             [--user U] [--password P] [--count N] [--expect RESULT] [--timeout-ms N]
//             [--terrain zone1.vterrain] [--race 1|2] [--phase walk|stay|check] [--state-file F]
//   botclient --bots N --duration S [--threads T] [--ramp-ms MS] [--max-corrections K] [...]
//
// Defaults for host, port and server key come from config/client.toml (server.host, server.port,
// server_public_key). Prints one "PASS ..." or "FAIL ..." summary line; exit code 0 only on PASS.
// Exit codes: 0 pass, 1 scenario failed, 2 bad arguments/config.

#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"
#include "tools/botclient/Bot.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>

namespace vo::tools::botclient {

void printUsage()
{
    std::printf(
        "usage: botclient --scenario <name> [options]\n"
        "       botclient --bots <n> --duration <s> [options]          (load mode)\n"
        "scenarios:\n"
        "  ping       handshake + N pings, prints RTT\n"
        "  login      LOGIN_REQ, expects --expect result (default ok)\n"
        "  chars      login, list, create, list, delete, list (+ deleted name stays reserved)\n"
        "  world      create, enter at the race spawn, run/walk legal paths (no POSITION_CORRECT),\n"
        "             leave, re-enter at the same spot, logout\n"
        "  speedhack  4x run speed, a teleport and a banked-time jump: corrected, position held\n"
        "  wallhack   move into a blocked cell, through a thin wall and out of the zone: corrected\n"
        "  floodmove  40 MOVE_REQ in a burst: ERROR(Flood) and close\n"
        "  leaverace  MOVE_REQ right after LEAVE_WORLD_REQ / LOGOUT: dropped, never kicked\n"
        "  dupworld   duplicate login while in the world: kick, despawn, re-enter at the live position\n"
        "  aoi        two bots: spawn, ENTITY_MOVE <= 10 Hz with the final stop, despawn on leave,\n"
        "             despawn/spawn when walking out of / back into the 5x5 cells\n"
        "  persist    --phase walk (default): walk, logout, login -> same position;\n"
        "             --phase stay: walk, stay until the server shuts down; --phase check: enter and\n"
        "             compare with --state-file (written by walk/stay)\n"
        "  items      warrior: kit listed before entry and granted once, INVENTORY order, equip /\n"
        "             unequip / refused moves with ITEM_UPDATE + ITEM_MOVE_RES, ENTITY_EQUIP, stored\n"
        "             after re-entering (--state-file: writes the character name); --phase check:\n"
        "             the character from --state-file has its kit plus a given template 7\n"
        "  combat     warrior: walk to the nearest Kül Çömezi, auto-attack + Ağır Darbe, check\n"
        "             COMBAT_EVENT / ENTITY_DEATH / XP_GAIN; then die to the Kor Bekçisi on purpose,\n"
        "             RESPAWN_REQ too early (TooEarly) and after 5 s (race spawn, 50 %% hp)\n"
        "  loot       warrior: kill Kül Çömezis, open the container (only its own: LootMine), take\n"
        "             everything; ITEM_UPDATE / COINS / LOOT_TAKE_RES consistent with LOOT_CONTENTS,\n"
        "             the container despawns (--state-file: name and balance); --phase check: the\n"
        "             character from --state-file enters with that balance and the looted stacks\n"
        "  stats      warrior: CHAR_STATS on entry, refused allocations, hunt Kül Çömezis to level 2\n"
        "             (respawning after a death), spend the 3 points: CHAR_STATS / SELF_STATS and the\n"
        "             damage dealt follow, kept after re-entering, then a STAT_ALLOCATE_REQ flood\n"
        "             (--state-file: writes the name); --phase check: the character from --state-file\n"
        "             (after vo_admin set-level 10) spends 24 points on Güç, damage follows; --phase\n"
        "             reset: after vo_admin reset-stats every point is unspent again\n"
        "  load       same as --bots N --duration S (every 3rd bot also equips/unequips now and then,\n"
        "             every 5th fights and loots outside the village; the others stay in the safe zone)\n"
        "  zones      (docs/PROTOCOL_ZONES.md; needs every zone's terrain next to --terrain) a new\n"
        "             character of --race starts in its homeland, relogs there, walks into the gate to\n"
        "             Eşik, arrives at the right marker, relogs, is refused once at the other race's\n"
        "             gate, goes back home; monsters in view always belong to the zone; --phase pair:\n"
        "             two Aydor and a Kargan in different zones do not see each other, the Aydor see\n"
        "             each other leave and appear when they change zone (--state-file: name, home zone)\n"
        "  shop       (docs/PROTOCOL_SHOP.md; needs the server's world.testing_enter_vitals_percent = 10 and\n"
        "             homeland starts) a new mage sees its town's merchants, is refused at a distance,\n"
        "             sells its spare staff to the smith, buys potions, puts them on the skill bar,\n"
        "             relogs (stock, Akçe, bar kept), drinks an hp and an mp potion (cooldown, refusals),\n"
        "             relogs (cooldown kept), floods; --phase check: the stored state (--state-file)\n"
        "  shoperrors (server: start_zone_override = 1, world.testing_fail_first_item_write = 16) a mage in\n"
        "             Ortaocak: ServerError for its first buy / sale / use of a Kızılca Şerbet, AlreadyFull,\n"
        "             OutOfRange inside the open shop, YouAreDead after dying to the Kor Bekçisi; --phase\n"
        "             prepare / rich (vo_admin set-coins <name> 999999990 in between): PurseFull, BagFull\n"
        "  zonesrace  overlapping zone changes: --phase same (two bots into one gate in the same tick, and\n"
        "             back), cross (both directions of a gate pair at once, observers in both zones),\n"
        "             logout / drop (server: world.testing_zone_arrival_delay_ms; LOGOUT, LEAVE_WORLD_REQ or a\n"
        "             dropped connection right after ZONE_CHANGE_BEGIN, then new logins)\n"        "  badstate   CHAR_LIST_REQ before login, expects ERROR(BadState) and close\n"
        "  garbage    corrupted encrypted frame, expects a silent close\n"
        "  wrongkey   pins a wrong server key, expects local verification failure\n"
        "  waitshutdown  login, then wait for ERROR(ServerShutdown) and close\n"
        "options:\n"
        "  --host <h>          server host           (default: client.toml server.host)\n"
        "  --port <n>          server port           (default: client.toml server.port)\n"
        "  --server-key <hex>  pinned server key     (default: client.toml server_public_key)\n"
        "  --config <path>     client.toml to read defaults from (default: search config/client.toml)\n"
        "  --user <name>       username (default: random bot_NNNNNNNN)\n"
        "  --password <pw>     password (default: botpassword)\n"
        "  --count <n>         pings for 'ping' (default 5)\n"
        "  --expect <r>        login: ok|invalid_credentials|banned|too_many_attempts|server_error\n"
        "  --timeout-ms <n>    per-step timeout (default 10000)\n"
        "  --terrain <path>    zone 1 .vterrain for world scenarios (default: search\n"
        "                      assets/terrain/zone1/zone1.vterrain upwards from cwd and exe dir)\n"
        "  --race <1|2>        race of created characters (default 1, Aydor)\n"
        "  --phase <p>, --state-file <path>   persist scenario, see above\n"
        "  --bots <n>          load mode: number of bots (auto-registered accounts)\n"
        "  --duration <s>      load mode: measurement window after everyone entered (default 30)\n"
        "  --threads <n>       load mode: client threads (default: half the cores, max 8)\n"
        "  --ramp-ms <n>       load mode: delay between bot starts (default 5)\n"
        "  --max-corrections <n>  load mode: corrections tolerated for PASS (default 0)\n"
        "  --zones             load mode: some bots commute through the gates between Eşik and their\n"
        "                      homeland, some go home and stay (needs every zone's terrain)\n");
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
        const auto positive = [&](int& out) {
            return parseNumber(std::string_view(argv[++i]), out) && out > 0;
        };
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
            if (!positive(o.count))
                return false;
        }
        else if (a == "--expect" && hasValue)
            o.expect = argv[++i];
        else if (a == "--timeout-ms" && hasValue)
        {
            int ms = 0;
            if (!positive(ms))
                return false;
            o.timeout = Ms(ms);
        }
        else if (a == "--terrain" && hasValue)
            o.terrainPath = argv[++i];
        else if (a == "--race" && hasValue)
        {
            if (!positive(o.race) || o.race > 2)
                return false;
        }
        else if (a == "--phase" && hasValue)
            o.phase = argv[++i];
        else if (a == "--state-file" && hasValue)
            o.stateFile = argv[++i];
        else if (a == "--bots" && hasValue)
        {
            if (!positive(o.bots) || o.bots > 20000)
                return false;
        }
        else if (a == "--duration" && hasValue)
        {
            if (!positive(o.durationS))
                return false;
        }
        else if (a == "--threads" && hasValue)
        {
            if (!positive(o.threads) || o.threads > 64)
                return false;
        }
        else if (a == "--ramp-ms" && hasValue)
        {
            if (!parseNumber(std::string_view(argv[++i]), o.rampMs) || o.rampMs < 0)
                return false;
        }
        else if (a == "--zones")
            o.zones = true;
        else if (a == "--max-corrections" && hasValue)
        {
            if (!parseNumber(std::string_view(argv[++i]), o.maxCorrections) || o.maxCorrections < 0)
                return false;
        }
        else
        {
            std::fprintf(stderr, "unknown or incomplete argument '%.*s'\n", static_cast<int>(a.size()), a.data());
            return false;
        }
    }
    if (o.scenario.empty() && o.bots > 0)
        o.scenario = "load";
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
    return static_cast<std::uint32_t>(std::chrono::duration_cast<Ms>(Clock::now().time_since_epoch()).count());
}

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
    for (int i = 0; i < 9; ++i)
        s.push_back(static_cast<char>('a' + letter(rd)));
    return s;
}

namespace {

// ---- Scenarios without a world ----

std::string scenarioPing(const Context& c)
{
    Bot bot(c.opts.timeout);
    bot.connectOrFail(c.settings);
    double total = 0.0;
    double worst = 0.0;
    for (int i = 0; i < c.opts.count; ++i)
    {
        const auto sent = Clock::now();
        const std::uint32_t stamp = nowMs() + static_cast<std::uint32_t>(i);
        bot.send(vn::Ping{stamp});
        const auto pong = bot.expect<vn::Pong>();
        const double rtt = std::chrono::duration<double, std::milli>(Clock::now() - sent).count();
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

using ScenarioFn = std::string (*)(const Context&);
struct ScenarioEntry
{
    std::string_view name;
    ScenarioFn fn;
    bool needsTerrain;
};
constexpr ScenarioEntry kScenarios[] = {
    {"ping", &scenarioPing, false},
    {"login", &scenarioLogin, false},
    {"chars", &scenarioChars, false},
    {"world", &scenarioWorld, true},
    {"speedhack", &scenarioSpeedhack, true},
    {"wallhack", &scenarioWallhack, true},
    {"floodmove", &scenarioFloodMove, true},
    {"leaverace", &scenarioLeaveRace, true},
    {"dupworld", &scenarioDupWorld, true},
    {"aoi", &scenarioAoi, true},
    {"persist", &scenarioPersist, true},
    {"items", &scenarioItems, true},
    {"combat", &scenarioCombat, true},
    {"loot", &scenarioLoot, true},
    {"stats", &scenarioStats, true},
    {"skills", &scenarioSkills, true},
    {"zones", &scenarioZones, true},
    {"shop", &scenarioShop, true},
    {"shoperrors", &scenarioShopErrors, true},
    {"zonesrace", &scenarioZonesRace, true},
    {"badstate", &scenarioBadState, false},
    {"garbage", &scenarioGarbage, false},
    {"wrongkey", &scenarioWrongKey, false},
    {"waitshutdown", &scenarioWaitShutdown, false},
};

bool needsTerrain(std::string_view scenario)
{
    if (scenario == "load")
        return true;
    for (const auto& e : kScenarios)
        if (e.name == scenario)
            return e.needsTerrain;
    return false;
}

int runScenario(const Context& c)
{
    if (c.opts.scenario == "load")
    {
        if (c.opts.bots <= 0)
        {
            std::fprintf(stderr, "load mode needs --bots <n>\n");
            return kExitUsage;
        }
        return runLoadTest(c);
    }
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

} // namespace

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
        std::fprintf(stderr, "--scenario (or --bots) is required\n");
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

    if (needsTerrain(opts.scenario))
    {
        std::optional<std::filesystem::path> terrainPath;
        if (opts.terrainPath)
            terrainPath = vo::core::pathFromUtf8(*opts.terrainPath);
        else
            terrainPath = vo::core::locateDataFile("assets/terrain/zone1/zone1.vterrain");
        if (!terrainPath)
        {
            std::fprintf(stderr, "zone 1 terrain not found: pass --terrain <zone1.vterrain>\n");
            return kExitUsage;
        }
        auto terrain = std::make_shared<assets::TerrainData>();
        std::string error;
        if (!assets::loadTerrain(*terrainPath, *terrain, error))
        {
            std::fprintf(stderr, "cannot load terrain: %s\n", error.c_str());
            return kExitUsage;
        }
        c.terrain = std::move(terrain);
        // Zones (docs/PROTOCOL_ZONES.md): every zone of gamedata/zones.toml, from the same assets
        // directory (<assets>/terrain/zone1/zone1.vterrain -> <assets>/<terrain of the zone>).
        c.zoneTerrains[1] = c.terrain;
        const bool zonesNeeded = opts.scenario == "zones" || opts.scenario == "shop" || opts.scenario == "shoperrors" ||
                                 opts.scenario == "zonesrace" || (opts.scenario == "load" && opts.zones);
        if (!vo::game::zones().empty())
        {
            const auto root = std::filesystem::absolute(*terrainPath).parent_path().parent_path().parent_path();
            for (const auto& z : vo::game::zones().zones())
            {
                if (z.id == 1)
                    continue;
                auto t = std::make_shared<assets::TerrainData>();
                std::string zoneError;
                if (assets::loadTerrain(root / vo::core::pathFromUtf8(z.terrain), *t, zoneError))
                    c.zoneTerrains[z.id] = std::move(t);
                else if (zonesNeeded)
                {
                    std::fprintf(stderr, "cannot load the terrain of zone %u: %s\n", z.id, zoneError.c_str());
                    return kExitUsage;
                }
            }
        }
        else if (zonesNeeded)
        {
            std::fprintf(stderr, "gamedata/zones.toml not found or invalid\n");
            return kExitUsage;
        }

        // Item templates: the load mode's item bots and the items scenario plan legal moves with
        // the shared rules. Optional for the other world scenarios.
        if (const auto itemsPath = vo::game::locateItemsFile())
        {
            auto catalog = std::make_shared<vo::game::ItemCatalog>();
            std::string itemsError;
            if (!vo::game::loadItemCatalog(*itemsPath, *catalog, itemsError))
            {
                std::fprintf(stderr, "invalid item templates: %s\n", itemsError.c_str());
                return kExitUsage;
            }
            c.items = std::move(catalog);
        }
        else if (opts.scenario == "items" || opts.scenario == "load" || opts.scenario == "stats" ||
                 opts.scenario == "skills" || opts.scenario == "shop" || opts.scenario == "shoperrors")
        {
            std::fprintf(stderr, "gamedata/items.toml not found (searched upwards from cwd and exe dir)\n");
            return kExitUsage;
        }
        // Monsters and safe zones: the combat scenario and the load mode's fighters / village walkers.
        if (const auto monstersPath = vo::game::locateMonstersFile())
        {
            auto data = std::make_shared<vo::game::MonsterData>();
            std::string monstersError;
            if (!vo::game::loadMonsterData(*monstersPath, *data, monstersError))
            {
                std::fprintf(stderr, "invalid monster data: %s\n", monstersError.c_str());
                return kExitUsage;
            }
            c.monsters = std::move(data);
        }
        else if (opts.scenario == "combat" || opts.scenario == "loot" || opts.scenario == "stats" ||
                 opts.scenario == "skills" || opts.scenario == "shoperrors")
        {
            std::fprintf(stderr, "gamedata/monsters.toml not found (searched upwards from cwd and exe dir)\n");
            return kExitUsage;
        }
    }

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
