// Three synthetic zones with the markers and gates of docs/PROTOCOL_ZONES.md §1.2 (flat test
// terrains, everything walkable): zone 1 "Eşik" (both races), zone 2 (Aydor only), zone 3 (Kargan only).
//
//   zone 1: spawn_aydor (100,100), spawn_kargan (400,400), gate_aydor (110,100) -> zone 2 arrive_esik,
//           arrive_aydor (118,100), gate_kargan (390,400) -> zone 3 arrive_esik, arrive_kargan (382,400)
//   zone 2: spawn_aydor (100,100), gate_esik (110,100) -> zone 1 arrive_aydor, arrive_esik (118,100)
//   zone 3: spawn_kargan (400,400), gate_esik (390,400) -> zone 1 arrive_kargan, arrive_esik (382,400)
#pragma once

#include "server/world/World.h"
#include "shared/game/ZoneData.h"
#include "tests/server/world/TestTerrain.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace vo::tests {

inline constexpr float kGateRadius = 2.5f;

inline void addMarker(assets::TerrainData& t, const std::string& name, float x, float z, float yaw = 0.0f)
{
    t.spawns.push_back({name, {x, 0.0f, z}, yaw});
}

inline assets::TerrainData zoneTerrain(std::uint16_t zone)
{
    assets::TerrainData t = makeTestTerrain();
    if (zone == 1)
    {
        addMarker(t, "gate_aydor", 110.0f, 100.0f);
        addMarker(t, "arrive_aydor", 118.0f, 100.0f, 1.0f);
        addMarker(t, "gate_kargan", 390.0f, 400.0f);
        addMarker(t, "arrive_kargan", 382.0f, 400.0f, 2.0f);
    }
    else
    {
        const float base = zone == 2 ? 100.0f : 400.0f;
        const float dir = zone == 2 ? 1.0f : -1.0f;
        addMarker(t, "gate_esik", base + dir * 10.0f, base);
        addMarker(t, "arrive_esik", base + dir * 18.0f, base, 0.5f);
    }
    return t;
}

inline server::world::ZoneLayout zoneLayout(std::uint16_t zone)
{
    using G = server::world::ZoneLayout::Gate;
    const std::uint8_t aydor = game::raceBit(1), kargan = game::raceBit(2);
    server::world::ZoneLayout l;
    if (zone == 1)
    {
        l.raceMask = game::kAllRaces;
        l.gates.push_back(G{{"gate_aydor", kGateRadius, 2, "arrive_esik"}, aydor});
        l.gates.push_back(G{{"gate_kargan", kGateRadius, 3, "arrive_esik"}, kargan});
    }
    else if (zone == 2)
    {
        l.raceMask = aydor;
        l.gates.push_back(G{{"gate_esik", kGateRadius, 1, "arrive_aydor"}, game::kAllRaces});
    }
    else
    {
        l.raceMask = kargan;
        l.gates.push_back(G{{"gate_esik", kGateRadius, 1, "arrive_kargan"}, game::kAllRaces});
    }
    return l;
}

// Adds the three zones (zone 1 first: the start zone) and finalises them with homes 2 / 3, or 1 / 1
// with `homesInZone1` (like world.start_zone_override = 1).
inline void addThreeZones(server::world::World& world, bool homesInZone1 = false)
{
    std::string error;
    for (std::uint16_t id = 1; id <= 3; ++id)
    {
        INFO(error);
        REQUIRE(world.createZone(id, zoneTerrain(id), error, zoneLayout(id)));
    }
    INFO(error);
    REQUIRE(world.finalizeZones(homesInZone1 ? 1 : 2, homesInZone1 ? 1 : 3, error));
}

} // namespace vo::tests
