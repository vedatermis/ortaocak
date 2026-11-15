// 3D math for the world client (no D3D): angles, view/projection matrices, frustum culling and
// screen rays. Built on shared/assets/AssetMath.h (Vec3, Mat4 in the glTF layout: column-major
// storage of a matrix applied to column vectors, so `clip = proj * view * world * p`; HLSL with the
// default column_major packing uses the same floats as `mul(M, v)`).
//
// Conventions (docs/PROTOCOL.md §5.4, docs/CLIENT_RENDERING.md): left-handed, +X right, +Y up,
// +Z forward. Yaw 0 faces +Z and grows clockwise seen from above: forward(yaw) = (sin, 0, cos).
// Projection is REVERSE-Z: the near plane maps to depth 1, the far plane to 0 (depth test GREATER).
#pragma once

#include "shared/assets/AssetMath.h"

#include <array>
#include <cmath>
#include <numbers>

namespace vo::client::world {

using assets::Mat4;
using assets::Vec3;

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * kPi;

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// ---- Angles ----
// Wraps to [-pi, pi).
[[nodiscard]] float wrapAngle(float a) noexcept;
// Signed shortest rotation from `from` to `to`, in [-pi, pi).
[[nodiscard]] inline float angleDelta(float from, float to) noexcept { return wrapAngle(to - from); }
// Shortest-arc interpolation.
[[nodiscard]] inline float lerpAngle(float a, float b, float t) noexcept { return wrapAngle(a + angleDelta(a, b) * t); }
// Turns `current` towards `target` by at most `maxStep` radians.
[[nodiscard]] float approachAngle(float current, float target, float maxStep) noexcept;
// Yaw of a horizontal direction (x, z): atan2(x, z).
[[nodiscard]] inline float yawFromDirection(float x, float z) noexcept { return std::atan2(x, z); }
[[nodiscard]] inline Vec3 forwardFromYaw(float yaw) noexcept { return {std::sin(yaw), 0.0f, std::cos(yaw)}; }
// Right vector of a yaw (90 degrees clockwise from forward): (cos, 0, -sin).
[[nodiscard]] inline Vec3 rightFromYaw(float yaw) noexcept { return {std::cos(yaw), 0.0f, -std::sin(yaw)}; }

// Frame-rate independent exponential smoothing factor for `rate` (1/s) over dt seconds.
[[nodiscard]] inline float smoothingFactor(float rate, float dt) noexcept { return 1.0f - std::exp(-rate * dt); }

[[nodiscard]] inline float horizontalDistance(Vec3 a, Vec3 b) noexcept
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// ---- Matrices (column vectors) ----
// World -> view (left-handed: the camera looks down +Z of view space).
[[nodiscard]] Mat4 lookAtLH(Vec3 eye, Vec3 target, Vec3 up) noexcept;
// Reverse-Z perspective: view-space z = nearZ -> NDC depth 1, z = farZ -> 0. fovY in radians.
[[nodiscard]] Mat4 perspectiveReverseZ(float fovY, float aspect, float nearZ, float farZ) noexcept;
// Orthographic, standard depth: view z = nearZ -> 0, farZ -> 1 (shadow maps).
[[nodiscard]] Mat4 orthographicLH(float width, float height, float nearZ, float farZ) noexcept;
// T(position) * RotY(yaw) * S(scale).
[[nodiscard]] Mat4 makeWorld(Vec3 position, float yaw, float scale) noexcept;
// clip = m * (p, 1)
[[nodiscard]] Vec4 transform(const Mat4& m, Vec3 p) noexcept;

// ---- Culling ----
struct Plane
{
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f; // a*x + b*y + c*z + d >= 0 inside, (a,b,c) unit
};

class Frustum
{
public:
    // Planes of a reverse-Z or standard D3D projection (0 <= z_ndc <= 1 either way).
    void setFromViewProj(const Mat4& viewProj) noexcept;
    [[nodiscard]] bool intersectsSphere(Vec3 center, float radius) const noexcept;
    [[nodiscard]] bool intersectsAabb(Vec3 min, Vec3 max) const noexcept;

private:
    std::array<Plane, 6> m_planes{};
};

// ---- Rays ----
struct Ray
{
    Vec3 origin{};
    Vec3 direction{}; // unit
};

// Ray through a pixel (top-left origin) of a viewport of size w x h, from the inverse view-projection.
[[nodiscard]] Ray screenRay(const Mat4& invViewProj, float px, float py, float w, float h) noexcept;

// World point -> pixel. False if behind the camera.
[[nodiscard]] bool projectToScreen(const Mat4& viewProj, Vec3 p, float w, float h, Vec2& out,
                                   float* depth = nullptr) noexcept;

} // namespace vo::client::world
