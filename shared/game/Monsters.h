// Monster data shared by client and server (docs/PROTOCOL_COMBAT.md §1.3): monster templates, the
// spawn points of each zone and its safe zones ("Ortaocak's oath ground"), loaded from
// gamedata/monsters.toml with the same strict validation as gamedata/items.toml.
//
//   MonsterData data;
//   if (!loadMonsterData(*locateMonstersFile(), data, error)) -> refuse to start
//   if (data.inSafeZone(zoneId, x, z)) -> nobody can be attacked here, monsters do not enter
//
// Walkability of the spawn points is a property of the zone terrain, not of this file: the server
// checks it at startup (and tests/game/MonstersTests.cpp checks the shipped file against the
// compiled zone 1).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::game {

using MonsterTemplateId = std::uint16_t; // 0 = nothing (ENTITY_SPAWN.template_id of players)

inline constexpr std::string_view kMonstersFile = "gamedata/monsters.toml";

struct MonsterTemplate
{
    MonsterTemplateId id = 0;
    std::string name;  // UTF-8, Turkish (ENTITY_SPAWN.name)
    std::string model; // assets/monsters/<model>.vmesh
    float scale = 1.0f; // drawn size (presentation)
    std::uint16_t level = 1;
    std::uint32_t hp = 1;
    std::uint32_t attack = 0;
    std::uint32_t defense = 0;
    float attackRange = 2.0f;
    std::uint32_t attackIntervalMs = 1500;
    float moveSpeed = 3.0f;   // chasing / returning (m/s)
    float wanderSpeed = 1.2f; // idle wandering (m/s)
    float aggroRadius = 8.0f;
    float leashRadius = 25.0f; // from the spawn point
    std::uint32_t xp = 0;
    std::uint32_t respawnMs = 15000; // from death
    bool elite = false;
};

struct MonsterSpawn
{
    std::uint16_t zone = 0;
    MonsterTemplateId monster = 0;
    float x = 0.0f;
    float z = 0.0f;
    float radius = 0.0f; // wander radius around (x, z)
    std::uint16_t count = 1;
};

struct SafeZone
{
    std::uint16_t zone = 0;
    float x = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
};

class MonsterData
{
public:
    // nullptr if the id is 0 or unknown.
    [[nodiscard]] const MonsterTemplate* find(MonsterTemplateId id) const noexcept;
    [[nodiscard]] const std::vector<MonsterTemplate>& templates() const noexcept { return m_templates; } // by id
    [[nodiscard]] const std::vector<MonsterSpawn>& spawns() const noexcept { return m_spawns; }         // file order
    [[nodiscard]] const std::vector<SafeZone>& safeZones() const noexcept { return m_safeZones; }
    [[nodiscard]] bool empty() const noexcept { return m_templates.empty(); }
    // Whether (x, z) lies inside (or on the edge of) a safe zone of `zone`.
    [[nodiscard]] bool inSafeZone(std::uint16_t zone, float x, float z) const noexcept;

private:
    friend bool parseMonsterData(std::string_view, std::string_view, MonsterData&, std::string&);
    std::vector<MonsterTemplate> m_templates; // sorted by id
    std::vector<MonsterSpawn> m_spawns;
    std::vector<SafeZone> m_safeZones;
};

// Parses and validates the TOML text of gamedata/monsters.toml. Strict: unknown keys, duplicate or
// out-of-range ids, empty names/models, non-positive or absurd numbers, leash not beyond aggro,
// respawn shorter than the corpse time, spawns naming unknown monsters or zones outside 1..65535,
// a wander circle reaching into a safe zone -> false with a message naming the file and line. On
// failure `out` is left unchanged.
[[nodiscard]] bool parseMonsterData(std::string_view tomlText, std::string_view sourceName, MonsterData& out,
                                    std::string& error);
[[nodiscard]] bool loadMonsterData(const std::filesystem::path& file, MonsterData& out, std::string& error);
// gamedata/monsters.toml searched upwards from the working directory, then from the executable
// directory (core::locateDataFile).
[[nodiscard]] std::optional<std::filesystem::path> locateMonstersFile();

} // namespace vo::game
