// M13 "Üç Yurt" client presentation logic (docs/PROTOCOL_ZONES.md §7): zone names and taglines from
// the zone catalog (gamedata/zones.toml), the fixed Turkish texts of the zone change, the gate views of a
// zone (where each gate stands, what its label and hint say, which veil it wears) and the loading-screen
// lore lines per zone. Pure logic (no GPU, no sockets): client/scenes/WorldZones.cpp draws it,
// tests/client/ZoneViewTests.cpp checks it.
#pragma once

#include "client/world/WorldMath.h"
#include "shared/game/ZoneData.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vo::assets {
struct TerrainData;
}

namespace vo::client::world {

// ---- Fixed texts (§7.1, §7.4) ----
inline constexpr std::string_view kGateCrossingText = "Geçitten geçiliyor…";
inline constexpr std::string_view kZoneChangeFailedText = "Geçit şu an kullanılamıyor, tekrar dene.";

// ---- Distances (§7.3) ----
inline constexpr float kGateLabelRange = 40.0f; // the gate label floats above a gate within this (metres)
inline constexpr float kGateHintRange = 8.0f;   // the near-gate hint within this

// The zone's name ("Tanyeri"): the catalog's, else the built-in table's (shared/game/Zones.h), else
// "Bölge <id>".
[[nodiscard]] std::string zoneName(const game::ZoneCatalog& catalog, std::uint16_t id);
// The zone's tagline ("Aydor'un sınır yurdu"); "" if unknown.
[[nodiscard]] std::string zoneTagline(const game::ZoneCatalog& catalog, std::uint16_t id);
// "<Bölge> yükleniyor…"
[[nodiscard]] std::string zoneLoadingText(const game::ZoneCatalog& catalog, std::uint16_t id);
// "<Bölge> bölgesine geçtin." (the arrival line)
[[nodiscard]] std::string zoneArrivalText(const game::ZoneCatalog& catalog, std::uint16_t id);
// "<Bölge> Geçidi" (the label above a gate leading to `toZone`)
[[nodiscard]] std::string gateLabelText(const game::ZoneCatalog& catalog, std::uint16_t toZone);
// "Geçide yürü: <Bölge>" (the hint near a gate the player may pass)
[[nodiscard]] std::string gatePassHint(const game::ZoneCatalog& catalog, std::uint16_t toZone);
// "<Bölge> yalnızca Aydorlulara açık." / "... Karganlara açık." (ZONE_GATE_REFUSED and the hint near a
// gate closed to the player's race); the race the target zone is open to is named.
[[nodiscard]] std::string gateClosedText(const game::ZoneCatalog& catalog, std::uint16_t toZone);
// "Aydorlulara" / "Karganlara" for a zone open to one race; "" for both or none.
[[nodiscard]] std::string_view zoneRaceDative(const game::ZoneDef& zone);

// The veil effect mesh of a gate prop: "props/gate_aydor.vmesh" -> "effects/gate_veil_aydor.vmesh";
// "" if the mesh is not a gate.
[[nodiscard]] std::string gateVeilMesh(std::string_view propMesh);
// The three veil meshes (requested while loading every zone).
[[nodiscard]] const std::vector<std::string>& gateVeilMeshes();

// One gate of the zone the player is in, as the client shows it.
struct GateView
{
    std::string marker;      // "gate_aydor"
    Vec3 position{};         // the marker (trigger centre)
    float yaw = 0.0f;        // the marker yaw: the way the gate faces (towards its approach side)
    float radius = 2.5f;     // trigger radius (zones.toml)
    std::uint16_t toZone = 0;
    bool open = true;        // the player's race may enter the target zone
    std::string label;       // "Tanyeri Geçidi"
    std::string hint;        // "Geçide yürü: Tanyeri" or "Tanyeri yalnızca Aydorlulara açık."
    int placement = -1;      // index of the gate prop in TerrainData::objects (-1: no prop found)
    std::string veilMesh;    // "effects/gate_veil_<look>.vmesh" ("" without a prop)
    float top = 7.0f;        // world y of the label anchor (prop top, else marker + 7 m)
};

// The gates of `zoneId` (zones.toml) found in its terrain: markers without a terrain spawn point are
// skipped. Each gate's prop is the closest placement of a "props/gate_*" mesh within 4 m of the
// marker; the label anchor is the placement's y + `propHeight(placement)` (callers pass the scaled
// mesh height; missing or non-positive: the marker + 7 m).
[[nodiscard]] std::vector<GateView> buildGateViews(const assets::TerrainData& terrain, const game::ZoneCatalog& catalog,
                                                   std::uint16_t zoneId, std::uint8_t race,
                                                   const std::function<float(int placement)>& propHeight = {});

// The gate whose marker is horizontally nearest to (x, z) within `range`; nullptr if none.
[[nodiscard]] const GateView* nearestGate(const std::vector<GateView>& gates, float x, float z, float range);

// Alpha of a gate label at horizontal distance `d` from the player: 1 up to 32 m, fading to 0 at 40 m.
[[nodiscard]] float gateLabelAlpha(float d);

// Loading-screen lore lines of a zone (docs/LORE.md; Ortaocak's for unknown zones). Never empty.
[[nodiscard]] const std::vector<std::string_view>& zoneLoreLines(std::uint16_t zoneId);

} // namespace vo::client::world
