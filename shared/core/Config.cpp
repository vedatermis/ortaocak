#include "shared/core/Config.h"

#include "shared/core/Log.h"
#include "shared/core/Paths.h"

#include <toml++/toml.hpp>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace vo::core {

struct Config::Impl
{
    toml::table root;
};

namespace {

void deepMerge(toml::table& dst, const toml::table& src)
{
    for (auto&& [key, value] : src)
    {
        if (const toml::table* srcTable = value.as_table())
        {
            if (toml::table* dstTable = dst.get_as<toml::table>(key.str()))
            {
                deepMerge(*dstTable, *srcTable);
                continue;
            }
        }
        dst.insert_or_assign(key, value);
    }
}

bool readWholeFile(const std::filesystem::path& file, std::string& out, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        error = std::format("cannot open config file '{}'", pathToUtf8(file));
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = std::move(ss).str();
    return true;
}

bool parseToml(std::string_view text, std::string_view sourceName, toml::table& out, std::string& error)
{
    try
    {
        out = toml::parse(text, sourceName);
        return true;
    }
    catch (const toml::parse_error& e)
    {
        error = std::format("{}:{}:{}: {}", sourceName, e.source().begin.line, e.source().begin.column,
                            e.description());
        return false;
    }
}

char asciiLower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::optional<bool> parseBool(std::string_view s)
{
    std::string lower(s);
    for (char& c : lower)
        c = asciiLower(c);
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
        return true;
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
        return false;
    return std::nullopt;
}

std::optional<std::int64_t> parseInt(std::string_view s)
{
    std::int64_t v = 0;
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return v;
}

std::optional<double> parseDouble(std::string_view s)
{
    double v = 0.0;
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc{} || ptr != end)
        return std::nullopt;
    return v;
}

} // namespace

Config::Config() : m_impl(std::make_unique<Impl>()), m_envLookup(&Config::processEnv) {}
Config::~Config() = default;
Config::Config(Config&&) noexcept = default;
Config& Config::operator=(Config&&) noexcept = default;

bool Config::loadString(std::string_view toml, std::string& error, std::string_view sourceName)
{
    toml::table parsed;
    if (!parseToml(toml, sourceName, parsed, error))
        return false;
    m_impl->root = std::move(parsed);
    m_sources.clear();
    return true;
}

bool Config::mergeString(std::string_view toml, std::string& error, std::string_view sourceName)
{
    toml::table parsed;
    if (!parseToml(toml, sourceName, parsed, error))
        return false;
    deepMerge(m_impl->root, parsed);
    return true;
}

bool Config::loadFile(const std::filesystem::path& file, std::string& error)
{
    std::string text;
    if (!readWholeFile(file, text, error))
        return false;
    if (!loadString(text, error, pathToUtf8(file)))
        return false;
    m_sources.push_back(file);
    return true;
}

bool Config::mergeFile(const std::filesystem::path& file, std::string& error)
{
    std::string text;
    if (!readWholeFile(file, text, error))
        return false;
    if (!mergeString(text, error, pathToUtf8(file)))
        return false;
    m_sources.push_back(file);
    return true;
}

bool Config::loadWithOverlay(const std::filesystem::path& base, std::string& error)
{
    if (!loadFile(base, error))
        return false;
    const auto overlay = overlayPathFor(base);
    std::error_code ec;
    if (std::filesystem::is_regular_file(overlay, ec))
        return mergeFile(overlay, error);
    return true;
}

std::filesystem::path Config::overlayPathFor(const std::filesystem::path& base)
{
    auto result = base;
    auto ext = base.extension();
    result.replace_extension();
    result += ".local";
    result += ext.empty() ? std::filesystem::path(".toml") : ext;
    return result;
}

void Config::bindEnv(std::string key, std::string envVar)
{
    for (auto& [k, v] : m_envBindings)
    {
        if (k == key)
        {
            v = std::move(envVar);
            return;
        }
    }
    m_envBindings.emplace_back(std::move(key), std::move(envVar));
}

void Config::setEnvLookup(EnvLookup lookup)
{
    m_envLookup = lookup ? std::move(lookup) : EnvLookup(&Config::processEnv);
}

std::optional<std::string> Config::processEnv(const std::string& name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name.c_str()) != 0 || value == nullptr)
        return std::nullopt;
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr)
        return std::nullopt;
    return std::string(value);
#endif
}

std::optional<std::string> Config::envFor(std::string_view key) const
{
    for (const auto& [k, envVar] : m_envBindings)
    {
        if (k == key)
        {
            auto value = m_envLookup(envVar);
            if (value && !value->empty())
                return value;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bool Config::has(std::string_view key) const
{
    return envFor(key).has_value() || static_cast<bool>(m_impl->root.at_path(key));
}

std::optional<std::string> Config::findString(std::string_view key) const
{
    if (auto env = envFor(key))
        return env;
    return m_impl->root.at_path(key).value<std::string>();
}

std::optional<std::int64_t> Config::findInt(std::string_view key) const
{
    if (auto env = envFor(key))
    {
        if (auto v = parseInt(*env))
            return v;
        VO_LOG_WARN("config: env override for '{}' is not an integer, ignoring", key);
    }
    return m_impl->root.at_path(key).value<std::int64_t>();
}

std::optional<double> Config::findDouble(std::string_view key) const
{
    if (auto env = envFor(key))
    {
        if (auto v = parseDouble(*env))
            return v;
        VO_LOG_WARN("config: env override for '{}' is not a number, ignoring", key);
    }
    return m_impl->root.at_path(key).value<double>();
}

std::optional<bool> Config::findBool(std::string_view key) const
{
    if (auto env = envFor(key))
    {
        if (auto v = parseBool(*env))
            return v;
        VO_LOG_WARN("config: env override for '{}' is not a boolean, ignoring", key);
    }
    return m_impl->root.at_path(key).value<bool>();
}

std::string Config::getString(std::string_view key, std::string_view def) const
{
    auto v = findString(key);
    return v ? std::move(*v) : std::string(def);
}

std::int64_t Config::getInt(std::string_view key, std::int64_t def) const
{
    return findInt(key).value_or(def);
}

double Config::getDouble(std::string_view key, double def) const
{
    return findDouble(key).value_or(def);
}

bool Config::getBool(std::string_view key, bool def) const
{
    return findBool(key).value_or(def);
}

} // namespace vo::core
