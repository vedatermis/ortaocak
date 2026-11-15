#include "client/scenes/UserSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using namespace vo::client::scenes;

namespace {

// Unique empty directory under the system temp dir, removed again by the destructor.
struct TempDir
{
    TempDir()
    {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() / ("vo_settings_test_" + std::to_string(rd()) + std::to_string(rd()));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("User settings round-trip through a file", "[client][settings]")
{
    TempDir dir;
    const auto file = dir.path / "nested" / "client_settings.toml"; // parent is created
    std::string error;

    UserSettings loaded;
    REQUIRE(loadUserSettings(file, loaded, error)); // missing file -> defaults
    REQUIRE(loaded == UserSettings{});

    UserSettings s;
    s.rememberUsername = true;
    s.username = "Vedat_01";
    REQUIRE(saveUserSettings(file, s, error));
    REQUIRE(std::filesystem::exists(file));
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "nested" / "client_settings.toml.tmp"));
    REQUIRE(loadUserSettings(file, loaded, error));
    REQUIRE(loaded == s);

    // Forgetting the name overwrites the old value.
    s.rememberUsername = false;
    REQUIRE(saveUserSettings(file, s, error));
    REQUIRE(loadUserSettings(file, loaded, error));
    REQUIRE_FALSE(loaded.rememberUsername);
    REQUIRE(loaded.username.empty());
    REQUIRE(readFile(file).find("Vedat_01") == std::string::npos);
}

TEST_CASE("User settings never store more than a valid username", "[client][settings]")
{
    UserSettings s;
    s.rememberUsername = true;
    s.username = "bad\"name\nwith = stuff";
    const std::string text = serializeUserSettings(s);
    REQUIRE(text.find("bad") == std::string::npos);
    REQUIRE(text.find("password") == std::string::npos);
    REQUIRE(text.find("şifre") == std::string::npos);

    UserSettings parsed;
    std::string error;
    REQUIRE(parseUserSettings(text, parsed, error));
    REQUIRE(parsed.rememberUsername);
    REQUIRE(parsed.username.empty());
}

TEST_CASE("User settings parsing tolerates odd files", "[client][settings]")
{
    UserSettings s;
    std::string error;
    // Hand-edited: invalid name dropped, unknown keys ignored, a stray password key is not read.
    REQUIRE(parseUserSettings("[login]\nremember_username = true\nusername = \"ab\"\npassword = \"x\"\n[other]\nk = 1\n", s,
                              error));
    REQUIRE(s.rememberUsername);
    REQUIRE(s.username.empty());

    // A name without the remember flag is not used.
    REQUIRE(parseUserSettings("[login]\nusername = \"vedat\"\n", s, error));
    REQUIRE_FALSE(s.rememberUsername);
    REQUIRE(s.username.empty());

    // Not TOML at all: error, defaults kept by load.
    REQUIRE_FALSE(parseUserSettings("this is [ not toml", s, error));
    REQUIRE_FALSE(error.empty());

    TempDir dir;
    const auto file = dir.path / "broken.toml";
    {
        std::ofstream out(file, std::ios::binary);
        out << "[login\nremember_username = maybe\n";
    }
    UserSettings loaded;
    loaded.rememberUsername = true;
    error.clear();
    REQUIRE_FALSE(loadUserSettings(file, loaded, error));
    REQUIRE(loaded == UserSettings{});
}
