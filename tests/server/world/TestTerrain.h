// Synthetic terrains for the world tests: no files, fully known heights and walk mask.
#pragma once

#include "shared/assets/Terrain.h"

#include <cstdint>
#include <string>

namespace vo::tests {

struct TestTerrainSpec
{
    float worldSize = 512.0f;
    std::uint32_t maskResolution = 512; // 1 m walk-mask cells for 512 m
    std::uint32_t heightResolution = 33;
    float slope = 0.0f; // height = slope * x (metres per metre along +X)
    float aydorX = 100.0f, aydorZ = 100.0f;
    float karganX = 400.0f, karganZ = 400.0f;
};

// Flat (or uniformly sloped along X) terrain, everything walkable, both race spawns.
inline assets::TerrainData makeTestTerrain(const TestTerrainSpec& spec = {})
{
    assets::TerrainData t;
    t.resolution = spec.heightResolution;
    t.worldSize = spec.worldSize;
    t.minHeight = 0.0f;
    t.maxHeight = spec.slope > 0.0f ? spec.slope * spec.worldSize : 10.0f;
    t.heights.assign(static_cast<std::size_t>(t.resolution) * t.resolution, 0);
    if (spec.slope > 0.0f)
    {
        for (std::uint32_t j = 0; j < t.resolution; ++j)
            for (std::uint32_t i = 0; i < t.resolution; ++i)
            {
                const float x = static_cast<float>(i) * t.worldSize / static_cast<float>(t.resolution - 1);
                t.heights[static_cast<std::size_t>(j) * t.resolution + i] = t.quantizeHeight(spec.slope * x);
            }
    }
    t.splatResolution = 1;
    t.splat.assign(4, 0);
    t.maskResolution = spec.maskResolution;
    t.walkMask.assign(static_cast<std::size_t>(t.maskResolution) * t.maskResolution, 1);
    for (int l = 0; l < 4; ++l)
        t.layers.push_back({"textures/terrain/test.png", 4.0f});
    t.spawns.push_back({"spawn_aydor", {spec.aydorX, 0.0f, spec.aydorZ}, 0.0f});
    t.spawns.push_back({"spawn_kargan", {spec.karganX, 0.0f, spec.karganZ}, 1.0f});
    return t;
}

// Marks the walk-mask cell containing (x, z) as blocked.
inline void blockCell(assets::TerrainData& t, float x, float z)
{
    const float cell = t.worldSize / static_cast<float>(t.maskResolution);
    const auto i = static_cast<std::size_t>(x / cell);
    const auto j = static_cast<std::size_t>(z / cell);
    t.walkMask[j * t.maskResolution + i] = 0;
}

} // namespace vo::tests
