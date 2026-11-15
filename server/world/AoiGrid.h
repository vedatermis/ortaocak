// AoiGrid: area-of-interest grid of a zone (docs/PROTOCOL.md §5.4). Square cells of 32 m; an entity
// sees every entity whose cell lies in the 5x5 block centred on its own cell (Chebyshev cell
// distance <= 2). Visibility is therefore symmetric and depends only on the two cells.
// Not thread-safe (world thread only).
#pragma once

#include "server/world/WorldTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace vo::server::world {

struct AoiCell
{
    int x = 0;
    int z = 0;
    bool operator==(const AoiCell&) const = default;
};

class AoiGrid
{
public:
    AoiGrid(float worldSize, float cellSize, int viewRadius);

    [[nodiscard]] int cellsPerSide() const noexcept { return m_side; }
    [[nodiscard]] int viewRadius() const noexcept { return m_radius; }
    // Cell of a world position; positions outside the zone are clamped to the border cells.
    [[nodiscard]] AoiCell cellOf(float x, float z) const noexcept;
    [[nodiscard]] bool inView(AoiCell a, AoiCell b) const noexcept
    {
        return std::abs(a.x - b.x) <= m_radius && std::abs(a.z - b.z) <= m_radius;
    }

    void add(EntityId id, AoiCell cell);
    void remove(EntityId id, AoiCell cell);
    void move(EntityId id, AoiCell from, AoiCell to);
    [[nodiscard]] const std::vector<EntityId>& at(AoiCell cell) const { return m_cells[index(cell)]; }
    [[nodiscard]] std::size_t population() const noexcept { return m_population; }

    // Calls f(id) for every entity in the view block around `center` (including one at `center`).
    template <class F>
    void forEachInView(AoiCell center, F&& f) const
    {
        const int x0 = std::max(center.x - m_radius, 0), x1 = std::min(center.x + m_radius, m_side - 1);
        const int z0 = std::max(center.z - m_radius, 0), z1 = std::min(center.z + m_radius, m_side - 1);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx)
                for (const EntityId id : m_cells[index({cx, cz})])
                    f(id);
    }

    // For an observer moving from cell `from` to `to`: calls left(id) for every entity in a cell that
    // was in view and no longer is, and entered(id) for every entity in a cell that comes into view.
    template <class L, class E>
    void forEachViewChange(AoiCell from, AoiCell to, L&& left, E&& entered) const
    {
        if (from == to)
            return;
        forEachCellOnlyIn(from, to, left);
        forEachCellOnlyIn(to, from, entered);
    }

private:
    [[nodiscard]] std::size_t index(AoiCell c) const noexcept
    {
        return static_cast<std::size_t>(c.z) * static_cast<std::size_t>(m_side) + static_cast<std::size_t>(c.x);
    }

    // Entities in the view block of `a` whose cell is not in the view block of `b`.
    template <class F>
    void forEachCellOnlyIn(AoiCell a, AoiCell b, F& f) const
    {
        const int x0 = std::max(a.x - m_radius, 0), x1 = std::min(a.x + m_radius, m_side - 1);
        const int z0 = std::max(a.z - m_radius, 0), z1 = std::min(a.z + m_radius, m_side - 1);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx)
            {
                if (inView({cx, cz}, b))
                    continue;
                for (const EntityId id : m_cells[index({cx, cz})])
                    f(id);
            }
    }

    float m_cellSize;
    int m_side;
    int m_radius;
    std::vector<std::vector<EntityId>> m_cells;
    std::size_t m_population = 0;
};

} // namespace vo::server::world
