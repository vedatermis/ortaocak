// Minimal vector / quaternion / matrix math for asset loading, pose evaluation and CPU skinning.
// Platform-neutral (no DirectXMath) so the server and tools can use it.
//
// Mat4 uses the glTF / docs/ASSET_FORMATS.md layout: column-major storage of a matrix applied to
// column vectors (p' = M * p), translation in m[12], m[13], m[14]. The same 16 floats are the
// DirectXMath row-vector form (XMLoadFloat4x4), so the client can memcpy them.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vo::assets {

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Unit quaternion x, y, z, w (w last).
struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Mat4
{
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    [[nodiscard]] static constexpr Mat4 identity() noexcept { return Mat4{}; }
    [[nodiscard]] float& at(int row, int col) noexcept { return m[static_cast<std::size_t>(col * 4 + row)]; }
    [[nodiscard]] float at(int row, int col) const noexcept { return m[static_cast<std::size_t>(col * 4 + row)]; }
};

// ---- Vec3 ----
[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] constexpr Vec3 operator*(float s, Vec3 a) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Vec3 a) noexcept { return std::sqrt(dot(a, a)); }
[[nodiscard]] inline Vec3 normalize(Vec3 a) noexcept
{
    const float len = length(a);
    return len > 0.0f ? a * (1.0f / len) : Vec3{};
}
[[nodiscard]] constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr Vec3 vmin(Vec3 a, Vec3 b) noexcept
{
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
[[nodiscard]] constexpr Vec3 vmax(Vec3 a, Vec3 b) noexcept
{
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

// ---- Quat ----
[[nodiscard]] constexpr float dot(Quat a, Quat b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
[[nodiscard]] inline Quat normalize(Quat q) noexcept
{
    const float len = std::sqrt(dot(q, q));
    if (!(len > 0.0f))
        return Quat{};
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}
// Hamilton product: (a * b) rotates by b first, then by a.
[[nodiscard]] constexpr Quat operator*(Quat a, Quat b) noexcept
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
[[nodiscard]] constexpr Quat conjugate(Quat q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }
// Normalised lerp along the shortest arc.
[[nodiscard]] inline Quat nlerp(Quat a, Quat b, float t) noexcept
{
    if (dot(a, b) < 0.0f)
        b = {-b.x, -b.y, -b.z, -b.w};
    return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                          a.w + (b.w - a.w) * t});
}
// Spherical lerp along the shortest arc (falls back to nlerp for nearly equal rotations).
[[nodiscard]] inline Quat slerp(Quat a, Quat b, float t) noexcept
{
    float d = dot(a, b);
    if (d < 0.0f)
    {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f)
        return nlerp(a, b, t);
    const float theta = std::acos(std::clamp(d, -1.0f, 1.0f));
    const float s = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / s;
    const float wb = std::sin(t * theta) / s;
    return normalize(Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}
// Rotation of `angle` radians about `axis` (component formula; about +Y a positive angle turns +Z
// towards +X, which is the engine's yaw direction).
[[nodiscard]] inline Quat quatFromAxisAngle(Vec3 axis, float angle) noexcept
{
    const Vec3 a = normalize(axis);
    const float s = std::sin(angle * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}
[[nodiscard]] inline Vec3 rotate(Quat q, Vec3 v) noexcept
{
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

// ---- Mat4 ----
[[nodiscard]] inline Mat4 operator*(const Mat4& a, const Mat4& b) noexcept
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.at(row, k) * b.at(k, c);
            r.at(row, c) = s;
        }
    return r;
}
[[nodiscard]] inline Vec3 transformPoint(const Mat4& a, Vec3 p) noexcept
{
    return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12],
            a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
            a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}
[[nodiscard]] inline Vec3 transformVector(const Mat4& a, Vec3 v) noexcept
{
    return {a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z, a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z,
            a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z};
}
[[nodiscard]] inline float maxAbsDifference(const Mat4& a, const Mat4& b) noexcept
{
    float d = 0.0f;
    for (std::size_t i = 0; i < 16; ++i)
        d = std::max(d, std::fabs(a.m[i] - b.m[i]));
    return d;
}

// T * R * S (scale first). The quaternion is normalised.
[[nodiscard]] Mat4 composeTrs(Vec3 t, Quat q, Vec3 s) noexcept;
// Inverse of composeTrs for affine matrices without shear (a negative determinant puts the sign into
// s.x). Returns false if degenerate or if recomposition differs by more than `tolerance` (relative).
[[nodiscard]] bool decomposeTrs(const Mat4& m, Vec3& t, Quat& r, Vec3& s, float tolerance = 1e-4f) noexcept;
[[nodiscard]] float determinant3x3(const Mat4& m) noexcept;
// Rotation part of an orthonormal matrix as a unit quaternion with w >= 0.
[[nodiscard]] Quat quatFromRotationMatrix(const Mat4& m) noexcept;
// General inverse; false (and identity) for a singular matrix.
[[nodiscard]] bool invert(const Mat4& in, Mat4& out) noexcept;

// Local transform: scale first, then rotation, then translation.
struct Transform
{
    Vec3 translation{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] Mat4 toMatrix() const noexcept { return composeTrs(translation, rotation, scale); }
};

} // namespace vo::assets
