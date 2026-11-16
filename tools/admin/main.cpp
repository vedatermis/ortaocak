// vo_admin - database administration for VedatOnline (docs/DATABASE.md).
//
//   vo_admin [--config <path/server.toml>] [--verbose] <command> ...
//
// Uses the same configuration as vo_server (config/server.toml + server.local.toml, VEDAT_DB_URL).
// Passwords are never taken from the command line: they are prompted without echo, or read from
// the first line of stdin with --password-stdin.
//
// Exit codes: 0 ok, 1 usage, 2 config, 3 not found / invalid input / refused, 4 database error.

#include "server/auth/PostgresAccountService.h"
#include "server/db/ConnectionPool.h"
#include "server/db/Db.h"
#include "server/db/DbSettings.h"
#include "server/db/Migrations.h"
#include "shared/core/Config.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/game/Items.h"
#include "shared/net/Crypto.h"

#include <Windows.h>
#include <shellapi.h>

#include <pqxx/pqxx>

#include <charconv>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace va = vo::server::auth;
namespace vdb = vo::server::db;
using AR = va::PostgresAccountService::AdminResult;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitConfig = 2;
constexpr int kExitRefused = 3;
constexpr int kExitDatabase = 4;

void printUsage()
{
    std::printf(
        "usage: vo_admin [--config <path/server.toml>] [--verbose] <command>\n"
        "commands:\n"
        "  migrate                              create the database if missing, apply pending migrations\n"
        "  status                               connection, schema version, account/character counts\n"
        "  account create <user> [--password-stdin]\n"
        "  account passwd <user> [--password-stdin]\n"
        "  account ban <user> [reason...]\n"
        "  account unban <user>\n"
        "  account delete <user> --yes          permanently remove the account and all its characters\n"
        "  character list <user>                all characters of an account, deleted ones included\n"
        "  character rename <user> <char_id> <new_name>\n"
        "  character restore <user> <char_id>   undo a soft delete (needs a free slot)\n"
        "  character purge <user> <char_id> --yes\n"
        "                                       permanently remove a soft-deleted character (frees its name)\n"
        "  give-item <character> <template_id>  new item in the lowest free bag slot of a live character\n"
        "                                       (template from gamedata/items.toml; a character in the\n"
        "                                       world sees it after leaving and entering again)\n"
        "  list-items <character>               every item of a live character\n"
        "  testdb drop-schema <schema>          only in vedatonline_test, only vo_test_* / vo_it_* schemas\n"
        "  testdb sweep                         drop vo_test_* / vo_it_* schemas older than two hours\n"
        "Passwords are prompted without echo; --password-stdin reads the first line of stdin instead.\n"
        "exit codes: 0 ok, 1 usage, 2 config, 3 not found/invalid/refused, 4 database error\n");
}

struct Args
{
    std::optional<std::filesystem::path> configPath;
    bool verbose = false;
    bool passwordStdin = false;
    bool yes = false;
    std::vector<std::string> words; // command and positional arguments
};

bool parseArgs(int argc, char** argv, Args& out)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--help" || a == "-h")
            return false;
        if (a == "--config" && i + 1 < argc)
            out.configPath = vo::core::pathFromUtf8(argv[++i]);
        else if (a == "--verbose")
            out.verbose = true;
        else if (a == "--password-stdin")
            out.passwordStdin = true;
        else if (a == "--yes")
            out.yes = true;
        else if (a.starts_with("--"))
        {
            std::fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return false;
        }
        else
            out.words.emplace_back(a);
    }
    return !out.words.empty();
}

std::string wideToUtf8(std::wstring_view w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Reads one line from the console with echo off. nullopt if stdin is not a console.
std::optional<std::string> promptHidden(const char* prompt)
{
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in == INVALID_HANDLE_VALUE || !GetConsoleMode(in, &mode))
        return std::nullopt;
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    SetConsoleMode(in, (mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) | ENABLE_LINE_INPUT);
    std::wstring line;
    wchar_t buf[128];
    DWORD read = 0;
    while (ReadConsoleW(in, buf, static_cast<DWORD>(std::size(buf)), &read, nullptr) && read > 0)
    {
        line.append(buf, read);
        if (line.find(L'\n') != std::wstring::npos)
            break;
    }
    SecureZeroMemory(buf, sizeof(buf));
    SetConsoleMode(in, mode);
    std::fprintf(stderr, "\n");
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r'))
        line.pop_back();
    std::string utf8 = wideToUtf8(line);
    SecureZeroMemory(line.data(), line.size() * sizeof(wchar_t));
    return utf8;
}

