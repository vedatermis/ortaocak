// Low-level PostgreSQL helpers shared by the pool, the migration runner, the account service and
// vo_admin: opening a configured connection, creating a missing database, error sanitising and the
// "no DB calls on this thread" guard for the network thread. pqxx stays out of this header.
#pragma once

#include "server/db/DbSettings.h"

#include <exception>
#include <memory>
#include <string>
#include <string_view>

namespace pqxx {
class connection;
}

namespace vo::server::db {

struct ConnectionDeleter
{
    void operator()(pqxx::connection* c) const noexcept;
};
using ConnectionPtr = std::unique_ptr<pqxx::connection, ConnectionDeleter>;

// Opens a connection to `connectionString` and configures the session: UTC time zone, UTF8 client
// encoding, statement_timeout and search_path from `settings` (pass statementTimeoutOverride >= 0
// to replace the statement timeout, e.g. 0 for migrations). Returns null and sets `error` (no
// secrets) on failure. Never throws.
[[nodiscard]] ConnectionPtr openConnection(const DbSettings& settings, std::string_view connectionString,
                                           std::string& error, long long statementTimeoutOverrideMs = -1);

// Runs a trivial query; false if the connection is unusable. Never throws.
[[nodiscard]] bool pingConnection(pqxx::connection& conn) noexcept;

// Creates the database named in settings.connectionString through settings.adminConnectionString
// if it does not exist yet (UTF8, template0). Does nothing else on the admin connection. Returns
// false on error; `created` tells whether CREATE DATABASE ran.
[[nodiscard]] bool ensureDatabaseExists(const DbSettings& settings, bool& created, std::string& error);

struct ServerInfo
{
    std::string version; // e.g. "PostgreSQL 16.4 ..."
    std::string database;
    std::string schema;
    bool ssl = false;
    std::string sslVersion; // empty without SSL
};
[[nodiscard]] bool queryServerInfo(pqxx::connection& conn, ServerInfo& out, std::string& error) noexcept;

// One-line, secret-free description of a DB exception: the primary message (no DETAIL lines,
// which can quote row contents such as password hashes) plus the SQLSTATE if any.
[[nodiscard]] std::string describeDbError(const std::exception& e);

// True for errors that mean the connection itself is gone (the job may be retried on a new one).
[[nodiscard]] bool isConnectionError(const std::exception& e, const pqxx::connection* conn) noexcept;

// The network (io_context) thread calls markThreadNoDb() once; any DB access attempted from it
// afterwards is a programming error (VO_ASSERT in Debug, error log + failure in Release).
void markThreadNoDb() noexcept;
[[nodiscard]] bool isThreadNoDb() noexcept;

} // namespace vo::server::db
