// Static obstacles for the orbit camera (no D3D): every large prop placement of the zone as an
// oriented box (its mesh bounds placed by the placement's world matrix, grown by a margin so the
// near plane stays off the walls), in a uniform x/z grid for the broad phase. The camera asks for
// the first box its boom (focus -> eye) runs into and pulls in to stay outside.
//
// Thin or small props are skipped (lamp posts, fences, signposts, crates, barrels, small rocks):
// a box narrower than kMinWidth on either horizontal axis or lower than kMinHeight. Trees are added
// by the caller only if it wants them (their bounds are mostly open canopy; the world skips them).
#pragma once

#include "client/world/TerrainQuery.h"
#include "client/world/WorldMath.h"

#include <cstdint>
#include <vector>

namespace vo::client::world {

class CameraObstacles
{
public:
    static constexpr float kCellSize = 16.0f; // metres per broad-phase cell
    static constexpr float kMinWidth = 0.8f;  // metres, smaller horizontal extent
    static constexpr float kMinHeight = 1.2f; // metres
    static constexpr float kMargin = 0.25f;   // metres the eye keeps from a wall

    // True if a placed box of this world size (after scale) blocks the camera.
    [[nodiscard]] static bool isLargeEnough(Vec3 worldExtent) noexcept;

    void clear() noexcept;
    // Adds a placed box (local bounds + world matrix: rotation about Y and a per-axis scale). Returns
    // false if it is too thin/small and was skipped.
    bool add(const Mat4& world, Vec3 localMin, Vec3 localMax);
    // Builds the grid; call after the last add().
    void finish();

    [[nodiscard]] std::size_t size() const noexcept { return m_boxes.size(); }
    [[nodiscard]] const LabelOccluder& box(std::size_t i) const noexcept { return m_boxes[i]; }

    // Optional walk mask for boxes that contain the start of a segment (see clip()).
    void setTerrain(const assets::TerrainData* terrain) noexcept { m_terrain = terrain; }

    // Fraction t in [0, 1] of the segment a -> b at which it first enters a box (1 = clear). A box
    // that contains `a` (the character under a roof's eaves, or pressed to a wall inside the grown
    // margin) cannot say where its walls are: with a terrain set, the segment is then stopped at the
    // first blocked walk-mask cell inside that box; without one the box is ignored.
    [[nodiscard]] float clip(Vec3 a, Vec3 b) const;
    // True if p is inside a (grown) box.
    [[nodiscard]] bool contains(Vec3 p) const;

private:
    template <class F>
    void forCells(float minX, float minZ, float maxX, float maxZ, F&& f) const;
    [[nodiscard]] float clipInside(const LabelOccluder& box, Vec3 a, Vec3 b) const;

    const assets::TerrainData* m_terrain = nullptr;
    std::vector<LabelOccluder> m_boxes;
    std::vector<float> m_boxMinX, m_boxMinZ, m_boxMaxX, m_boxMaxZ; // world x/z bounds per box
    float m_originX = 0.0f;
    float m_originZ = 0.0f;
    int m_cols = 0;
    int m_rows = 0;
    std::vector<std::vector<std::uint32_t>> m_cells;
    mutable std::vector<std::uint32_t> m_stamp; // per box: last query that tested it
    mutable std::uint32_t m_query = 0;
};

} // namespace vo::client::world
