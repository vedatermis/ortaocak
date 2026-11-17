// Zone data shared by client, server and tools (docs/PROTOCOL_ZONES.md, v7): the zones of the world
// (id, key, Turkish name and tagline, terrain path, which races may be there), the home zone of
// each race and the gates between zones, loaded from gamedata/zones.toml and strictly validated
// like skills.toml.
//
//   game::ZoneCatalog catalog;
//   if (!game::loadZoneCatalog(path, catalog, error)) -> refuse to start
//   const game::ZoneDef* z = catalog.find(2);
//   if (!z->allows(race)) ...
//
// Marker positions are properties of the terrains, not of this file: the server checks every
// marker named here against the loaded terrains at startup.
//
// The process-wide catalog (game::zones()) is the one installed with installZoneCatalog() (the
// server installs the file it validated), or else gamedata/zones.toml found like the other data
// files on first use.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::game {

inline constexpr std::string_view kZonesFile = "gamedata/zones.toml";
inline constexpr std::size_t kMaxZones = 16;
inline constexpr std::size_t kMaxGatesPerZone = 8;
inline constexpr float kMinGateRadius = 1.0f;
inline constexpr float kMaxGateRadius = 10.0f;
// Rules of PROTOCOL_ZONES.md §2 and §3.4.
inline constexpr std::uint32_t kGateRearmMs = 2000;
inline constexpr std::uint32_t kGateRefusalIntervalMs = 3000;
inline constexpr std::uint32_t kZoneLoadingTimeoutMs = 20000;
inline constexpr float kArrivalGateMargin = 1.0f; // arrival markers lie > radius + this from every gate

// Bit of a race in ZoneDef::raceMask (race 1 = bit 1, race 2 = bit 2).
[[nodiscard]] constexpr std::uint8_t raceBit(std::uint8_t race) noexcept
{
    return race >= 1 && race <= 7 ? static_cast<std::uint8_t>(1u << race) : std::uint8_t{0};
}
inline constexpr std::uint8_t kAllRaces = 0x06; // Aydor | Kargan

// "spawn_aydor" / "spawn_kargan" (the race spawn marker of a zone); "" for an unknown race.
[[nodiscard]] std::string_view raceSpawnMarker(std::uint8_t race) noexcept;

struct GateDef
{
    std::string marker;    // trigger centre in this zone's terrain ("gate_aydor")
    float radius = 2.5f;   // metres
    std::uint16_t toZone = 0;
    std::string arrive;    // marker in the target zone's terrain ("arrive_esik")
};

struct ZoneDef
{
    std::uint16_t id = 0;
    std::string key;     // "esik"
    std::string name;    // "Ortaocak" (UTF-8 Turkish)
    std::string tagline; // "Eşik'in tarafsız köyü"
    std::string terrain; // "terrain/zone1/zone1.vterrain", relative to the assets directory
    std::uint8_t raceMask = 0;
    std::vector<GateDef> gates;

    [[nodiscard]] bool allows(std::uint8_t race) const noexcept
    {
        const std::uint8_t bit = raceBit(race);
        return bit != 0 && (raceMask & bit) != 0;
    }
};

class ZoneCatalog
{
public:
    // nullptr for an unknown id / key.
    [[nodiscard]] const ZoneDef* find(std::uint16_t id) const noexcept;
    [[nodiscard]] const ZoneDef* findKey(std::string_view key) const noexcept;
    // id, or key ("2", "tanyeri"); nullptr if neither.
    [[nodiscard]] const ZoneDef* findByIdOrKey(std::string_view text) const noexcept;
    [[nodiscard]] const std::vector<ZoneDef>& zones() const noexcept { return m_zones; } // ascending id
    // Home zone of a race ([start]); 0 for an unknown race.
    [[nodiscard]] std::uint16_t homeZone(std::uint8_t race) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return m_zones.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_zones.size(); }

    // A catalog of one zone (id 1, both races, no gates, home of both) with the given terrain:
    // what the server runs without a zones.toml entry for a zone (tests, synthetic worlds).
    [[nodiscard]] static ZoneCatalog singleZone(std::uint16_t id = 1, std::string terrain = "terrain/zone1/zone1.vterrain");

private:
    friend bool parseZoneCatalog(std::string_view, std::string_view, ZoneCatalog&, std::string&);
    std::vector<ZoneDef> m_zones;
    std::uint16_t m_homeAydor = 0;
    std::uint16_t m_homeKargan = 0;
};

// Parses and validates zones.toml text. False + "<source>:<line>: <problem>" on any problem; `out`
// is unchanged then.
[[nodiscard]] bool parseZoneCatalog(std::string_view tomlText, std::string_view sourceName, ZoneCatalog& out,
                                    std::string& error);
[[nodiscard]] bool loadZoneCatalog(const std::filesystem::path& file, ZoneCatalog& out, std::string& error);
[[nodiscard]] std::optional<std::filesystem::path> locateZonesFile();

// ---- The process-wide catalog ----

// The installed catalog, else gamedata/zones.toml loaded on first use (an empty catalog if that
// fails). Thread-safe to read; install before other threads use it.
[[nodiscard]] const ZoneCatalog& zones() noexcept;
void installZoneCatalog(std::shared_ptr<const ZoneCatalog> catalog);

// ---- Gate rules (PROTOCOL_ZONES.md §2) ----

enum class GateCheck : std::uint8_t
{
    Pass = 0,          // the gate fires
    Disarmed = 1,      // not armed (re-arm time, or not outside every gate since appearing)
    Dead = 2,          // dead players never fire
    Loading = 3,       // still loading the zone
    RaceForbidden = 4, // the target zone does not allow the race: ZONE_GATE_REFUSED
};

struct GateState
{
    bool alive = true;
    bool loading = false;
    bool armedByTime = true;    // >= kGateRearmMs since appearing in the zone
    bool beenOutside = true;    // outside every gate since appearing in the zone
};

// The decision for a player standing inside a gate whose target zone allows `targetRaceMask`
// (pure; the caller checks the distance). Order: Loading, Dead, Disarmed, RaceForbidden, Pass.
[[nodiscard]] GateCheck checkGate(std::uint8_t targetRaceMask, std::uint8_t race, const GateState& state) noexcept;
[[nodiscard]] GateCheck checkGate(const ZoneCatalog& catalog, const GateDef& gate, std::uint8_t race,
                                  const GateState& state) noexcept;

} // namespace vo::game
