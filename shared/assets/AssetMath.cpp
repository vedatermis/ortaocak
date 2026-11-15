#include "shared/assets/AssetMath.h"

#include <algorithm>

namespace vo::assets {

Mat4 composeTrs(Vec3 t, Quat q, Vec3 s) noexcept
{
    q = normalize(q);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 m;
    m.at(0, 0) = (1.0f - 2.0f * (yy + zz)) * s.x;
    m.at(1, 0) = (2.0f * (xy + wz)) * s.x;
    m.at(2, 0) = (2.0f * (xz - wy)) * s.x;
    m.at(3, 0) = 0.0f;
    m.at(0, 1) = (2.0f * (xy - wz)) * s.y;
    m.at(1, 1) = (1.0f - 2.0f * (xx + zz)) * s.y;
    m.at(2, 1) = (2.0f * (yz + wx)) * s.y;
    m.at(3, 1) = 0.0f;
    m.at(0, 2) = (2.0f * (xz + wy)) * s.z;
    m.at(1, 2) = (2.0f * (yz - wx)) * s.z;
    m.at(2, 2) = (1.0f - 2.0f * (xx + yy)) * s.z;
    m.at(3, 2) = 0.0f;
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    m.at(3, 3) = 1.0f;
    return m;
}

float determinant3x3(const Mat4& m) noexcept
{
    return m.at(0, 0) * (m.at(1, 1) * m.at(2, 2) - m.at(1, 2) * m.at(2, 1)) -
           m.at(0, 1) * (m.at(1, 0) * m.at(2, 2) - m.at(1, 2) * m.at(2, 0)) +
           m.at(0, 2) * (m.at(1, 0) * m.at(2, 1) - m.at(1, 1) * m.at(2, 0));
}

Quat quatFromRotationMatrix(const Mat4& m) noexcept
{
    const float m00 = m.at(0, 0), m11 = m.at(1, 1), m22 = m.at(2, 2);
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m.at(2, 1) - m.at(1, 2)) / s;
        q.y = (m.at(0, 2) - m.at(2, 0)) / s;
        q.z = (m.at(1, 0) - m.at(0, 1)) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m.at(2, 1) - m.at(1, 2)) / s;
        q.x = 0.25f * s;
        q.y = (m.at(0, 1) + m.at(1, 0)) / s;
        q.z = (m.at(0, 2) + m.at(2, 0)) / s;
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m.at(0, 2) - m.at(2, 0)) / s;
        q.x = (m.at(0, 1) + m.at(1, 0)) / s;
        q.y = 0.25f * s;
        q.z = (m.at(1, 2) + m.at(2, 1)) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m.at(1, 0) - m.at(0, 1)) / s;
        q.x = (m.at(0, 2) + m.at(2, 0)) / s;
        q.y = (m.at(1, 2) + m.at(2, 1)) / s;
        q.z = 0.25f * s;
    }
    q = normalize(q);
    if (q.w < 0.0f)
        q = {-q.x, -q.y, -q.z, -q.w};
    return q;
}

bool decomposeTrs(const Mat4& m, Vec3& t, Quat& r, Vec3& s, float tolerance) noexcept
{
    t = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    Vec3 c0{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const Vec3 c1{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    const Vec3 c2{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    s = {length(c0), length(c1), length(c2)};
    if (!(s.x > 1e-8f) || !(s.y > 1e-8f) || !(s.z > 1e-8f))
        return false;
    if (determinant3x3(m) < 0.0f)
    {
        s.x = -s.x;
        c0 = -c0;
    }
    Mat4 rot;
    const Vec3 n0 = c0 * (1.0f / std::abs(s.x));
    const Vec3 n1 = c1 * (1.0f / s.y);
    const Vec3 n2 = c2 * (1.0f / s.z);
    rot.at(0, 0) = n0.x;
    rot.at(1, 0) = n0.y;
    rot.at(2, 0) = n0.z;
    rot.at(0, 1) = n1.x;
    rot.at(1, 1) = n1.y;
    rot.at(2, 1) = n1.z;
    rot.at(0, 2) = n2.x;
    rot.at(1, 2) = n2.y;
    rot.at(2, 2) = n2.z;
    r = quatFromRotationMatrix(rot);

    // Reject shear / non-TRS matrices: the recomposition must match.
    const Mat4 back = composeTrs(t, r, s);
    float maxAbs = 1.0f;
    for (int i = 0; i < 12; ++i)
        maxAbs = std::max(maxAbs, std::abs(m.m[static_cast<std::size_t>(i)]));
    for (int i = 0; i < 16; ++i)
    {
        const auto k = static_cast<std::size_t>(i);
        if (std::abs(back.m[k] - m.m[k]) > tolerance * maxAbs)
            return false;
    }
    return true;
}

bool invert(const Mat4& in, Mat4& out) noexcept
{
    const auto& a = in.m;
    std::array<float, 16> inv{};
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
             a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
             a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
             a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
              a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
             a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
             a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
             a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
              a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
             a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
              a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
              a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
             a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
              a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!(std::abs(det) > 1e-20f))
    {
        out = Mat4::identity();
        return false;
    }
    const float invDet = 1.0f / det;
    for (std::size_t i = 0; i < 16; ++i)
        out.m[i] = inv[i] * invDet;
    return true;
}

} // namespace vo::assets
