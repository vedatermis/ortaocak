// MikkTSpace tangents (the reference implementation, vcpkg port `mikktspace`) for meshes that have a
// normal map but no glTF TANGENT attribute. Matches what Blender bakes against: UVs are fed with V
// flipped (glTF's (0,0) is the image's top-left, Blender's the bottom-left), so the bitangent
// cross(N, T) * w points image-up, the OpenGL normal-map convention of the art contract.
#pragma once

#include "shared/assets/AssetMath.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vo::tools::assetc {

// Positions and normals in engine space (left-handed, after the X mirror), UVs in the glTF
// convention. `tris` holds vertex indices, three per triangle (engine winding). Returns one tangent
// per corner (tris.size()), xyz unit and perpendicular to the vertex normal, w = +-1 such that
// bitangent = cross(N, T) * w points towards decreasing glTF V. Degenerate input (no UV area) gets an
// arbitrary perpendicular tangent. Deterministic.
[[nodiscard]] bool generateTangents(std::span<const assets::Vec3> positions, std::span<const assets::Vec3> normals,
                                    std::span<const std::array<float, 2>> uvs, std::span<const std::uint32_t> tris,
                                    std::vector<std::array<float, 4>>& cornerTangents);

// A unit vector perpendicular to n (stable choice), used where no tangent can be derived.
[[nodiscard]] assets::Vec3 anyPerpendicular(assets::Vec3 n) noexcept;

} // namespace vo::tools::assetc
