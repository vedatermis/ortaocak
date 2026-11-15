// Synthetic terrains for the client world tests (no asset files).
#pragma once

#include "shared/assets/Terrain.h"

#include <cstdint>
#include <functional>

namespace vo::client::test {

// N x N heights over `size` metres, heights from `height(x, z)`, all cells walkable unless
// `blocked(cellX, cellZ)` says otherwise (mask = size x size cells of 1 m).
inline assets::TerrainData makeTerrain(std::uint32_t n, float size, const std::function<float(float, float)>& height,
                                       const std::function<bool(int, int)>& blocked = {})
{
    assets::TerrainData t;
    t.resolution = n;
    t.worldSize = size;
    t.minHeight = -50.0f;
    t.maxHeight = 150.0f;
    t.heights.resize(static_cast<std::size_t>(n) * n);
    for (std::uint32_t j = 0; j < n; ++j)
    {
        for (std::uint32_t i = 0; i < n; ++i)
        {
            const float x = static_cast<float>(i) * size / static_cast<float>(n - 1);
            const float z = static_cast<float>(j) * size / static_cast<float>(n - 1);
            t.heights[static_cast<std::size_t>(j) * n + i] = t.quantizeHeight(height(x, z));
        }
    }
    const auto m = static_cast<std::uint32_t>(size);
    t.maskResolution = m;
    t.walkMask.assign(static_cast<std::size_t>(m) * m, 1);
    if (blocked)
    {
        for (std::uint32_t j = 0; j < m; ++j)
            for (std::uint32_t i = 0; i < m; ++i)
                if (blocked(static_cast<int>(i), static_cast<int>(j)))
                    t.walkMask[static_cast<std::size_t>(j) * m + i] = 0;
    }
    t.splatResolution = 1;
    t.splat.assign(4, 0);
    return t;
}

inline assets::TerrainData makeFlat(float h = 10.0f, float size = 64.0f)
{
    return makeTerrain(65, size, [h](float, float) { return h; });
}

} // namespace vo::client::test
