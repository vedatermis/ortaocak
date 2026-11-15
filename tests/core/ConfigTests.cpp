#include "shared/core/Config.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <process.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using vo::core::Config;

namespace {

constexpr const char* kBase = R"(
[net]
listen_port = 15001
listen_address = "0.0.0.0"

[world]
tick_rate = 20
gravity = 9.81

[log]
level = "info"
color = true

[db]
connection_string = ""
pool = { size = 4, timeout_ms = 500 }

[list]
items = [1, 2, 3]
)";

constexpr const char* kOverlay = R"(
[net]
listen_port = 16000

[db]
connection_string = "postgresql://local"
pool = { size = 8 }

[list]
items = [9]

[extra]
only_in_overlay = "yes"
)";

// Temporary directory removed at scope exit.
struct TempDir
{
    std::filesystem::path path;

    TempDir()
    {
        static std::atomic<int> counter{0};
        path = std::filesystem::temp_directory_path() /
               ("vo_tests_" + std::to_string(::_getpid()) + "_" + std::to_string(++counter));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path write(const std::string& name, const std::string& content) const
    {
        const auto file = path / name;
        std::ofstream(file, std::ios::binary) << content;
        return file;
    }
};

Config::EnvLookup fakeEnv(std::map<std::string, std::string> vars)
{
    return [vars = std::move(vars)](const std::string& name) -> std::optional<std::string> {
        const auto it = vars.find(name);
        if (it == vars.end())
            return std::nullopt;
        return it->second;
    };
}

} // namespace

TEST_CASE("Config returns defaults for missing keys", "[config]")
{
    Config cfg;
    std::string err;
    REQUIRE(cfg.loadString(kBase, err));

    CHECK(cfg.getInt("net.missing", 42) == 42);
    CHECK(cfg.getString("nope.nothing", "dflt") == "dflt");
    CHECK(cfg.getBool("log.missing", true));
    CHECK_THAT(cfg.getDouble("world.missing", 1.5), Catch::Matchers::WithinAbs(1.5, 1e-12));
    CHECK_FALSE(cfg.has("net.missing"));
    CHECK_FALSE(cfg.findInt("net.missing").has_value());
}

TEST_CASE("Config typed getters read values", "[config]")
{
    Config cfg;
    std::string err;
    REQUIRE(cfg.loadString(kBase, err));

    CHECK(cfg.getInt("net.listen_port", 0) == 15001);
    CHECK(cfg.getString("net.listen_address") == "0.0.0.0");
    CHECK(cfg.getInt("world.tick_rate") == 20);
    CHECK_THAT(cfg.getDouble("world.gravity"), Catch::Matchers::WithinAbs(9.81, 1e-12));
    CHECK(cfg.getBool("log.color"));
    CHECK(cfg.getInt("db.pool.size") == 4);
    CHECK(cfg.has("db.connection_string"));
}

TEST_CASE("Config type mismatch falls back to default", "[config]")
{
    Config cfg;
    std::string err;
    REQUIRE(cfg.loadString(kBase, err));

    CHECK(cfg.getInt("log.level", -1) == -1);       // string, not int
    CHECK(cfg.getString("net.listen_port", "x") == "x"); // int, not string
    CHECK_FALSE(cfg.getBool("net.listen_address", false));
}

TEST_CASE("Config overlay deep-merges over base", "[config]")
{
    Config cfg;
    std::string err;
    REQUIRE(cfg.loadString(kBase, err));
    REQUIRE(cfg.mergeString(kOverlay, err));

    CHECK(cfg.getInt("net.listen_port") == 16000);                      // overridden
    CHECK(cfg.getString("net.listen_address") == "0.0.0.0");            // kept from base
    CHECK(cfg.getString("db.connection_string") == "postgresql://local");
    CHECK(cfg.getInt("db.pool.size") == 8);                             // nested table merged
    CHECK(cfg.getInt("db.pool.timeout_ms") == 500);                     // nested key kept
    CHECK(cfg.getInt("list.items[0]") == 9);                            // arrays replace
    CHECK_FALSE(cfg.has("list.items[1]"));
    CHECK(cfg.getString("extra.only_in_overlay") == "yes");             // new key
    CHECK(cfg.getInt("world.tick_rate") == 20);
}

TEST_CASE("Config parse errors are reported and leave contents intact", "[config]")
{
    Config cfg;
    std::string err;
    REQUIRE(cfg.loadString(kBase, err));

    CHECK_FALSE(cfg.mergeString("this is = = not toml", err, "bad.toml"));
    CHECK_FALSE(err.empty());
    CHECK(err.find("bad.toml") != std::string::npos);
    CHECK(cfg.getInt("net.listen_port") == 15001);

    err.clear();
    CHECK_FALSE(cfg.loadString("[broken", err));
    CHECK_FALSE(err.empty());
    CHECK(cfg.getInt("net.listen_port") == 15001);
}

