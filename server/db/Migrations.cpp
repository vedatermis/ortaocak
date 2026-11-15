#include "server/db/Migrations.h"

#include "server/db/Db.h"
#include "server/db/DbSettings.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"

#include <pqxx/pqxx>
#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace vo::server::db {

namespace fs = std::filesystem;

namespace {

// pg_advisory_lock(classid, objid): classid "VOMG", objid = hashtext(schema).
constexpr int kAdvisoryLockClass = 0x564F4D47;

std::string sha256Hex(std::string_view data)
{
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(digest) * 2);
    for (const unsigned char b : digest)
    {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string normaliseLineEndings(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')
            continue;
        out.push_back(raw[i]);
    }
    // A UTF-8 BOM would end up inside the first statement.
    if (out.starts_with("\xEF\xBB\xBF"))
        out.erase(0, 3);
    return out;
}

// "0001_init.sql" -> (1, "init")
bool parseFileName(const std::string& file, int& version, std::string& name)
{
    if (file.size() < 10 || !file.ends_with(".sql") || file[4] != '_')
        return false;
    for (int i = 0; i < 4; ++i)
    {
        if (file[static_cast<std::size_t>(i)] < '0' || file[static_cast<std::size_t>(i)] > '9')
            return false;
    }
    const auto [ptr, ec] = std::from_chars(file.data(), file.data() + 4, version);
    if (ec != std::errc{} || version <= 0)
        return false;
    name = file.substr(5, file.size() - 5 - 4);
    return isSafeIdentifier(name);
}

std::string label(int version, std::string_view name)
{
    return std::format("{:04}_{}", version, name);
}

std::vector<AppliedMigration> readApplied(pqxx::transaction_base& tx)
{
    std::vector<AppliedMigration> out;
    const bool exists = tx.query_value<bool>("SELECT to_regclass('schema_migrations') IS NOT NULL");
    if (!exists)
        return out;
    for (const auto& row : tx.exec("SELECT version, name, checksum, "
                                   "to_char(applied_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
                                   "FROM schema_migrations ORDER BY version"))
    {
        AppliedMigration a;
        a.version = row[0].as<int>();
        a.name = row[1].as<std::string>();
        a.checksum = row[2].as<std::string>();
        a.appliedAt = row[3].as<std::string>();
        out.push_back(std::move(a));
    }
    return out;
}

void compare(const std::vector<Migration>& files, MigrationStatus& st)
{
    std::map<int, const Migration*> byVersion;
    for (const auto& m : files)
        byVersion[m.version] = &m;
    std::set<int> applied;
    for (const auto& a : st.applied)
    {
        applied.insert(a.version);
        st.currentVersion = std::max(st.currentVersion, a.version);
        const auto it = byVersion.find(a.version);
        if (it == byVersion.end())
            st.problems.push_back(std::format("applied migration {} has no file (database is newer than this build?)",
                                              label(a.version, a.name)));
        else if (it->second->checksum != a.checksum)
            st.problems.push_back(std::format("checksum mismatch for applied migration {}: the file was changed "
                                              "after it was applied (add a new migration instead)",
                                              label(a.version, it->second->name)));
    }
    for (const auto& m : files)
    {
        if (applied.contains(m.version))
            continue;
        st.pending.push_back(m.version);
        if (m.version < st.currentVersion)
            st.problems.push_back(std::format("migration {} is older than the applied version {:04} (out of order)",
                                              label(m.version, m.name), st.currentVersion));
    }
}

} // namespace

Migration makeMigration(int version, std::string name, std::string_view rawSql)
{
    Migration m;
    m.version = version;
    m.name = std::move(name);
    m.sql = normaliseLineEndings(rawSql);
    m.checksum = sha256Hex(m.sql);
    return m;
}

bool loadMigrations(const fs::path& dir, std::vector<Migration>& out, std::string& error)
{
    out.clear();
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
    {
        error = std::format("migrations directory '{}' not found", core::pathToUtf8(dir));
        return false;
    }
    std::map<int, Migration> found;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string file = core::pathToUtf8(entry.path().filename());
        if (!file.ends_with(".sql"))
            continue;
        int version = 0;
        std::string name;
        if (!parseFileName(file, version, name))
        {
            error = std::format("migration file '{}' does not match NNNN_lower_case_name.sql", file);
            return false;
        }
        if (found.contains(version))
        {
            error = std::format("duplicate migration version {:04} ('{}')", version, file);
            return false;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in)
        {
            error = std::format("cannot read migration '{}'", core::pathToUtf8(entry.path()));
            return false;
        }
        const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        found.emplace(version, makeMigration(version, std::move(name), raw));
    }
    if (ec)
    {
        error = std::format("cannot list '{}': {}", core::pathToUtf8(dir), ec.message());
        return false;
    }
    for (auto& [v, m] : found)
        out.push_back(std::move(m));
    return true;
}

