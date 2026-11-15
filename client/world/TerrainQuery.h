// CPU terrain queries used by movement, camera, picking and labels (no D3D). Height and walkability
// come from shared vo::assets::TerrainData (heightAt / isWalkable), so client and server agree.
#pragma once

#include "client/world/WorldMath.h"
#include "shared/assets/Terrain.h"

namespace vo::client::world {

// First intersection of a ray with the heightfield within [0, maxDistance] metres: ray marching in
// `step`-metre increments, then bisection to ~1 cm. Only the zone rectangle [0, worldSize]^2 is
// considered. Returns false if nothing is hit.
[[nodiscard]] bool raycastTerrain(const assets::TerrainData& terrain, const Ray& ray, float maxDistance,
                                  Vec3& hit, float step = 0.25f) noexcept;

// True if the straight segment a -> b stays above the terrain by at least `clearance` metres at
// every sample (sampled every `step` metres). Used to hide name labels behind hills.
[[nodiscard]] bool segmentClearOfTerrain(const assets::TerrainData& terrain, Vec3 a, Vec3 b, float clearance,
                                         float step = 1.0f) noexcept;

// An oriented box that hides name labels: a prop's (or character's) local bounding box placed with
// its world matrix. Cheap CPU occlusion for labels, no depth readback: the box is conservative (it
// also covers e.g. the gaps between a tree's branches).
struct LabelOccluder
{
    Mat4 toLocal;       // inverse of the world matrix
    Vec3 localMin{};
    Vec3 localMax{};
    Vec3 center{};      // world-space bounding sphere (quick reject)
    float radius = 0.0f;
};
[[nodiscard]] LabelOccluder makeOccluder(const Mat4& world, Vec3 localMin, Vec3 localMax) noexcept;
// True if the segment a -> b passes through the box.
[[nodiscard]] bool segmentHitsOccluder(const LabelOccluder& box, Vec3 a, Vec3 b) noexcept;

} // namespace vo::client::world
