// Forward-only SQL migrations (db/migrations/NNNN_name.sql, docs/DATABASE.md).
//
// - Applied in version order, each in its own transaction together with its schema_migrations row
//   (version, name, checksum, applied_at).
// - The checksum is the SHA-256 of the file with CRLF normalised to LF; a changed checksum of an
//   applied migration, an applied version missing from the files, or a new file older than the
//   newest applied version is a hard error (nothing is applied).
// - Concurrent runners (two servers, server + vo_admin) are serialised with a session-level
//   advisory lock keyed by the current schema; the loser waits and then finds nothing to do.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pqxx {
class connection;
}

namespace vo::server::db {

struct DbSettings;

struct Migration
{
    int version = 0;
    std::string name; // "init" for 0001_init.sql
    std::string sql;  // normalised (LF line endings)
    std::string checksum; // lower-case hex SHA-256 of `sql`
};

// Builds a migration from raw file contents (normalises line endings, computes the checksum).
[[nodiscard]] Migration makeMigration(int version, std::string name, std::string_view rawSql);

// Loads every NNNN_name.sql in `dir` (other files are ignored), sorted by version. Fails on a
// malformed .sql name, duplicate versions or unreadable files.
[[nodiscard]] bool loadMigrations(const std::filesystem::path& dir, std::vector<Migration>& out,
                                  std::string& error);

// db/migrations searched upwards from the working directory and the executable directory.
[[nodiscard]] std::optional<std::filesystem::path> locateMigrationsDir();

struct AppliedMigration
{
    int version = 0;
    std::string name;
    std::string checksum;
    std::string appliedAt; // ISO-8601 UTC
};

struct MigrationStatus
{
    std::vector<AppliedMigration> applied; // ascending
    std::vector<int> pending;              // versions not yet applied, ascending
    int currentVersion = 0;                // highest applied, 0 = none
    std::vector<std::string> problems;     // checksum mismatches, unknown/out-of-order versions
};

// Reads the state without changing anything (schema_migrations may not exist yet).
[[nodiscard]] bool readMigrationStatus(pqxx::connection& conn, const std::vector<Migration>& migrations,
                                       MigrationStatus& out, std::string& error);

struct MigrationReport
{
    std::vector<int> applied; // versions applied by this run
    int currentVersion = 0;
};

// Creates schema (must be the connection's search_path) and schema_migrations if needed, takes the
// advisory lock, verifies and applies pending migrations. Requires a UTF8 database.
[[nodiscard]] bool runMigrations(pqxx::connection& conn, const std::string& schema,
                                 const std::vector<Migration>& migrations, MigrationReport& report,
                                 std::string& error);

// Convenience for vo_server / vo_admin: loads the migrations (from `dir` or locateMigrationsDir()),
// opens a dedicated connection without statement timeout, migrates and logs what was applied.
[[nodiscard]] bool migrateDatabase(const DbSettings& settings, const std::optional<std::filesystem::path>& dir,
                                   MigrationReport& report, std::string& error);

} // namespace vo::server::db
