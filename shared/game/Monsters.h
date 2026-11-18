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

namespace vo::assets {
struct TerrainData;
}

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
    // M15: a member of a hunting slot (index into MonsterData::slots() + 1; 0 = a loose [[spawn]]), and
    // the slot's overrides of the template (0 = the template's value).
    std::uint16_t slot = 0;
    std::uint32_t respawnMs = 0;
    float leash = 0.0f;
};

// M15 "Avlaklar" (docs/DEVELOPMENT_PLAN.md §35, docs/PROTOCOL_LEVELS.md §3): a hunting slot - a named,
// compact pack of one kind of monster at fixed home points that respawn there quickly, kept apart from
// its neighbours so pulling one does not pull the next.
struct SlotHome
{
    float x = 0.0f;
    float z = 0.0f;
    MonsterTemplateId monster = 0;
};

struct MonsterSlot
{
    std::string key;  // [a-z0-9_], unique ("esik_kul_1")
    std::string name; // UTF-8, Turkish, shown by the client ("Kül Çukuru 1")
    std::uint16_t zone = 0;
    float x = 0.0f, z = 0.0f; // centre
    std::vector<MonsterTemplateId> monsters; // one kind (same name and model); members take them in turn
    std::uint16_t minLevel = 1, maxLevel = 1;
    float wander = 1.0f;  // wander radius of every member round its home
    float radius = 0.0f;  // the slot's area: farthest home from the centre + wander
    float aggro = 0.0f;   // largest aggro radius of its monsters
    std::uint32_t respawnMs = 0; // 0 = the templates'
    float leash = 0.0f;          // 0 = the templates'
    bool elite = false;          // a single elite (a boss slot)
    std::vector<SlotHome> homes;
};

// Slot rules (validated at load; the terrain ones by checkSlotTerrain).
inline constexpr std::size_t kMaxSlotMembers = 12;
inline constexpr float kMaxSlotRadius = 9.0f;     // area radius (18 m across at most)
inline constexpr float kMinSlotHomeSpacing = 2.5f; // between two homes of a slot
inline constexpr float kSlotGap = 4.0f;           // between two slots' areas, on top of the larger aggro radius
inline constexpr float kSlotGateMargin = 12.0f;   // gate radius + this + area + aggro
inline constexpr float kSlotMarkerMargin = 8.0f;  // spawn_ / arrive_ markers: area + aggro + this (elites: + leash + 2)
// The client shows a slot's name while the player stands within its area + this.
inline constexpr float kSlotShowMargin = 6.0f;

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
    // M15: the hunting slots (file order); their members are also in spawns() (one entry per member,
    // MonsterSpawn::slot = index + 1, after the loose [[spawn]] entries).
    [[nodiscard]] const std::vector<MonsterSlot>& slots() const noexcept { return m_slots; }
    // The slot of a spawn (nullptr for a loose spawn).
    [[nodiscard]] const MonsterSlot* slotOf(const MonsterSpawn& s) const noexcept;
    [[nodiscard]] const MonsterSlot* findSlot(std::string_view key) const noexcept;
    // The slot of `zone` whose area + `margin` contains (x, z) (the nearest centre if several);
    // nullptr if none.
    [[nodiscard]] const MonsterSlot* slotAt(std::uint16_t zone, float x, float z, float margin = 0.0f) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return m_templates.empty(); }
    // Whether (x, z) lies inside (or on the edge of) a safe zone of `zone`.
    [[nodiscard]] bool inSafeZone(std::uint16_t zone, float x, float z) const noexcept;

private:
    friend bool parseMonsterData(std::string_view, std::string_view, MonsterData&, std::string&);
    std::vector<MonsterTemplate> m_templates; // sorted by id
    std::vector<MonsterSpawn> m_spawns;
    std::vector<SafeZone> m_safeZones;
    std::vector<MonsterSlot> m_slots;
};

// A keep-out circle for slots (gate triggers, spawn / arrival markers): `name` for the message.
struct SlotKeepOut
{
    std::string name;
    float x = 0.0f, z = 0.0f;
    float radius = 0.0f; // nothing of the slot (area + aggro) may come closer than this
};

// M15: the terrain rules of a slot (server start, content tests): the centre, every home and 8 points
// of its wander circle walkable, a straight walkable line from every home to the centre, every home
// in `reachable` (a mask from reachableCells; empty = not checked), and the slot's area + aggro clear
// of every keep-out. False + `why` otherwise.
[[nodiscard]] bool checkSlotTerrain(const assets::TerrainData& terrain, const MonsterSlot& slot,
                                    const std::vector<SlotKeepOut>& keepOut, const std::vector<std::uint8_t>& reachable,
                                    std::string& why);
// The walk-mask cells reachable on foot (4-neighbour) from (x, z): one byte per mask cell, 1 = reachable.
[[nodiscard]] std::vector<std::uint8_t> reachableCells(const assets::TerrainData& terrain, float x, float z);

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
