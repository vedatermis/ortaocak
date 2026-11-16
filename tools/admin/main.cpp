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
#include "shared/game/Character.h"
#include "shared/game/Combat.h"
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
        "  give-item <character> <template_id> [count]\n"
        "                                       new item(s) in the bag of a live character (template from\n"
        "                                       gamedata/items.toml; stackables fill existing stacks first,\n"
        "                                       all or nothing; a character in the world sees them after\n"
        "                                       leaving and entering again)\n"
        "  list-items <character>               every item of a live character, and its Akçe\n"
        "  set-coins <character> <amount>       the Akçe balance (0..1000000000); a character in the world\n"
        "                                       sees it on its next entry\n"
        "  set-level <character> <level>        testing: level clamped to 1..10, xp 0, hp/mp full on the next\n"
        "                                       entry (a character in the world overwrites it when it leaves);\n"
        "                                       spent stat / skill points that no longer fit a lower level are reset\n"
        "  reset-stats <character>              every spent stat point back to unspent (a character in the\n"
        "                                       world keeps its live points until its next entry)\n"
        "  reset-skills <character>             every skill back to rank 0 (the free skill to 1), default skill\n"
        "                                       bar (a character in the world keeps its ranks until its next entry)\n"
        "  show-character <character>           level, xp, attributes, stat points, skill ranks and bar, hp/mp, Akçe\n"
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

int cmdGiveItem(va::PostgresAccountService& svc, const std::string& character, const std::string& templateText,
                const std::string& countText)
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
    std::uint32_t count = 1;
    if (!countText.empty() &&
        (!parseId(countText, count) || count > static_cast<std::uint32_t>(t.maxStack) * vo::game::kBagSlots))
    {
        std::fprintf(stderr, "give-item: invalid count '%s' (1..%u for '%s', max_stack %u)\n", countText.c_str(),
                     static_cast<unsigned>(t.maxStack) * static_cast<unsigned>(vo::game::kBagSlots), t.name.c_str(),
                     t.maxStack);
        return kExitRefused;
    }
    std::vector<vo::game::StoredItem> changed;
    std::string error;
    const AR r = svc.giveItems(character, *catalog, t.id, count, changed, error);
    if (r == AR::NoFreeSlot)
    {
        std::fprintf(stderr, "give-item: %u x '%s' do not fit into the bag of '%s' (nothing given)\n", count,
                     t.name.c_str(), character.c_str());
        return kExitRefused;
    }
    if (r != AR::Ok)
        return report(r, error, "give-item");
    std::printf("gave %u x %u %s to '%s':\n", count, t.id, t.name.c_str(), character.c_str());
    for (const auto& c : changed)
        std::printf("  item %llu in %s, now %u\n", static_cast<unsigned long long>(c.id), placeName(c.place).c_str(),
                    c.quantity);
    std::printf("note: if '%s' is in the world now, the items show up after leaving and entering again\n",
                character.c_str());
    return kExitOk;
}

