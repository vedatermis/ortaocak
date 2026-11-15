// Walkability of straight movement segments on a terrain walk mask (docs/SERVER_WORLD.md §4).
// The server rejects a MOVE_REQ whose straight path from the last accepted position crosses a
// blocked mask cell (no tunnelling through walls or object footprints); clients and bots can use the
// same test to plan legal moves. Header-only.
#pragma once

#include "shared/assets/Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace vo::game {

// True if both end points are walkable and every walk-mask cell the segment (x0,z0)-(x1,z1) touches
// is walkable. Grid traversal is exact (Amanatides-Woo); when the segment passes exactly through a
// cell corner, both side cells must be walkable too (no squeezing diagonally between two blocked
// cells).
[[nodiscard]] inline bool isSegmentWalkable(const assets::TerrainData& t, float x0, float z0, float x1, float z1)
{
    if (!t.isWalkable(x0, z0) || !t.isWalkable(x1, z1))
        return false;
    const int m = static_cast<int>(t.maskResolution);
    const float cell = t.worldSize / static_cast<float>(m);
    // Same cell mapping as TerrainData::isWalkable.
    const auto cellIndex = [&](float v) { return std::clamp(static_cast<int>(v / cell), 0, m - 1); };
    const auto walkable = [&](int ci, int cj) {
        return ci >= 0 && cj >= 0 && ci < m && cj < m &&
               t.walkMask[static_cast<std::size_t>(cj) * static_cast<std::size_t>(m) + static_cast<std::size_t>(ci)] !=
                   0;
    };

    int i = cellIndex(x0);
    int j = cellIndex(z0);
    const int iEnd = cellIndex(x1);
    const int jEnd = cellIndex(z1);
    if (i == iEnd && j == jEnd)
        return true;

    const double dx = static_cast<double>(x1) - x0;
    const double dz = static_cast<double>(z1) - z0;
    const int stepI = iEnd > i ? 1 : (iEnd < i ? -1 : 0);
    const int stepJ = jEnd > j ? 1 : (jEnd < j ? -1 : 0);
    constexpr double kInf = std::numeric_limits<double>::infinity();
    const double c = cell;
    double tMaxX = kInf, tMaxZ = kInf, tDeltaX = kInf, tDeltaZ = kInf;
    if (stepI != 0 && dx != 0.0)
    {
        const double boundary = stepI > 0 ? (i + 1) * c : i * c;
        tMaxX = (boundary - x0) / dx;
        tDeltaX = c / std::abs(dx);
    }
    if (stepJ != 0 && dz != 0.0)
    {
        const double boundary = stepJ > 0 ? (j + 1) * c : j * c;
        tMaxZ = (boundary - z0) / dz;
        tDeltaZ = c / std::abs(dz);
    }

    constexpr double kTie = 1e-9;
    int guard = std::abs(iEnd - i) + std::abs(jEnd - j) + 2;
    while ((i != iEnd || j != jEnd) && guard-- > 0)
    {
        // Never step past the end cell on an axis (robust against rounding at cell boundaries).
        const bool canI = i != iEnd;
        const bool canJ = j != jEnd;
        if (canI && (!canJ || tMaxX < tMaxZ - kTie))
        {
            i += stepI;
            tMaxX += tDeltaX;
        }
        else if (canJ && (!canI || tMaxZ < tMaxX - kTie))
        {
            j += stepJ;
            tMaxZ += tDeltaZ;
        }
        else
        {
            // Exactly through a corner: both side cells must be open, then move diagonally.
            if (!walkable(i + stepI, j) || !walkable(i, j + stepJ))
                return false;
            i += stepI;
            j += stepJ;
            tMaxX += tDeltaX;
            tMaxZ += tDeltaZ;
        }
        if (!walkable(i, j))
            return false;
    }
    return i == iEnd && j == jEnd;
}

} // namespace vo::game