std::optional<fs::path> locateMigrationsDir()
{
    // locateDataFile finds files only; the first migration exists forever, so anchor on it.
    if (auto first = core::locateDataFile("db/migrations/0001_init.sql"))
        return first->parent_path();
    return std::nullopt;
}

bool readMigrationStatus(pqxx::connection& conn, const std::vector<Migration>& migrations, MigrationStatus& out,
                         std::string& error)
{
    try
    {
        pqxx::read_transaction tx(conn);
        MigrationStatus st;
        st.applied = readApplied(tx);
        compare(migrations, st);
        tx.commit();
        out = std::move(st);
        return true;
    }
    catch (const std::exception& e)
    {
        error = describeDbError(e);
        return false;
    }
}

bool runMigrations(pqxx::connection& conn, const std::string& schema, const std::vector<Migration>& migrations,
                   MigrationReport& report, std::string& error)
{
    report = {};
    if (!isSafeIdentifier(schema))
    {
        error = std::format("schema '{}' is not a lower-case identifier", schema);
        return false;
    }
    bool locked = false;
    const auto unlock = [&] {
        if (!locked)
            return;
        try
        {
            pqxx::nontransaction tx(conn);
            tx.exec("SELECT pg_advisory_unlock($1, hashtext($2))", pqxx::params{kAdvisoryLockClass, schema});
        }
        catch (const std::exception&) // the lock dies with the session anyway
        {
        }
        locked = false;
    };

    std::string step = "preparing";
    try
    {
        {
            pqxx::nontransaction tx(conn);
            const auto encoding = tx.query_value<std::string>("SHOW server_encoding");
            if (encoding != "UTF8")
            {
                error = std::format("database encoding is {}, UTF8 is required", encoding);
                return false;
            }
            step = "waiting for the migration lock";
            tx.exec("SELECT pg_advisory_lock($1, hashtext($2))", pqxx::params{kAdvisoryLockClass, schema});
            locked = true;
            step = "creating schema_migrations";
            tx.exec(std::format("CREATE SCHEMA IF NOT EXISTS {}", conn.quote_name(schema)));
            tx.exec(std::format("CREATE TABLE IF NOT EXISTS {}.schema_migrations ("
                                "version integer PRIMARY KEY CHECK (version > 0), "
                                "name text NOT NULL, "
                                "checksum text NOT NULL CHECK (checksum ~ '^[0-9a-f]{{64}}$'), "
                                "applied_at timestamptz NOT NULL DEFAULT now())",
                                conn.quote_name(schema)));
        }

        MigrationStatus st;
        {
            pqxx::read_transaction tx(conn);
            st.applied = readApplied(tx);
            tx.commit();
        }
        compare(migrations, st);
        if (!st.problems.empty())
        {
            error = st.problems.front();
            for (std::size_t i = 1; i < st.problems.size(); ++i)
                error += "; " + st.problems[i];
            unlock();
            return false;
        }

        report.currentVersion = st.currentVersion;
        for (const auto& m : migrations)
        {
            if (std::find(st.pending.begin(), st.pending.end(), m.version) == st.pending.end())
                continue;
            step = "applying migration " + label(m.version, m.name);
            pqxx::work tx(conn);
            tx.exec(m.sql);
            tx.exec("INSERT INTO schema_migrations (version, name, checksum) VALUES ($1, $2, $3)",
                    pqxx::params{m.version, m.name, m.checksum});
            tx.commit();
            report.applied.push_back(m.version);
            report.currentVersion = m.version;
            VO_LOG_INFO("db: applied migration {}", label(m.version, m.name));
        }
        unlock();
        return true;
    }
    catch (const std::exception& e)
    {
        error = std::format("{}: {}", step, describeDbError(e));
        unlock();
        return false;
    }
}

bool migrateDatabase(const DbSettings& settings, const std::optional<fs::path>& dir, MigrationReport& report,
                     std::string& error)
{
    fs::path migrationsDir;
    if (dir)
        migrationsDir = *dir;
    else if (auto found = locateMigrationsDir())
        migrationsDir = *found;
    else
    {
        error = "db/migrations not found (searched upwards from the working and executable directories)";
        return false;
    }
    std::vector<Migration> migrations;
    if (!loadMigrations(migrationsDir, migrations, error))
        return false;

    // No statement timeout: a migration may legitimately run long, and the lock wait must not time out.
    auto conn = openConnection(settings, settings.connectionString, error, 0);
    if (!conn)
    {
        error = "cannot connect to the database: " + error;
        return false;
    }
    if (!runMigrations(*conn, settings.schema, migrations, report, error))
        return false;
    if (report.applied.empty())
        VO_LOG_INFO("db: schema is up to date (version {:04}, {} migration file(s))", report.currentVersion,
                    migrations.size());
    else
        VO_LOG_INFO("db: schema migrated to version {:04} ({} migration(s) applied)", report.currentVersion,
                    report.applied.size());
    return true;
}

} // namespace vo::server::db
