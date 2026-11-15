#include "client/world/CameraObstacles.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vo::client::world {

bool CameraObstacles::isLargeEnough(Vec3 e) noexcept
{
    return std::min(e.x, e.z) >= kMinWidth && e.y >= kMinHeight;
}

void CameraObstacles::clear() noexcept
{
    m_boxes.clear();
    m_boxMinX.clear();
    m_boxMinZ.clear();
    m_boxMaxX.clear();
    m_boxMaxZ.clear();
    m_cells.clear();
    m_stamp.clear();
    m_cols = m_rows = 0;
    m_query = 0;
}

bool CameraObstacles::add(const Mat4& world, Vec3 localMin, Vec3 localMax)
{
    // Per-axis scale: placements may stretch non-uniformly (.vterrain v2).
    const Vec3 scale{assets::length(assets::transformVector(world, {1.0f, 0.0f, 0.0f})),
                     assets::length(assets::transformVector(world, {0.0f, 1.0f, 0.0f})),
                     assets::length(assets::transformVector(world, {0.0f, 0.0f, 1.0f}))};
    if (!(scale.x > 1e-6f) || !(scale.y > 1e-6f) || !(scale.z > 1e-6f))
        return false;
    const Vec3 size = localMax - localMin;
    const Vec3 extent{size.x * scale.x, size.y * scale.y, size.z * scale.z};
    if (!isLargeEnough(extent))
        return false;
    const Vec3 lo{localMin.x - kMargin / scale.x, localMin.y - kMargin / scale.y, localMin.z - kMargin / scale.z};
    const Vec3 hi{localMax.x + kMargin / scale.x, localMax.y + kMargin / scale.y, localMax.z + kMargin / scale.z};
    m_boxes.push_back(makeOccluder(world, lo, hi));
    float minX = std::numeric_limits<float>::max();
    float minZ = minX;
    float maxX = -minX;
    float maxZ = -minX;
    for (int i = 0; i < 8; ++i)
    {
        const Vec3 corner{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
        const Vec3 w = assets::transformPoint(world, corner);
        minX = std::min(minX, w.x);
        maxX = std::max(maxX, w.x);
        minZ = std::min(minZ, w.z);
        maxZ = std::max(maxZ, w.z);
    }
    m_boxMinX.push_back(minX);
    m_boxMinZ.push_back(minZ);
    m_boxMaxX.push_back(maxX);
    m_boxMaxZ.push_back(maxZ);
    return true;
}

void CameraObstacles::finish()
{
    m_cells.clear();
    m_stamp.assign(m_boxes.size(), 0);
    m_query = 0;
    if (m_boxes.empty())
    {
        m_cols = m_rows = 0;
        return;
    }
    const float minX = *std::min_element(m_boxMinX.begin(), m_boxMinX.end());
    const float minZ = *std::min_element(m_boxMinZ.begin(), m_boxMinZ.end());
    const float maxX = *std::max_element(m_boxMaxX.begin(), m_boxMaxX.end());
    const float maxZ = *std::max_element(m_boxMaxZ.begin(), m_boxMaxZ.end());
    m_originX = minX;
    m_originZ = minZ;
    m_cols = std::max(1, static_cast<int>(std::ceil((maxX - minX) / kCellSize)) + 1);
    m_rows = std::max(1, static_cast<int>(std::ceil((maxZ - minZ) / kCellSize)) + 1);
    m_cells.assign(static_cast<std::size_t>(m_cols) * static_cast<std::size_t>(m_rows), {});
    for (std::size_t i = 0; i < m_boxes.size(); ++i)
        forCells(m_boxMinX[i], m_boxMinZ[i], m_boxMaxX[i], m_boxMaxZ[i],
                 [&](std::size_t cell) { m_cells[cell].push_back(static_cast<std::uint32_t>(i)); });
}

template <class F>
void CameraObstacles::forCells(float minX, float minZ, float maxX, float maxZ, F&& f) const
{
    if (m_cols == 0)
        return;
    const auto col = [&](float x) { return std::clamp(static_cast<int>(std::floor((x - m_originX) / kCellSize)), 0, m_cols - 1); };
    const auto row = [&](float z) { return std::clamp(static_cast<int>(std::floor((z - m_originZ) / kCellSize)), 0, m_rows - 1); };
    if (maxX < m_originX || maxZ < m_originZ || minX > m_originX + static_cast<float>(m_cols) * kCellSize ||
        minZ > m_originZ + static_cast<float>(m_rows) * kCellSize)
        return;
    const int c0 = col(minX), c1 = col(maxX), r0 = row(minZ), r1 = row(maxZ);
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c)
            f(static_cast<std::size_t>(r) * static_cast<std::size_t>(m_cols) + static_cast<std::size_t>(c));
}

float CameraObstacles::clip(Vec3 a, Vec3 b) const
{
    if (m_boxes.empty())
        return 1.0f;
    if (++m_query == 0)
    {
        std::fill(m_stamp.begin(), m_stamp.end(), 0u);
        m_query = 1;
    }
    float best = 1.0f;
    forCells(std::min(a.x, b.x), std::min(a.z, b.z), std::max(a.x, b.x), std::max(a.z, b.z),
             [&](std::size_t cell)
             {
                 for (const std::uint32_t i : m_cells[cell])
                 {
                     if (m_stamp[i] == m_query)
                         continue;
                     m_stamp[i] = m_query;
                     float t = 1.0f;
                     bool inside = false;
                     if (!segmentEnterOccluder(m_boxes[i], a, b, t, inside))
                         continue;
                     if (!inside)
                         best = std::min(best, t);
                     else if (m_terrain != nullptr)
                         best = std::min(best, clipInside(m_boxes[i], a, b));
                 }
             });
    return best;
}

float CameraObstacles::clipInside(const LabelOccluder& box, Vec3 a, Vec3 b) const
{
    // The start lies inside the box's bounds (e.g. under a roof's eaves): the box says nothing about
    // where the walls are, the walk mask does (blocked cells = the prop's footprint grown by the
    // character radius). March the boom and stop at the first blocked cell inside this box.
    const Vec3 ab = b - a;
    const float len = assets::length(ab);
    if (!(len > 1e-4f))
        return 1.0f;
    constexpr float kStep = 0.2f;
    const int n = static_cast<int>(std::ceil(len / kStep));
    for (int s = 1; s <= n; ++s)
    {
        const float t = std::min(1.0f, static_cast<float>(s) * kStep / len);
        const Vec3 p = a + ab * t;
        const Vec3 l = assets::transformPoint(box.toLocal, p);
        if (l.x < box.localMin.x || l.x > box.localMax.x || l.y < box.localMin.y || l.y > box.localMax.y ||
            l.z < box.localMin.z || l.z > box.localMax.z)
            return 1.0f; // left the box without meeting a wall
        if (!m_terrain->isWalkable(p.x, p.z))
            return std::max(0.0f, t - kStep / len);
    }
    return 1.0f;
}

bool CameraObstacles::contains(Vec3 p) const
{
    bool found = false;
    forCells(p.x, p.z, p.x, p.z,
             [&](std::size_t cell)
             {
                 for (const std::uint32_t i : m_cells[cell])
                 {
                     const LabelOccluder& box = m_boxes[i];
                     const Vec3 l = assets::transformPoint(box.toLocal, p);
                     if (l.x >= box.localMin.x && l.x <= box.localMax.x && l.y >= box.localMin.y &&
                         l.y <= box.localMax.y && l.z >= box.localMin.z && l.z <= box.localMax.z)
                         found = true;
                 }
             });
    return found;
}

} // namespace vo::client::world
