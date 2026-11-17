// Navigation helpers for bots on a terrain walk mask: BFS paths over mask cells (8-connected, no
// corner cutting, so every segment between consecutive path points is walkable by the server's
// rule), straight-line targets, and AOI cell maths.
#pragma once

#include "shared/assets/Terrain.h"
#include "shared/game/Movement.h"
#include "shared/game/TerrainWalk.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <optional>
#include <random>
#include <vector>

namespace vo::tools::botclient {

struct Point
{
    float x = 0.0f;
    float z = 0.0f;
};

inline float distance(Point a, Point b)
{
    return std::hypot(a.x - b.x, a.z - b.z);
}

class Nav
{
public:
    explicit Nav(const assets::TerrainData& t)
        : m_t(t), m_m(static_cast<int>(t.maskResolution)), m_cell(t.worldSize / static_cast<float>(t.maskResolution))
    {
    }

    [[nodiscard]] int cellIndex(float v) const
    {
        const int i = static_cast<int>(v / m_cell);
        return i < 0 ? 0 : (i >= m_m ? m_m - 1 : i);
    }
    [[nodiscard]] bool walkable(int i, int j) const
    {
        return i >= 0 && j >= 0 && i < m_m && j < m_m &&
               m_t.walkMask[static_cast<std::size_t>(j) * static_cast<std::size_t>(m_m) + static_cast<std::size_t>(i)] != 0;
    }
    [[nodiscard]] Point centre(int i, int j) const
    {
        return {(static_cast<float>(i) + 0.5f) * m_cell, (static_cast<float>(j) + 0.5f) * m_cell};
    }
    [[nodiscard]] int resolution() const noexcept { return m_m; }

    // Path from `from` (first point) through its cell centre to the centre of the first cell (BFS
    // order = nearest first) for which goal(i, j) holds. nullopt if none within `maxVisited` cells.
    // `avoid` (optional): cells the path must not enter (e.g. gate triggers, docs/PROTOCOL_ZONES.md).
    [[nodiscard]] std::optional<std::vector<Point>> bfs(Point from, const std::function<bool(int, int)>& goal,
                                                        int maxVisited,
                                                        const std::function<bool(int, int)>& avoid = nullptr) const
    {
        const int si = cellIndex(from.x), sj = cellIndex(from.z);
        if (!walkable(si, sj))
            return std::nullopt;
        const auto open_ = [&](int i, int j) { return walkable(i, j) && (!avoid || !avoid(i, j)); };
        std::vector<int> parent(static_cast<std::size_t>(m_m) * static_cast<std::size_t>(m_m), -2);
        const auto idx = [&](int i, int j) { return static_cast<std::size_t>(j) * static_cast<std::size_t>(m_m) + static_cast<std::size_t>(i); };
        std::deque<std::pair<int, int>> open;
        open.emplace_back(si, sj);
        parent[idx(si, sj)] = -1;
        int visited = 0;
        static constexpr int kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        while (!open.empty() && visited < maxVisited)
        {
            const auto [i, j] = open.front();
            open.pop_front();
            ++visited;
            if (goal(i, j))
            {
                std::vector<Point> rev;
                for (int c = static_cast<int>(idx(i, j)); c >= 0; c = parent[static_cast<std::size_t>(c)])
                    rev.push_back(centre(c % m_m, c / m_m));
                std::vector<Point> path{from};
                path.insert(path.end(), rev.rbegin(), rev.rend());
                return path;
            }
            for (const auto& d : kDirs)
            {
                const int ni = i + d[0], nj = j + d[1];
                if (!open_(ni, nj) || parent[idx(ni, nj)] != -2)
                    continue;
                if (d[0] != 0 && d[1] != 0 && (!open_(i + d[0], j) || !open_(i, j + d[1])))
                    continue; // no corner cutting
                parent[idx(ni, nj)] = static_cast<int>(idx(i, j));
                open.emplace_back(ni, nj);
            }
        }
        return std::nullopt;
    }

    // A random target min..max metres away reachable in a straight walkable line (with a small
    // clearance check either side), or nullopt after a few attempts.
    template <class Rng>
    [[nodiscard]] std::optional<Point> randomStraightTarget(Point from, Rng& rng, float minDist, float maxDist) const
    {
        std::uniform_real_distribution<float> angle(0.0f, 6.2831853f);
        std::uniform_real_distribution<float> dist(minDist, maxDist);
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const float a = angle(rng);
            const float d = dist(rng);
            const Point to{from.x + d * std::sin(a), from.z + d * std::cos(a)};
            if (game::isSegmentWalkable(m_t, from.x, from.z, to.x, to.z))
                return to;
        }
        return std::nullopt;
    }

private:
    const assets::TerrainData& m_t;
    int m_m;
    float m_cell;
};

inline int aoiCell(float v)
{
    return static_cast<int>(std::floor(v / game::kAoiCellSize));
}
inline int aoiDistance(Point a, Point b)
{
    return std::max(std::abs(aoiCell(a.x) - aoiCell(b.x)), std::abs(aoiCell(a.z) - aoiCell(b.z)));
}

} // namespace vo::tools::botclient