TEST_CASE("Config overlay path naming", "[config]")
{
    CHECK(Config::overlayPathFor("config/server.toml") == std::filesystem::path("config/server.local.toml"));
    CHECK(Config::overlayPathFor("client.toml") == std::filesystem::path("client.local.toml"));
    CHECK(Config::overlayPathFor("dir/settings") == std::filesystem::path("dir/settings.local.toml"));
}

TEST_CASE("Config loadWithOverlay uses *.local.toml when present", "[config]")
{
    TempDir dir;
    const auto base = dir.write("server.toml", kBase);

    SECTION("without overlay")
    {
        Config cfg;
        std::string err;
        REQUIRE(cfg.loadWithOverlay(base, err));
        CHECK(cfg.getInt("net.listen_port") == 15001);
        CHECK(cfg.sources().size() == 1);
    }

    SECTION("with overlay")
    {
        dir.write("server.local.toml", kOverlay);
        Config cfg;
        std::string err;
        REQUIRE(cfg.loadWithOverlay(base, err));
        CHECK(cfg.getInt("net.listen_port") == 16000);
        CHECK(cfg.getString("extra.only_in_overlay") == "yes");
        REQUIRE(cfg.sources().size() == 2);
        CHECK(cfg.sources()[1].filename() == "server.local.toml");
    }

    SECTION("broken overlay is an error")
    {
        dir.write("server.local.toml", "[[[");
        Config cfg;
        std::string err;
        CHECK_FALSE(cfg.loadWithOverlay(base, err));
        CHECK(err.find("server.local.toml") != std::string::npos);
    }
}

TEST_CASE("Config missing file is an error", "[config]")
{
    Config cfg;
    std::string err;
    CHECK_FALSE(cfg.loadWithOverlay("definitely/not/here.toml", err));
    CHECK(err.find("here.toml") != std::string::npos);
}

TEST_CASE("Config env override has highest precedence", "[config][env]")
{
    Config cfg;
    std::string err;
    REQUIRE(cfg.loadString(kBase, err));
    REQUIRE(cfg.mergeString(kOverlay, err));
    cfg.bindEnv("db.connection_string", "VEDAT_DB_URL");
    cfg.bindEnv("net.listen_port", "VO_PORT");
    cfg.bindEnv("log.color", "VO_COLOR");
    cfg.bindEnv("world.gravity", "VO_GRAVITY");
    cfg.bindEnv("new.key", "VO_NEW");

    SECTION("unset env keeps file values")
    {
        cfg.setEnvLookup(fakeEnv({}));
        CHECK(cfg.getString("db.connection_string") == "postgresql://local");
        CHECK(cfg.getInt("net.listen_port") == 16000);
        CHECK_FALSE(cfg.has("new.key"));
    }

    SECTION("set env wins")
    {
        cfg.setEnvLookup(fakeEnv({{"VEDAT_DB_URL", "postgresql://env"},
                                  {"VO_PORT", "17000"},
                                  {"VO_COLOR", "false"},
                                  {"VO_GRAVITY", "1.25"},
                                  {"VO_NEW", "fresh"}}));
        CHECK(cfg.getString("db.connection_string") == "postgresql://env");
        CHECK(cfg.getInt("net.listen_port") == 17000);
        CHECK_FALSE(cfg.getBool("log.color", true));
        CHECK_THAT(cfg.getDouble("world.gravity"), Catch::Matchers::WithinAbs(1.25, 1e-12));
        CHECK(cfg.has("new.key"));
        CHECK(cfg.getString("new.key") == "fresh");
    }

    SECTION("empty env value is ignored")
    {
        cfg.setEnvLookup(fakeEnv({{"VEDAT_DB_URL", ""}}));
        CHECK(cfg.getString("db.connection_string") == "postgresql://local");
    }

    SECTION("unparseable env value falls back to file")
    {
        cfg.setEnvLookup(fakeEnv({{"VO_PORT", "not-a-number"}, {"VO_COLOR", "maybe"}}));
        CHECK(cfg.getInt("net.listen_port") == 16000);
        CHECK(cfg.getBool("log.color"));
    }
}

TEST_CASE("Config default env lookup reads the process environment", "[config][env]")
{
    const char* name = "VO_TESTS_CONFIG_ENV_PROBE";
    REQUIRE(::_putenv_s(name, "from-process-env") == 0);

    Config cfg;
    std::string err;
    REQUIRE(cfg.loadString(kBase, err));
    cfg.bindEnv("db.connection_string", name);
    CHECK(cfg.getString("db.connection_string") == "from-process-env");

    REQUIRE(::_putenv_s(name, "") == 0); // removes the variable
    CHECK(cfg.getString("db.connection_string", "x").empty());
}
