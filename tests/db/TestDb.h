// Test database access for the PostgreSQL tests (docs/DATABASE.md "Test database").
//
// The URL comes from VEDAT_TEST_DB_URL, else db.test_connection_string in config/server.local.toml
// (searched upwards). VEDAT_TEST_DB_URL=skip forces the DB tests to skip. Without a URL every DB
// test SKIPs with a message instead of failing, so the suite passes on machines without PostgreSQL.
//
// Safety: only the database named `vedatonline_test` is ever used (checked on the URL and with
// current_database()). Each test gets its own schema vo_test_<unix time>_<random>, dropped again
// by ~TestSchema; schemas left behind by crashed runs are swept when older than two hours.
#pragma once

#include "server/db/DbSettings.h"

#include <memory>
#include <optional>
#include <string>

namespace vo::tests {

inline constexpr const char* kTestDatabaseName = "vedatonline_test";

struct TestDbUrls
{
    std::string url;      // vedatonline_test
    std::string adminUrl; // maintenance DB (may be empty): only to create vedatonline_test if missing
};

// nullopt + `why` if no test database is configured (or skipping is forced).
[[nodiscard]] std::optional<TestDbUrls> testDbUrls(std::string& why);

class TestSchema
{
public:
    // Creates a fresh, migrated (unless migrate = false) schema. On failure returns null and sets
    // `why`; `configured` tells whether a test DB was configured at all (skip vs. fail).
    static std::unique_ptr<TestSchema> create(std::string& why, bool& configured, bool migrate = true);

    TestSchema(std::string url, std::string name) : m_url(std::move(url)), m_name(std::move(name)) {} // use create()
    ~TestSchema();
    TestSchema(const TestSchema&) = delete;
    TestSchema& operator=(const TestSchema&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& url() const noexcept { return m_url; }
    // Settings for a pool/service on this schema.
    [[nodiscard]] server::db::DbSettings settings(std::size_t poolSize = 4) const;

private:
    std::string m_url;
    std::string m_name;
};

} // namespace vo::tests

// Inside a TEST_CASE: creates `var` (std::unique_ptr<TestSchema>) or SKIPs/FAILs the test.
#define VO_TEST_SCHEMA(var, ...)                                                                                       \
    std::unique_ptr<::vo::tests::TestSchema> var;                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        std::string voWhy_;                                                                                            \
        bool voConfigured_ = false;                                                                                    \
        var = ::vo::tests::TestSchema::create(voWhy_, voConfigured_ __VA_OPT__(, ) __VA_ARGS__);                       \
        if (!var && !voConfigured_)                                                                                    \
            SKIP(voWhy_);                                                                                              \
        if (!var)                                                                                                      \
            FAIL(voWhy_);                                                                                              \
    } while (false)