std::optional<std::string> readPassword(const Args& args, bool confirm)
{
    if (args.passwordStdin)
    {
        std::string line;
        if (!std::getline(std::cin, line))
            return std::nullopt;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        return line;
    }
    auto first = promptHidden("Password: ");
    if (!first)
    {
        std::fprintf(stderr, "stdin is not a console: use --password-stdin\n");
        return std::nullopt;
    }
    if (confirm)
    {
        auto second = promptHidden("Repeat password: ");
        const bool same = second && *second == *first;
        if (second)
            vo::net::secureZero(second->data(), second->size());
        if (!same)
        {
            vo::net::secureZero(first->data(), first->size());
            std::fprintf(stderr, "passwords do not match\n");
            return std::nullopt;
        }
    }
    return first;
}

bool parseId(const std::string& s, std::uint32_t& out)
{
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size() && out != 0;
}

int report(AR r, const std::string& error, const std::string& what)
{
    switch (r)
    {
    case AR::Ok: std::printf("%s: ok\n", what.c_str()); return kExitOk;
    case AR::Invalid: std::fprintf(stderr, "%s: invalid input (see docs/PROTOCOL.md 5.2/5.3)\n", what.c_str()); return kExitRefused;
    case AR::NotFound: std::fprintf(stderr, "%s: not found\n", what.c_str()); return kExitRefused;
    case AR::Taken: std::fprintf(stderr, "%s: name already taken\n", what.c_str()); return kExitRefused;
    case AR::NoFreeSlot: std::fprintf(stderr, "%s: the account has no free character slot\n", what.c_str()); return kExitRefused;
    case AR::Error: break;
    }
    std::fprintf(stderr, "%s: database error: %s\n", what.c_str(), error.c_str());
    return kExitDatabase;
}