int cmdSetCoins(va::PostgresAccountService& svc, const std::string& character, const std::string& amountText)
{
    std::uint32_t amount = 0;
    const auto [p, ec] = std::from_chars(amountText.data(), amountText.data() + amountText.size(), amount);
    if (ec != std::errc{} || p != amountText.data() + amountText.size() || amount > vo::net::kMaxCoins)
    {
        std::fprintf(stderr, "set-coins: invalid amount '%s' (0..%u)\n", amountText.c_str(), vo::net::kMaxCoins);
        return kExitRefused;
    }
    va::CharacterInfo info;
    std::string error;
    const AR r = svc.setCoins(character, amount, info, error);
    if (r != AR::Ok)
        return report(r, error, "set-coins");
    std::printf("character %u '%s' now has %u Akçe\n", info.id, info.name.c_str(), info.coins);
    std::printf("note: if '%s' is in the world now, it sees the new balance on its next entry\n", character.c_str());
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
    std::printf("character %u '%s' (class %u, level %u), starter kit %s, %u Akçe\n", info.id, info.name.c_str(),
                info.charClass, info.level, info.starterKitGranted ? "granted" : "not granted yet", info.coins);
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

int cmdSetLevel(va::PostgresAccountService& svc, const std::string& character, const std::string& levelText)
{
    std::uint32_t level = 0;
    if (!parseId(levelText, level) || level == 0 || level > 65535)
    {
        std::fprintf(stderr, "set-level: invalid level '%s' (1..%u)\n", levelText.c_str(), vo::game::kLevelCap);
        return kExitRefused;
    }
    va::CharacterInfo before;
    std::string error;
    if (const AR r = svc.findCharacterByName(character, before, error); r != AR::Ok)
        return report(r, error, "set-level");
    va::CharacterInfo info;
    bool skillsReset = false;
    const AR r = svc.setLevel(character, static_cast<std::uint16_t>(level), info, error, &skillsReset);
    if (r != AR::Ok)
        return report(r, error, "set-level");
    std::printf("character %u '%s' is now level %u (xp 0, hp/mp full on the next entry)%s\n", info.id, info.name.c_str(),
                info.level, level > vo::game::kLevelCap ? " - clamped to the level cap" : "");
    const auto spentBefore = vo::game::allocatedTotal(before.statPoints);
    if (spentBefore > 0 && vo::game::allocatedTotal(info.statPoints) == 0)
        std::printf("stat points: the %u spent point(s) did not fit level %u and were reset\n", spentBefore, info.level);
    std::printf("stat points: %u unspent\n", vo::game::unspentPoints(info.level, info.statPoints));
    if (skillsReset) // docs/PROTOCOL_SKILLS.md §6.2
        std::printf("skill points: the spent skill points did not fit level %u; skills were reset (default bar)\n",
                    info.level);
    std::printf("note: if '%s' is in the world now, its next save (leaving, every 60 s) overwrites this\n",
                character.c_str());
    return kExitOk;
}

// docs/PROTOCOL_STATS.md §6.2.
int cmdResetStats(va::PostgresAccountService& svc, const std::string& character)
{
    va::CharacterInfo before;
    std::string error;
    if (const AR r = svc.findCharacterByName(character, before, error); r != AR::Ok)
        return report(r, error, "reset-stats");
    va::CharacterInfo info;
    const AR r = svc.resetStats(character, info, error);
    if (r != AR::Ok)
        return report(r, error, "reset-stats");
    std::printf("character %u '%s' (level %u): %u spent stat point(s) reset, %u unspent now\n", info.id,
                info.name.c_str(), info.level, vo::game::allocatedTotal(before.statPoints),
                vo::game::unspentPoints(info.level, info.statPoints));
    std::printf("note: if '%s' is in the world now, it keeps its live points until its next entry or allocation\n",
                character.c_str());
    return kExitOk;
}

// docs/PROTOCOL_SKILLS.md §6.2.
int cmdResetSkills(va::PostgresAccountService& svc, const std::string& character)
{
    va::CharacterInfo before;
    std::string error;
    if (const AR r = svc.loadSkills(character, before, error); r != AR::Ok)
        return report(r, error, "reset-skills");
    va::CharacterInfo info;
    const AR r = svc.resetSkills(character, info, error);
    if (r != AR::Ok)
        return report(r, error, "reset-skills");
    const vo::game::SkillCatalog& catalog = vo::game::skills();
    std::printf("character %u '%s' (level %u): %u spent skill point(s) reset, %u unspent now; default skill bar\n",
                info.id, info.name.c_str(), info.level,
                vo::game::skillPointsSpent(catalog, before.charClass, before.skillRanks),
                vo::game::unspentSkillPoints(catalog, info.charClass, info.level, info.skillRanks));
    std::printf("note: if '%s' is in the world now, it keeps its live ranks until its next entry or learn\n",
                character.c_str());
    return kExitOk;
}

int cmdShowCharacter(va::PostgresAccountService& svc, const std::string& character)
{
    va::CharacterInfo info;
    std::string error;
    if (const AR r = svc.loadSkills(character, info, error); r != AR::Ok)
        return report(r, error, "show-character");
    namespace vg = vo::game;
    const auto cls = vg::isValidClass(info.charClass) ? vg::className(static_cast<vg::CharClass>(info.charClass)) : "?";
    const auto race = vg::isValidRace(info.race) ? vg::raceName(static_cast<vg::Race>(info.race)) : "?";
    std::printf("character %u '%s'  %s %s (class %u, race %u)\n", info.id, info.name.c_str(),
                std::string(race).c_str(), std::string(cls).c_str(), info.charClass, info.race);
    std::printf("level %u, xp %u / %u, hp %s, mp %s, %u Akçe, zone %u at (%.1f, %.1f)\n", info.level, info.xp,
                vg::xpToNext(info.level), info.hp ? std::to_string(*info.hp).c_str() : "full",
                info.mp ? std::to_string(*info.mp).c_str() : "full", info.coins, info.position.zoneId, info.position.x,
                info.position.z);
    const vg::Attributes start = vg::classStartAttributes(info.charClass);
    std::printf("%-14s %5s %9s %5s\n", "attribute", "start", "allocated", "base");
    for (std::size_t i = 0; i < vg::kAttributeCount; ++i)
    {
        const auto a = static_cast<vg::Attribute>(i);
        std::printf("%-14s %5u %9u %5u\n", std::string(vg::attributeKey(a)).c_str(), start[i], info.statPoints[i],
                    static_cast<unsigned>(start[i] + info.statPoints[i]));
    }
    std::printf("stat points: %u of %u spent, %u unspent\n", vg::allocatedTotal(info.statPoints),
                vg::pointsForLevel(info.level), vg::unspentPoints(info.level, info.statPoints));
    // docs/PROTOCOL_SKILLS.md §6.2: skill points, ranks and the bar.
    const vg::SkillCatalog& catalog = vg::skills();
    if (catalog.empty())
        std::printf("skills: %s could not be loaded (%s); raw rows only\n", std::string(vg::kSkillsFile).c_str(),
                    vg::defaultSkillCatalogError().c_str());
    const vg::SkillRanks ranks = vg::normalizeSkillRanks(catalog, info.charClass, info.skillRanks);
    std::printf("skill points: %u of %u spent, %u unspent\n", vg::skillPointsSpent(catalog, info.charClass, ranks),
                vg::skillPointsForLevel(catalog, info.level),
                vg::unspentSkillPoints(catalog, info.charClass, info.level, ranks));
    for (std::uint16_t id = 1; id <= vg::kMaxSkillId; ++id)
    {
        const vg::SkillDef* s = catalog.find(id);
        const unsigned rank = s ? ranks[id] : info.skillRanks[id];
        if (rank == 0)
            continue;
        if (s)
            std::printf("  skill %2u %-16s %-8s tier %u  rank %u/3%s\n", id, s->name.c_str(),
                        std::string(catalog.branchName(s->charClass, s->branch)).c_str(), s->tier, rank,
                        s->free ? " (free)" : "");
        else
            std::printf("  skill %2u (unknown)  rank %u\n", id, rank);
    }
    const vg::SkillBar bar = info.skillBar.value_or(vg::defaultSkillBar(catalog, info.charClass));
    std::printf("skill bar%s:", info.skillBar ? "" : " (default)");
    for (std::size_t i = 0; i < bar.size(); ++i)
    {
        const vg::SkillDef* s = bar[i] ? catalog.find(bar[i]) : nullptr;
        std::printf(" [%zu] %s", i + 1, bar[i] == 0 ? "-" : (s ? s->name.c_str() : std::to_string(bar[i]).c_str()));
    }
    std::printf("\n");
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

    if (cmd == "give-item" && (w.size() == 3 || w.size() == 4))
        return cmdGiveItem(svc, w[1], w[2], w.size() == 4 ? w[3] : std::string());
    if (cmd == "set-coins" && w.size() == 3)
        return cmdSetCoins(svc, w[1], w[2]);
    if (cmd == "list-items" && w.size() == 2)
        return cmdListItems(svc, w[1]);
    if (cmd == "reset-stats" && w.size() == 2)
        return cmdResetStats(svc, w[1]);
    if (cmd == "reset-skills" && w.size() == 2)
        return cmdResetSkills(svc, w[1]);
    if (cmd == "show-character" && w.size() == 2)
        return cmdShowCharacter(svc, w[1]);
    if (cmd == "set-level" && w.size() == 3)
        return cmdSetLevel(svc, w[1], w[2]);
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
