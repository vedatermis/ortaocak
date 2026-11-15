// Tangent-space normal mapping as the client's shaders do it (client/shaders/World.hlsli
// `decodeNormalMap` / `perturbNormal`), on the CPU for tests and tools. Conventions
// (docs/ASSET_FORMATS.md §2): texels follow glTF/OpenGL (+X = image right = +U, +Y = image up);
// a vertex tangent t (xyz unit, w = +-1) gives the bitangent B = cross(N, t.xyz) * t.w, which points
// image-up; the shading normal is normalize(T * x + B * y + N * z).
#pragma once

#include "shared/assets/AssetMath.h"

#include <array>
#include <cmath>

namespace vo::assets {

// RG texel (unorm, 0..1) of a tangent-space normal map -> unit tangent-space normal. Z is
// reconstructed (two-channel storage, BC5); XY are scaled by `scale` (glTF normalTexture.scale)
// before renormalising.
[[nodiscard]] inline Vec3 decodeNormalTexel(float r, float g, float scale = 1.0f) noexcept
{
    const float x = r * 2.0f - 1.0f;
    const float y = g * 2.0f - 1.0f;
    const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
    return normalize(Vec3{x * scale, y * scale, z});
}

// Shading normal from the interpolated vertex normal and tangent (Gram-Schmidt, as the shader).
[[nodiscard]] inline Vec3 perturbNormal(Vec3 n, const std::array<float, 4>& tangent, Vec3 ts) noexcept
{
    n = normalize(n);
    Vec3 t{tangent[0], tangent[1], tangent[2]};
    t = normalize(t - n * dot(n, t));
    const Vec3 b = cross(n, t) * (tangent[3] < 0.0f ? -1.0f : 1.0f);
    return normalize(t * ts.x + b * ts.y + n * ts.z);
}

} // namespace vo::assets
