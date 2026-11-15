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

} // namespace vo::client::world
