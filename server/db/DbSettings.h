// Database settings ([db] in config/server.toml + server.local.toml) and connection-string helpers
// that never expose the password (docs/DATABASE.md).
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace vo::core {
class Config;
}

namespace vo::server::db {

struct DbSettings
{
    std::string connectionString;      // db.connection_string (env VEDAT_DB_URL); empty = no database
    std::string adminConnectionString; // db.admin_connection_string: only used to create a missing database
    std::string schema = "public";     // db.schema: search_path for every pooled connection
    std::size_t poolSize = 4;          // db.pool_size
    std::size_t workerThreads = 4;     // db.worker_threads (DB jobs incl. Argon2 hashing)
    std::chrono::milliseconds statementTimeout{5000}; // db.statement_timeout_ms (0 = none)
    std::chrono::seconds connectTimeout{5};           // db.connect_timeout_s
    std::chrono::milliseconds acquireTimeout{5000};   // waiting for a free pooled connection
    std::chrono::milliseconds healthCheckIdle{10000}; // ping a connection idle longer than this
    bool required = true;                             // db.required: fail startup if unreachable
    std::string applicationName = "vo_server";        // shown in pg_stat_activity
};

// Binds the environment overrides (VEDAT_DB_URL, VEDAT_DB_ADMIN_URL) on a Config before loading.
void bindDbEnv(core::Config& config);

// Reads [db]. Returns false (with `error`) on out-of-range values or an invalid schema name.
[[nodiscard]] bool loadDbSettings(const core::Config& config, DbSettings& out, std::string& error);

// "user@host:port/dbname" (or "<unparseable connection string>"); never contains the password.
[[nodiscard]] std::string describeConnectionString(std::string_view connectionString);

// The dbname a connection string refers to (nullopt if unparseable or not given).
[[nodiscard]] std::optional<std::string> connectionDbName(std::string_view connectionString);

// Lower-case identifier [a-z_][a-z0-9_]{0,62}: the only database/schema names we create or use.
[[nodiscard]] bool isSafeIdentifier(std::string_view name) noexcept;

} // namespace vo::server::db
