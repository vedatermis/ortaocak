// Path finding on a zone's walk mask (shared by the client's scripted walks and tests): breadth-first
// search over mask cells (8-connected, no corner cutting, so every step between consecutive cells is
// walkable by the server's segment rule), then string pulling: a waypoint is dropped while the
// straight segment past it is still walkable (game::isSegmentWalkable). Header-only, no allocation
// beyond the result and one visited array.
#pragma once

#include "shared/assets/Terrain.h"
#include "shared/game/TerrainWalk.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace vo::game {

struct PathPoint
{
    float x = 0.0f;
    float z = 0.0f;
};

// Waypoints from `from` (excluded) to `to` (the last point, exactly), or nullopt if `to` is not
// reachable within `maxVisited` cells. A straight walkable line gives just {to}.
[[nodiscard]] inline std::optional<std::vector<PathPoint>> findWalkPath(const assets::TerrainData& t, PathPoint from,
                                                                       PathPoint to, int maxVisited = 200000)
{
    const int m = static_cast<int>(t.maskResolution);
    if (m <= 0 || t.walkMask.size() < static_cast<std::size_t>(m) * static_cast<std::size_t>(m))
        return std::nullopt;
    if (isSegmentWalkable(t, from.x, from.z, to.x, to.z))
        return std::vector<PathPoint>{to};
    const float cell = t.worldSize / static_cast<float>(m);
    const auto index = [&](float v) { return std::clamp(static_cast<int>(v / cell), 0, m - 1); };
    const auto walkable = [&](int i, int j) {
        return i >= 0 && j >= 0 && i < m && j < m &&
               t.walkMask[static_cast<std::size_t>(j) * static_cast<std::size_t>(m) + static_cast<std::size_t>(i)] != 0;
    };
    const auto idx = [&](int i, int j) { return static_cast<std::size_t>(j) * static_cast<std::size_t>(m) + static_cast<std::size_t>(i); };
    const int si = index(from.x), sj = index(from.z);
    const int gi = index(to.x), gj = index(to.z);
    if (!walkable(si, sj) || !walkable(gi, gj))
        return std::nullopt;
    std::vector<int> parent(static_cast<std::size_t>(m) * static_cast<std::size_t>(m), -2);
    std::deque<std::pair<int, int>> open;
    open.emplace_back(si, sj);
    parent[idx(si, sj)] = -1;
    static constexpr int kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    int visited = 0;
    bool found = false;
    while (!open.empty() && visited < maxVisited)
    {
        const auto [i, j] = open.front();
        open.pop_front();
        ++visited;
        if (i == gi && j == gj)
        {
            found = true;
            break;
        }
        for (const auto& d : kDirs)
        {
            const int ni = i + d[0], nj = j + d[1];
            if (!walkable(ni, nj) || parent[idx(ni, nj)] != -2)
                continue;
            if (d[0] != 0 && d[1] != 0 && (!walkable(i + d[0], j) || !walkable(i, j + d[1])))
                continue; // no corner cutting
            parent[idx(ni, nj)] = static_cast<int>(idx(i, j));
            open.emplace_back(ni, nj);
        }
    }
    if (!found)
        return std::nullopt;
    // Cell centres from the goal back to the start cell, reversed; the exact target replaces the goal cell.
    std::vector<PathPoint> cells;
    for (int c = static_cast<int>(idx(gi, gj)); c >= 0; c = parent[static_cast<std::size_t>(c)])
        cells.push_back({(static_cast<float>(c % m) + 0.5f) * cell, (static_cast<float>(c / m) + 0.5f) * cell});
    std::reverse(cells.begin(), cells.end());
    cells.back() = to;
    // String pulling.
    std::vector<PathPoint> out;
    PathPoint at = from;
    std::size_t k = 0;
    while (k < cells.size())
    {
        std::size_t best = k;
        for (std::size_t n = k + 1; n < cells.size(); ++n)
        {
            if (isSegmentWalkable(t, at.x, at.z, cells[n].x, cells[n].z))
                best = n;
        }
        out.push_back(cells[best]); // cells[k] always follows from `at` (8-connected, no corner cuts)
        at = cells[best];
        k = best + 1;
    }
    return out;
}

} // namespace vo::game