int cmdStatus(const vdb::DbSettings& settings)
{
    std::string error;
    auto conn = vdb::openConnection(settings, settings.connectionString, error);
    if (!conn)
    {
        std::fprintf(stderr, "cannot connect: %s\n", error.c_str());
        return kExitDatabase;
    }
    vdb::ServerInfo info;
    if (!vdb::queryServerInfo(*conn, info, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return kExitDatabase;
    }
    std::printf("database    %s (schema %s)\n", vdb::describeConnectionString(settings.connectionString).c_str(),
                settings.schema.c_str());
    std::printf("server      %s\n", info.version.c_str());
    std::printf("ssl         %s\n", info.ssl ? ("on (" + info.sslVersion + ")").c_str() : "OFF (not encrypted)");

    std::vector<vdb::Migration> migrations;
    const auto dir = vdb::locateMigrationsDir();
    if (!dir || !vdb::loadMigrations(*dir, migrations, error))
    {
        std::fprintf(stderr, "cannot load db/migrations: %s\n", dir ? error.c_str() : "not found");
        return kExitConfig;
    }
    vdb::MigrationStatus st;
    if (!vdb::readMigrationStatus(*conn, migrations, st, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return kExitDatabase;
    }
    std::printf("schema      version %04d, %zu pending of %zu migration file(s)\n", st.currentVersion,
                st.pending.size(), migrations.size());
    for (const auto& a : st.applied)
        std::printf("  %04d_%-24s applied %s\n", a.version, a.name.c_str(), a.appliedAt.c_str());
    for (const int v : st.pending)
        std::printf("  %04d pending\n", v);
    for (const auto& p : st.problems)
        std::printf("  PROBLEM: %s\n", p.c_str());
    if (st.currentVersion == 0)
        return st.problems.empty() ? kExitOk : kExitRefused;

    vdb::ConnectionPool pool(settings);
    va::PostgresAccountService svc(pool, {false, 1});
    va::PostgresAccountService::Counts counts;
    if (svc.counts(counts, error) != AR::Ok)
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return kExitDatabase;
    }
    std::printf("accounts    %lld (%lld banned)\n", static_cast<long long>(counts.accounts),
                static_cast<long long>(counts.bannedAccounts));
    std::printf("characters  %lld live, %lld deleted\n", static_cast<long long>(counts.liveCharacters),
                static_cast<long long>(counts.deletedCharacters));
    return st.problems.empty() ? kExitOk : kExitRefused;
}

// gamedata/items.toml (same search as vo_server); null + message if missing or invalid.
std::unique_ptr<vo::game::ItemCatalog> loadCatalog()
{
    const auto path = vo::game::locateItemsFile();
    if (!path)
    {
        std::fprintf(stderr, "%.*s not found (searched upwards from the working and executable directories)\n",
                     static_cast<int>(vo::game::kItemsFile.size()), vo::game::kItemsFile.data());
        return nullptr;
    }
    auto catalog = std::make_unique<vo::game::ItemCatalog>();
    std::string error;
    if (!vo::game::loadItemCatalog(*path, *catalog, error))
    {
        std::fprintf(stderr, "invalid item templates: %s\n", error.c_str());
        return nullptr;
    }
    return catalog;
}

std::string placeName(vo::game::ItemPlace p)
{
    if (p.isEquipment())
        return std::string("equip/") + std::string(vo::game::equipSlotName(static_cast<vo::game::EquipSlot>(p.slot)));
    if (p.isBag())
        return "bag/" + std::to_string(p.slot);
    return "?" + std::to_string(p.container) + "/" + std::to_string(p.slot);
}

int cmdGiveItem(va::PostgresAccountService& svc, const std::string& character, const std::string& templateText)
{
    auto catalog = loadCatalog();
    if (!catalog)
        return kExitConfig;
    std::uint32_t id = 0;
    if (!parseId(templateText, id) || id > 65535 || !catalog->find(static_cast<vo::game::ItemTemplateId>(id)))
    {
        std::fprintf(stderr, "give-item: unknown template id '%s' (see gamedata/items.toml)\n", templateText.c_str());
        return kExitRefused;
    }
    const auto& t = *catalog->find(static_cast<vo::game::ItemTemplateId>(id));
    vo::game::StoredItem given;
    std::string error;
    const AR r = svc.giveItem(character, t.id, given, error);
    if (r == AR::NoFreeSlot)
    {
        std::fprintf(stderr, "give-item: the bag of '%s' is full\n", character.c_str());
        return kExitRefused;
    }
    if (r != AR::Ok)
        return report(r, error, "give-item");
    std::printf("gave item %llu (%u %s) to '%s' in %s\n", static_cast<unsigned long long>(given.id), t.id,
                t.name.c_str(), character.c_str(), placeName(given.place).c_str());
    std::printf("note: if '%s' is in the world now, the item shows up after leaving and entering again\n",
                character.c_str());
    return kExitOk;
}

int cmdListItems(va::PostgresAccountService& svc, const std::string& character)
{
    auto catalog = loadCatalog();
    if (!catalog)
        return kExitConfig;
    va::CharacterInfo info;
    std::vector<vo::game::StoredItem> items;
    std::string error;
    const AR r = svc.listItems(character, info, items, error);
    if (r != AR::Ok)
        return report(r, error, "list-items");
    std::printf("character %u '%s' (class %u, level %u), starter kit %s\n", info.id, info.name.c_str(), info.charClass,
                info.level, info.starterKitGranted ? "granted" : "not granted yet");
    std::printf("%-16s %-12s %-8s %-4s %s\n", "place", "item_id", "template", "qty", "name");
    for (const auto& i : items)
    {
        const auto* t = catalog->find(i.templateId);
        std::printf("%-16s %-12llu %-8u %-4u %s\n", placeName(i.place).c_str(), static_cast<unsigned long long>(i.id),
                    i.templateId, i.quantity, t ? t->name.c_str() : "(unknown template: hidden in game)");
    }
    std::printf("%zu item(s)\n", items.size());
    return kExitOk;
}

int cmdTestDb(const vdb::DbSettings& settings, const std::vector<std::string>& w)
{
    std::string error;
    auto conn = vdb::openConnection(settings, settings.connectionString, error);
    if (!conn)
    {
        std::fprintf(stderr, "cannot connect: %s\n", error.c_str());
        return kExitDatabase;
    }
    try
    {
        pqxx::nontransaction tx(*conn);
        if (tx.query_value<std::string>("SELECT current_database()") != "vedatonline_test")
        {
            std::fprintf(stderr, "refused: testdb commands only run against database vedatonline_test\n");
            return kExitRefused;
        }
        const auto isTestSchema = [](const std::string& s) {
            return vdb::isSafeIdentifier(s) && (s.starts_with("vo_test_") || s.starts_with("vo_it_"));
        };
        if (w.size() == 3 && w[1] == "drop-schema")
        {
            if (!isTestSchema(w[2]))
            {
                std::fprintf(stderr, "refused: only vo_test_* / vo_it_* schemas can be dropped\n");
                return kExitRefused;
            }
            tx.exec("DROP SCHEMA IF EXISTS " + conn->quote_name(w[2]) + " CASCADE");
            std::printf("dropped schema %s\n", w[2].c_str());
            return kExitOk;
        }
        if (w.size() == 2 && w[1] == "sweep")
        {
            const auto now = tx.query_value<long long>("SELECT extract(epoch FROM now())::bigint");
            int dropped = 0;
            for (auto [name] : tx.query<std::string>("SELECT nspname FROM pg_namespace WHERE nspname ~ "
                                                     "'^vo_(test|it)_[0-9]+_'"))
            {
                const auto first = name.find('_', 3) + 1;
                const auto end = name.find('_', first);
                long long ts = 0;
                std::from_chars(name.data() + first, name.data() + end, ts);
                if (isTestSchema(name) && now - ts > 2 * 60 * 60)
                {
                    tx.exec("DROP SCHEMA IF EXISTS " + conn->quote_name(name) + " CASCADE");
                    ++dropped;
                }
            }
            std::printf("dropped %d stale test schema(s)\n", dropped);
            return kExitOk;
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s\n", vdb::describeDbError(e).c_str());
        return kExitDatabase;
    }
    printUsage();
    return kExitUsage;
}

int run(const Args& args)
{
    std::filesystem::path configPath;
    if (args.configPath)
        configPath = *args.configPath;
    else if (auto found = vo::core::locateDataFile("config/server.toml"))
        configPath = *found;
    else
    {
        std::fprintf(stderr, "config/server.toml not found; use --config\n");
        return kExitConfig;
    }
    vo::core::Config config;
    vdb::bindDbEnv(config);
    std::string error;
    if (!config.loadWithOverlay(configPath, error))
    {
        std::fprintf(stderr, "failed to load config: %s\n", error.c_str());
        return kExitConfig;
    }
    vdb::DbSettings settings;
    if (!vdb::loadDbSettings(config, settings, error))
    {
        std::fprintf(stderr, "invalid [db] configuration: %s\n", error.c_str());
        return kExitConfig;
    }
    if (settings.connectionString.empty())
    {
        std::fprintf(stderr, "db.connection_string is not configured (config/server.local.toml or VEDAT_DB_URL)\n");
        return kExitConfig;
    }
    settings.applicationName = "vo_admin";
    settings.poolSize = 1;
    settings.workerThreads = 1;
    if (!vo::net::initCrypto())
    {
        std::fprintf(stderr, "sodium_init failed\n");
        return kExitConfig;
    }

    const auto& w = args.words;
    const std::string& cmd = w[0];
    if (cmd == "migrate" && w.size() == 1)
    {
        bool created = false;
        vdb::MigrationReport rep;
        if (!vdb::ensureDatabaseExists(settings, created, error) ||
            !vdb::migrateDatabase(settings, std::nullopt, rep, error))
        {
            std::fprintf(stderr, "migrate failed: %s\n", error.c_str());
            return kExitDatabase;
        }
        if (created)
            std::printf("created database\n");
        for (const int v : rep.applied)
            std::printf("applied %04d\n", v);
        std::printf("schema version %04d (%zu applied now)\n", rep.currentVersion, rep.applied.size());
        return kExitOk;
    }
    if (cmd == "status" && w.size() == 1)
        return cmdStatus(settings);
    if (cmd == "testdb")
        return cmdTestDb(settings, w);

    vdb::ConnectionPool pool(settings);
    va::PostgresAccountService svc(pool, {false, 1});

    if (cmd == "give-item" && w.size() == 3)
        return cmdGiveItem(svc, w[1], w[2]);
    if (cmd == "list-items" && w.size() == 2)
        return cmdListItems(svc, w[1]);
    if (cmd == "account" && w.size() >= 3)
    {
        const std::string& user = w[2];
        if (w[1] == "create" && w.size() == 3)
        {
            auto pw = readPassword(args, true);
            if (!pw)
                return kExitUsage;
            va::AccountId id = 0;
            const AR r = svc.createAccount(user, *pw, id, error);
            vo::net::secureZero(pw->data(), pw->size());
            if (r == AR::Ok)
                std::printf("account %u '%s' created\n", id, user.c_str());
            return r == AR::Ok ? kExitOk : report(r, error, "account create");
        }
        if (w[1] == "passwd" && w.size() == 3)
        {
            auto pw = readPassword(args, true);
            if (!pw)
                return kExitUsage;
            const AR r = svc.setPassword(user, *pw, error);
            vo::net::secureZero(pw->data(), pw->size());
            return report(r, error, "account passwd");
        }
        if (w[1] == "ban")
        {
            std::string reason;
            for (std::size_t i = 3; i < w.size(); ++i)
                reason += (i > 3 ? " " : "") + w[i];
            return report(svc.setBannedByName(user, true, reason, error), error, "account ban");
        }
        if (w[1] == "unban" && w.size() == 3)
            return report(svc.setBannedByName(user, false, "", error), error, "account unban");
        if (w[1] == "delete" && w.size() == 3)
        {
            if (!args.yes)
            {
                std::fprintf(stderr, "account delete permanently removes the account and all its characters: "
                                     "add --yes\n");
                return kExitRefused;
            }
            std::int64_t chars = 0;
            const AR r = svc.deleteAccount(user, chars, error);
            if (r == AR::Ok)
                std::printf("account '%s' deleted with %lld character(s)\n", user.c_str(), static_cast<long long>(chars));
            return r == AR::Ok ? kExitOk : report(r, error, "account delete");
        }
    }
    if (cmd == "character" && w.size() >= 3)
    {
        const std::string& user = w[2];
        if (w[1] == "list" && w.size() == 3)
        {
            std::vector<va::PostgresAccountService::AdminCharacter> chars;
            const AR r = svc.listAllCharacters(user, chars, error);
            if (r != AR::Ok)
                return report(r, error, "character list");
            std::printf("%-8s %-16s %4s %5s %4s %4s %5s %4s  %-20s %-20s %s\n", "id", "name", "race", "class", "face",
                        "hair", "level", "zone", "created", "last_played", "deleted");
            for (const auto& c : chars)
                std::printf("%-8u %-16s %4u %5u %4u %4u %5u %4u  %-20s %-20s %s\n", c.info.id, c.info.name.c_str(),
                            c.info.race, c.info.charClass, c.info.face, c.info.hair, c.info.level,
                            c.info.position.zoneId, c.createdAt.c_str(),
                            c.lastPlayedAt.empty() ? "-" : c.lastPlayedAt.c_str(),
                            c.deleted ? c.deletedAt.c_str() : "-");
            std::size_t live = 0;
            for (const auto& c : chars)
                live += c.deleted ? 0 : 1;
            std::printf("%zu character(s), %zu live\n", chars.size(), live);
            return kExitOk;
        }
        std::uint32_t id = 0;
        if (w.size() >= 4 && !parseId(w[3], id))
        {
            std::fprintf(stderr, "invalid character id '%s'\n", w[3].c_str());
            return kExitUsage;
        }
        if (w[1] == "rename" && w.size() == 5)
            return report(svc.renameCharacter(user, id, w[4], error), error, "character rename");
        if (w[1] == "restore" && w.size() == 4)
            return report(svc.restoreCharacter(user, id, error), error, "character restore");
        if (w[1] == "purge" && w.size() == 4)
        {
            if (!args.yes)
            {
                std::fprintf(stderr, "character purge permanently deletes the character: add --yes\n");
                return kExitRefused;
            }
            return report(svc.purgeCharacter(user, id, error), error, "character purge");
        }
    }
    printUsage();
    return kExitUsage;
}

} // namespace

int main(int argc, char** ansiArgv)
{
    SetConsoleOutputCP(CP_UTF8);
    // UTF-8 arguments (character names are Turkish; argv uses the ANSI code page).
    std::vector<std::string> utf8Args;
    std::vector<char*> argvUtf8;
    if (int wargc = 0; LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc))
    {
        for (int i = 0; i < wargc; ++i)
            utf8Args.push_back(wideToUtf8(wargv[i]));
        LocalFree(wargv);
        for (auto& a : utf8Args)
            argvUtf8.push_back(a.data());
        argc = static_cast<int>(argvUtf8.size());
    }
    char** argv = argvUtf8.empty() ? ansiArgv : argvUtf8.data();
    Args args;
    if (!parseArgs(argc, argv, args))
    {
        printUsage();
        return argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") ? kExitOk
                                                                                                         : kExitUsage;
    }
    vo::log::setLevel(args.verbose ? vo::log::Level::Info : vo::log::Level::Warn);
    int code = kExitDatabase;
    try
    {
        code = run(args);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", vdb::describeDbError(e).c_str());
    }
    vo::log::flush();
    return code;
}
