#include "client/scenes/UserSettings.h"

#include "shared/core/Config.h"
#include "shared/core/Paths.h"
#include "shared/game/Names.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace vo::client::scenes {

std::string serializeUserSettings(const UserSettings& settings)
{
    const bool keepName = settings.rememberUsername && game::isValidUsername(settings.username);
    std::string out = "# VedatOnline user settings, written by the client. Only the username is kept here.\n"
                      "[login]\n";
    out += settings.rememberUsername ? "remember_username = true\n" : "remember_username = false\n";
    // A valid username is [A-Za-z0-9_] only, so it needs no escaping inside the basic string.
    out += "username = \"";
    if (keepName)
        out += settings.username;
    out += "\"\n";
    return out;
}

bool parseUserSettings(std::string_view toml, UserSettings& out, std::string& error)
{
    vo::core::Config config;
    if (!config.loadString(toml, error, "user settings"))
        return false;
    UserSettings s;
    s.rememberUsername = config.getBool("login.remember_username", false);
    const std::string name = config.getString("login.username", "");
    if (s.rememberUsername && game::isValidUsername(name))
        s.username = name;
    out = std::move(s);
    return true;
}

bool loadUserSettings(const std::filesystem::path& file, UserSettings& out, std::string& error)
{
    out = UserSettings{};
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
        return true;
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + vo::core::pathToUtf8(file);
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    UserSettings parsed;
    if (!parseUserSettings(buffer.str(), parsed, error))
        return false;
    out = std::move(parsed);
    return true;
}

bool saveUserSettings(const std::filesystem::path& file, const UserSettings& settings, std::string& error)
{
    std::error_code ec;
    if (file.has_parent_path())
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
        {
            error = "cannot create " + vo::core::pathToUtf8(file.parent_path()) + ": " + ec.message();
            return false;
        }
    }
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream outFile(tmp, std::ios::binary | std::ios::trunc);
        if (!outFile)
        {
            error = "cannot write " + vo::core::pathToUtf8(tmp);
            return false;
        }
        outFile << serializeUserSettings(settings);
        outFile.flush();
        if (!outFile)
        {
            error = "cannot write " + vo::core::pathToUtf8(tmp);
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec)
    {
        std::filesystem::remove(tmp, ec);
        error = "cannot replace " + vo::core::pathToUtf8(file);
        return false;
    }
    return true;
}

} // namespace vo::client::scenes
