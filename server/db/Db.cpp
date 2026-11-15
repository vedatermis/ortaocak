#include "server/db/Db.h"

#include "shared/core/Assert.h"
#include "shared/core/Log.h"

#include <pqxx/pqxx>

#include <format>
#include <utility>
#include <vector>

namespace vo::server::db {

namespace {

thread_local bool t_noDb = false;

std::string firstLine(std::string_view text)
{
    const auto end = text.find_first_of("\r\n");
    std::string line(text.substr(0, end));
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    if (line.size() > 300)
        line.resize(300);
    return line;
}

} // namespace

void ConnectionDeleter::operator()(pqxx::connection* c) const noexcept
{
    if (c == nullptr)
        return;
    try
    {
        c->close();
    }
    catch (const std::exception&) // closing a dead connection can throw; nothing to do about it
    {
    }
    std::default_delete<pqxx::connection>{}(c);
}

ConnectionPtr openConnection(const DbSettings& settings, std::string_view connectionString, std::string& error,
                             long long statementTimeoutOverrideMs)
{
    if (isThreadNoDb())
    {
        VO_ASSERT_MSG(false, "database access from a thread marked markThreadNoDb()");
        VO_LOG_ERROR("db: connection attempt from the network thread refused (programming error)");
        error = "database access from the network thread";
        return nullptr;
    }
    try
    {
        // Parameters given here override the same keys in the connection string.
        const std::vector<std::pair<std::string, std::string>> params = {
            {"application_name", settings.applicationName},
            {"connect_timeout", std::to_string(settings.connectTimeout.count())},
            {"client_encoding", "UTF8"},
            // Detect a vanished server/route without waiting for the OS defaults (hours).
            {"keepalives", "1"},
            {"keepalives_idle", "30"},
            {"keepalives_interval", "10"},
            {"keepalives_count", "3"},
        };
        const std::string conninfo(connectionString);
        ConnectionPtr conn(std::make_unique<pqxx::connection>(conninfo, params).release());
        const long long timeoutMs =
            statementTimeoutOverrideMs >= 0 ? statementTimeoutOverrideMs : settings.statementTimeout.count();
        // All values are validated numbers/identifiers, so composing the SET statements is safe.
        VO_VERIFY(isSafeIdentifier(settings.schema));
        pqxx::nontransaction tx(*conn);
        tx.exec(std::format("SET TimeZone = 'UTC'; SET statement_timeout = {}; SET search_path TO {}", timeoutMs,
                            conn->quote_name(settings.schema)));
        tx.commit();
        return conn;
    }
    catch (const std::exception& e)
    {
        error = describeDbError(e);
        return nullptr;
    }
}

bool pingConnection(pqxx::connection& conn) noexcept
{
    try
    {
        if (!conn.is_open())
            return false;
        pqxx::nontransaction tx(conn);
        tx.exec("SELECT 1").one_row();
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool ensureDatabaseExists(const DbSettings& settings, bool& created, std::string& error)
{
    created = false;
    const auto target = connectionDbName(settings.connectionString);
    if (!target)
    {
        error = "db.connection_string names no database (dbname)";
        return false;
    }
    if (!isSafeIdentifier(*target))
    {
        error = std::format("refusing to create database '{}': not a lower-case identifier", *target);
        return false;
    }
    if (settings.adminConnectionString.empty())
        return true; // nothing to do; connecting will tell whether the database exists

    std::string connectError;
    // The admin connection is used for exactly one lookup and at most one CREATE DATABASE.
    DbSettings adminSettings = settings;
    adminSettings.schema = "public";
    auto admin = openConnection(adminSettings, settings.adminConnectionString, connectError);
    if (!admin)
    {
        error = "cannot connect with db.admin_connection_string: " + connectError;
        return false;
    }
    try
    {
        pqxx::nontransaction tx(*admin);
        const bool exists =
            tx.query_value<bool>("SELECT EXISTS (SELECT 1 FROM pg_database WHERE datname = $1)", pqxx::params{*target});
        if (!exists)
        {
            // CREATE DATABASE cannot run in a transaction block and takes no parameters; the name is
            // a validated identifier and quoted on top.
            tx.exec(std::format("CREATE DATABASE {} WITH TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE 'C' "
                                "LC_CTYPE 'C'",
                                admin->quote_name(*target)));
            created = true;
            VO_LOG_INFO("db: created database '{}'", *target);
        }
        return true;
    }
    catch (const pqxx::sql_error& e)
    {
        // 42P04 duplicate_database: created concurrently by another process - fine.
        if (e.sqlstate() == "42P04")
            return true;
        error = describeDbError(e);
        return false;
    }
    catch (const std::exception& e)
    {
        error = describeDbError(e);
        return false;
    }
}

bool queryServerInfo(pqxx::connection& conn, ServerInfo& out, std::string& error) noexcept
{
    try
    {
        pqxx::nontransaction tx(conn);
        const auto row = tx.exec("SELECT version(), current_database(), current_schema(), "
                                 "coalesce((SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()), false), "
                                 "coalesce((SELECT version FROM pg_stat_ssl WHERE pid = pg_backend_pid()), '')")
                             .one_row();
        out.version = row[0].as<std::string>();
        out.database = row[1].as<std::string>();
        out.schema = row[2].is_null() ? std::string() : row[2].as<std::string>();
        out.ssl = row[3].as<bool>();
        out.sslVersion = row[4].as<std::string>();
        return true;
    }
    catch (const std::exception& e)
    {
        error = describeDbError(e);
        return false;
    }
}

std::string describeDbError(const std::exception& e)
{
    std::string msg = firstLine(e.what());
    if (const auto* sql = dynamic_cast<const pqxx::sql_error*>(&e); sql != nullptr && !sql->sqlstate().empty())
        msg += std::format(" [SQLSTATE {}]", sql->sqlstate());
    if (msg.empty())
        msg = "unknown database error";
    return msg;
}

bool isConnectionError(const std::exception& e, const pqxx::connection* conn) noexcept
{
    if (dynamic_cast<const pqxx::broken_connection*>(&e) != nullptr)
        return true;
    if (const auto* sql = dynamic_cast<const pqxx::sql_error*>(&e); sql != nullptr)
    {
        const auto state = sql->sqlstate();
        // 08xxx connection exception, 57P01..57P03 admin shutdown / crash shutdown / cannot connect now.
        if (state.starts_with("08") || state == "57P01" || state == "57P02" || state == "57P03")
            return true;
    }
    return conn != nullptr && !conn->is_open();
}

void markThreadNoDb() noexcept
{
    t_noDb = true;
}

bool isThreadNoDb() noexcept
{
    return t_noDb;
}

} // namespace vo::server::db
