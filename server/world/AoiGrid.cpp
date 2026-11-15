#include "server/world/AoiGrid.h"

#include "shared/core/Assert.h"

#include <cmath>

namespace vo::server::world {

AoiGrid::AoiGrid(float worldSize, float cellSize, int viewRadius)
    : m_cellSize(cellSize > 0.0f ? cellSize : 32.0f),
      m_side(std::max(1, static_cast<int>(std::ceil(worldSize / m_cellSize)))), m_radius(std::max(0, viewRadius)),
      m_cells(static_cast<std::size_t>(m_side) * static_cast<std::size_t>(m_side))
{
}

AoiCell AoiGrid::cellOf(float x, float z) const noexcept
{
    const auto toCell = [&](float v) {
        if (!(v >= 0.0f)) // also NaN
            return 0;
        return std::min(static_cast<int>(v / m_cellSize), m_side - 1);
    };
    return {toCell(x), toCell(z)};
}

void AoiGrid::add(EntityId id, AoiCell cell)
{
    m_cells[index(cell)].push_back(id);
    ++m_population;
}

void AoiGrid::remove(EntityId id, AoiCell cell)
{
    auto& v = m_cells[index(cell)];
    const auto it = std::find(v.begin(), v.end(), id);
    VO_ASSERT(it != v.end());
    if (it == v.end())
        return;
    *it = v.back();
    v.pop_back();
    --m_population;
}

void AoiGrid::move(EntityId id, AoiCell from, AoiCell to)
{
    if (from == to)
        return;
    remove(id, from);
    add(id, to);
}

} // namespace vo::server::world
