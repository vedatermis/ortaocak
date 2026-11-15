// TOML configuration with a local overlay, typed getters with defaults and env-var overrides.
//
//   vo::core::Config cfg;
//   cfg.bindEnv("db.connection_string", "VEDAT_DB_URL");
//   std::string err;
//   if (!cfg.loadWithOverlay("config/server.toml", err)) { ... }   // + config/server.local.toml
//   auto port = cfg.getInt("net.listen_port", 15001);
//
// Keys are dotted paths ("db.connection_string"). Lookup precedence, highest first:
//   1. bound environment variable (bindEnv), if set and non-empty and parseable as the requested type
//   2. overlay file (*.local.toml), deep-merged over the base file
//   3. base file
//   4. the default passed to the getter
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::core {

class Config
{
public:
    // Returns the value of an environment variable, or nullopt if unset.
    using EnvLookup = std::function<std::optional<std::string>(const std::string& name)>;

    Config();
    ~Config();
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&) noexcept;
    Config& operator=(Config&&) noexcept;

    // Replaces the current contents. On failure the contents are unchanged and `error` is set.
    bool loadFile(const std::filesystem::path& file, std::string& error);
    bool loadString(std::string_view toml, std::string& error, std::string_view sourceName = "<string>");

    // Deep-merges a document over the current contents: tables merge recursively, any other value
    // (including arrays) replaces the existing one.
    bool mergeFile(const std::filesystem::path& file, std::string& error);
    bool mergeString(std::string_view toml, std::string& error, std::string_view sourceName = "<string>");

    // Loads `base` and, if it exists, merges overlayPathFor(base) on top.
    bool loadWithOverlay(const std::filesystem::path& base, std::string& error);

    // "dir/server.toml" -> "dir/server.local.toml"
    [[nodiscard]] static std::filesystem::path overlayPathFor(const std::filesystem::path& base);

    // Files successfully loaded/merged so far, in order.
    [[nodiscard]] const std::vector<std::filesystem::path>& sources() const noexcept { return m_sources; }

    // Environment overrides.
    void bindEnv(std::string key, std::string envVar);
    void setEnvLookup(EnvLookup lookup); // default: process environment
    [[nodiscard]] static std::optional<std::string> processEnv(const std::string& name);

    [[nodiscard]] bool has(std::string_view key) const;

    [[nodiscard]] std::string getString(std::string_view key, std::string_view def = {}) const;
    [[nodiscard]] std::int64_t getInt(std::string_view key, std::int64_t def = 0) const;
    [[nodiscard]] double getDouble(std::string_view key, double def = 0.0) const;
    [[nodiscard]] bool getBool(std::string_view key, bool def = false) const;

    [[nodiscard]] std::optional<std::string> findString(std::string_view key) const;
    [[nodiscard]] std::optional<std::int64_t> findInt(std::string_view key) const;
    [[nodiscard]] std::optional<double> findDouble(std::string_view key) const;
    [[nodiscard]] std::optional<bool> findBool(std::string_view key) const;

private:
    struct Impl;

    [[nodiscard]] std::optional<std::string> envFor(std::string_view key) const;

    std::unique_ptr<Impl> m_impl;
    std::vector<std::filesystem::path> m_sources;
    std::vector<std::pair<std::string, std::string>> m_envBindings;
    EnvLookup m_envLookup;
};

} // namespace vo::core
