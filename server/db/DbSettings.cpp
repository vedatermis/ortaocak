#include "server/db/DbSettings.h"

#include "shared/core/Config.h"

#include <libpq-fe.h>

#include <format>
#include <memory>

namespace vo::server::db {

namespace {

struct ConninfoDeleter
{
    void operator()(PQconninfoOption* o) const noexcept { PQconninfoFree(o); }
};
using ConninfoPtr = std::unique_ptr<PQconninfoOption, ConninfoDeleter>;

ConninfoPtr parseConninfo(std::string_view connectionString)
{
    const std::string copy(connectionString);
    char* err = nullptr;
    ConninfoPtr opts(PQconninfoParse(copy.c_str(), &err));
    if (err != nullptr)
        PQfreemem(err); // the message may quote parts of the string: never surfaced
    return opts;
}

std::optional<std::string> option(const ConninfoPtr& opts, std::string_view key)
{
    for (const PQconninfoOption* o = opts.get(); o != nullptr && o->keyword != nullptr; ++o)
    {
        if (key == o->keyword && o->val != nullptr && *o->val != '\0')
            return std::string(o->val);
    }
    return std::nullopt;
}

} // namespace

void bindDbEnv(core::Config& config)
{
    config.bindEnv("db.connection_string", "VEDAT_DB_URL");
    config.bindEnv("db.admin_connection_string", "VEDAT_DB_ADMIN_URL");
}

bool loadDbSettings(const core::Config& config, DbSettings& out, std::string& error)
{
    DbSettings s;
    s.connectionString = config.getString("db.connection_string", "");
    s.adminConnectionString = config.getString("db.admin_connection_string", "");
    s.schema = config.getString("db.schema", "public");
    const std::int64_t pool = config.getInt("db.pool_size", 4);
    const std::int64_t workers = config.getInt("db.worker_threads", pool);
    const std::int64_t stmt = config.getInt("db.statement_timeout_ms", 5000);
    const std::int64_t connect = config.getInt("db.connect_timeout_s", 5);
    s.required = config.getBool("db.required", true);

    if (pool < 1 || pool > 64)
    {
        error = std::format("db.pool_size {} out of range 1..64", pool);
        return false;
    }
    if (workers < 1 || workers > 64)
    {
        error = std::format("db.worker_threads {} out of range 1..64", workers);
        return false;
    }
    if (stmt < 0 || stmt > 600'000)
    {
        error = std::format("db.statement_timeout_ms {} out of range 0..600000", stmt);
        return false;
    }
    if (connect < 1 || connect > 300)
    {
        error = std::format("db.connect_timeout_s {} out of range 1..300", connect);
        return false;
    }
    if (!isSafeIdentifier(s.schema))
    {
        error = std::format("db.schema '{}' is not a lower-case identifier", s.schema);
        return false;
    }
    if (!s.connectionString.empty() && !parseConninfo(s.connectionString))
    {
        error = "db.connection_string cannot be parsed (libpq key=value string or postgresql:// URI expected)";
        return false;
    }
    if (!s.adminConnectionString.empty() && !parseConninfo(s.adminConnectionString))
    {
        error = "db.admin_connection_string cannot be parsed";
        return false;
    }
    s.poolSize = static_cast<std::size_t>(pool);
    s.workerThreads = static_cast<std::size_t>(workers);
    s.statementTimeout = std::chrono::milliseconds(stmt);
    s.connectTimeout = std::chrono::seconds(connect);
    out = std::move(s);
    return true;
}

std::string describeConnectionString(std::string_view connectionString)
{
    const auto opts = parseConninfo(connectionString);
    if (!opts)
        return "<unparseable connection string>";
    const auto user = option(opts, "user");
    const auto host = option(opts, "host").value_or(option(opts, "hostaddr").value_or("localhost"));
    const auto port = option(opts, "port").value_or("5432");
    const auto dbname = option(opts, "dbname").value_or(user.value_or("?"));
    std::string out;
    if (user)
        out = *user + "@";
    out += std::format("{}:{}/{}", host, port, dbname);
    if (const auto ssl = option(opts, "sslmode"))
        out += " sslmode=" + *ssl;
    return out;
}

std::optional<std::string> connectionDbName(std::string_view connectionString)
{
    const auto opts = parseConninfo(connectionString);
    if (!opts)
        return std::nullopt;
    return option(opts, "dbname");
}

bool isSafeIdentifier(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 63)
        return false;
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!(lower || c == '_' || (digit && i > 0)))
            return false;
    }
    return true;
}

} // namespace vo::server::db
