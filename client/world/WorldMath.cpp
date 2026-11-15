#include "client/world/WorldMath.h"

#include <algorithm>

namespace vo::client::world {

float wrapAngle(float a) noexcept
{
    if (!std::isfinite(a))
        return 0.0f;
    a = std::fmod(a + kPi, kTwoPi);
    if (a < 0.0f)
        a += kTwoPi;
    return a - kPi;
}

float approachAngle(float current, float target, float maxStep) noexcept
{
    const float d = angleDelta(current, target);
    if (std::fabs(d) <= maxStep)
        return wrapAngle(target);
    return wrapAngle(current + (d > 0.0f ? maxStep : -maxStep));
}

Mat4 lookAtLH(Vec3 eye, Vec3 target, Vec3 up) noexcept
{
    const Vec3 z = assets::normalize(target - eye);
    Vec3 x = assets::normalize(assets::cross(up, z));
    if (assets::dot(x, x) < 1e-12f) // looking straight up/down: pick any perpendicular
        x = assets::normalize(assets::cross(Vec3{0.0f, 0.0f, 1.0f}, z));
    const Vec3 y = assets::cross(z, x);
    Mat4 m;
    m.at(0, 0) = x.x;
    m.at(0, 1) = x.y;
    m.at(0, 2) = x.z;
    m.at(0, 3) = -assets::dot(x, eye);
    m.at(1, 0) = y.x;
    m.at(1, 1) = y.y;
    m.at(1, 2) = y.z;
    m.at(1, 3) = -assets::dot(y, eye);
    m.at(2, 0) = z.x;
    m.at(2, 1) = z.y;
    m.at(2, 2) = z.z;
    m.at(2, 3) = -assets::dot(z, eye);
    m.at(3, 0) = 0.0f;
    m.at(3, 1) = 0.0f;
    m.at(3, 2) = 0.0f;
    m.at(3, 3) = 1.0f;
    return m;
}

Mat4 perspectiveReverseZ(float fovY, float aspect, float nearZ, float farZ) noexcept
{
    const float ys = 1.0f / std::tan(fovY * 0.5f);
    const float xs = ys / aspect;
    Mat4 m;
    m.m.fill(0.0f);
    m.at(0, 0) = xs;
    m.at(1, 1) = ys;
    // z_ndc = A + B / z with z = near -> 1 and z = far -> 0.
    m.at(2, 2) = nearZ / (nearZ - farZ);
    m.at(2, 3) = nearZ * farZ / (farZ - nearZ);
    m.at(3, 2) = 1.0f;
    return m;
}

Mat4 orthographicLH(float width, float height, float nearZ, float farZ) noexcept
{
    Mat4 m;
    m.m.fill(0.0f);
    m.at(0, 0) = 2.0f / width;
    m.at(1, 1) = 2.0f / height;
    m.at(2, 2) = 1.0f / (farZ - nearZ);
    m.at(2, 3) = -nearZ / (farZ - nearZ);
    m.at(3, 3) = 1.0f;
    return m;
}

Mat4 makeWorld(Vec3 position, float yaw, float scale) noexcept
{
    const float c = std::cos(yaw) * scale;
    const float s = std::sin(yaw) * scale;
    Mat4 m;
    m.at(0, 0) = c;
    m.at(1, 0) = 0.0f;
    m.at(2, 0) = -s;
    m.at(0, 1) = 0.0f;
    m.at(1, 1) = scale;
    m.at(2, 1) = 0.0f;
    m.at(0, 2) = s;
    m.at(1, 2) = 0.0f;
    m.at(2, 2) = c;
    m.at(0, 3) = position.x;
    m.at(1, 3) = position.y;
    m.at(2, 3) = position.z;
    return m;
}

Vec4 transform(const Mat4& m, Vec3 p) noexcept
{
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
            m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3),
            m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3)};
}

void Frustum::setFromViewProj(const Mat4& vp) noexcept
{
    auto row = [&vp](int r) { return Vec4{vp.at(r, 0), vp.at(r, 1), vp.at(r, 2), vp.at(r, 3)}; };
    const Vec4 r0 = row(0);
    const Vec4 r1 = row(1);
    const Vec4 r2 = row(2);
    const Vec4 r3 = row(3);
    const std::array<Vec4, 6> raw{{
        {r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w}, // left
        {r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w}, // right
        {r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w}, // bottom
        {r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w}, // top
        {r2.x, r2.y, r2.z, r2.w},                             // z >= 0
        {r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w}, // z <= w
    }};
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        const Vec4& p = raw[i];
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        const float inv = len > 0.0f ? 1.0f / len : 0.0f;
        m_planes[i] = {p.x * inv, p.y * inv, p.z * inv, p.w * inv};
    }
}

bool Frustum::intersectsSphere(Vec3 c, float radius) const noexcept
{
    for (const Plane& p : m_planes)
    {
        if (p.a * c.x + p.b * c.y + p.c * c.z + p.d < -radius)
            return false;
    }
    return true;
}

bool Frustum::intersectsAabb(Vec3 mn, Vec3 mx) const noexcept
{
    for (const Plane& p : m_planes)
    {
        // The box corner furthest along the plane normal.
        const float x = p.a >= 0.0f ? mx.x : mn.x;
        const float y = p.b >= 0.0f ? mx.y : mn.y;
        const float z = p.c >= 0.0f ? mx.z : mn.z;
        if (p.a * x + p.b * y + p.c * z + p.d < 0.0f)
            return false;
    }
    return true;
}

Ray screenRay(const Mat4& invViewProj, float px, float py, float w, float h) noexcept
{
    const float nx = 2.0f * (px + 0.5f) / std::max(w, 1.0f) - 1.0f;
    const float ny = 1.0f - 2.0f * (py + 0.5f) / std::max(h, 1.0f);
    auto unproject = [&](float z)
    {
        const Vec4 v{invViewProj.at(0, 0) * nx + invViewProj.at(0, 1) * ny + invViewProj.at(0, 2) * z +
                         invViewProj.at(0, 3),
                     invViewProj.at(1, 0) * nx + invViewProj.at(1, 1) * ny + invViewProj.at(1, 2) * z +
                         invViewProj.at(1, 3),
                     invViewProj.at(2, 0) * nx + invViewProj.at(2, 1) * ny + invViewProj.at(2, 2) * z +
                         invViewProj.at(2, 3),
                     invViewProj.at(3, 0) * nx + invViewProj.at(3, 1) * ny + invViewProj.at(3, 2) * z +
                         invViewProj.at(3, 3)};
        const float iw = std::fabs(v.w) > 1e-12f ? 1.0f / v.w : 0.0f;
        return Vec3{v.x * iw, v.y * iw, v.z * iw};
    };
    const Vec3 nearPoint = unproject(1.0f); // reverse-Z: near plane is depth 1
    const Vec3 farPoint = unproject(0.25f);
    return {nearPoint, assets::normalize(farPoint - nearPoint)};
}

bool projectToScreen(const Mat4& viewProj, Vec3 p, float w, float h, Vec2& out, float* depth) noexcept
{
    const Vec4 c = transform(viewProj, p);
    if (c.w <= 1e-4f)
        return false;
    const float iw = 1.0f / c.w;
    out.x = (c.x * iw * 0.5f + 0.5f) * w;
    out.y = (0.5f - c.y * iw * 0.5f) * h;
    if (depth != nullptr)
        *depth = c.z * iw;
    return true;
}

} // namespace vo::client::world
