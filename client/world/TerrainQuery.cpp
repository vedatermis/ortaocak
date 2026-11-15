#include "client/world/TerrainQuery.h"

#include <algorithm>
#include <utility>
#include <cmath>

namespace vo::client::world {

namespace {

// Ray parameter range [t0, t1] inside the zone's x/z rectangle (slab test); false if it misses.
bool clipToZone(const assets::TerrainData& terrain, const Ray& ray, float maxDistance, float& t0, float& t1)
{
    t0 = 0.0f;
    t1 = maxDistance;
    const float size = terrain.worldSize;
    const float o[2] = {ray.origin.x, ray.origin.z};
    const float d[2] = {ray.direction.x, ray.direction.z};
    for (int axis = 0; axis < 2; ++axis)
    {
        if (std::fabs(d[axis]) < 1e-8f)
        {
            if (o[axis] < 0.0f || o[axis] > size)
                return false;
            continue;
        }
        float a = (0.0f - o[axis]) / d[axis];
        float b = (size - o[axis]) / d[axis];
        if (a > b)
            std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1)
            return false;
    }
    return true;
}

float aboveGround(const assets::TerrainData& terrain, const Ray& ray, float t)
{
    const Vec3 p = ray.origin + ray.direction * t;
    return p.y - terrain.heightAt(p.x, p.z);
}

} // namespace

bool raycastTerrain(const assets::TerrainData& terrain, const Ray& ray, float maxDistance, Vec3& hit,
                    float step) noexcept
{
    if (terrain.resolution < 2 || !(step > 0.0f))
        return false;
    float t0 = 0.0f;
    float t1 = 0.0f;
    if (!clipToZone(terrain, ray, maxDistance, t0, t1))
        return false;
    float prevT = t0;
    float prev = aboveGround(terrain, ray, t0);
    if (prev <= 0.0f)
    {
        hit = ray.origin + ray.direction * t0; // starts below the ground (or exactly on it)
        return true;
    }
    for (float t = t0 + step;; t += step)
    {
        const bool last = t >= t1;
        if (last)
            t = t1;
        const float cur = aboveGround(terrain, ray, t);
        if (cur <= 0.0f)
        {
            float lo = prevT;
            float hi = t;
            for (int i = 0; i < 24 && hi - lo > 0.005f; ++i)
            {
                const float mid = 0.5f * (lo + hi);
                if (aboveGround(terrain, ray, mid) > 0.0f)
                    lo = mid;
                else
                    hi = mid;
            }
            hit = ray.origin + ray.direction * hi;
            return true;
        }
        if (last)
            return false;
        prevT = t;
        prev = cur;
    }
}

bool segmentClearOfTerrain(const assets::TerrainData& terrain, Vec3 a, Vec3 b, float clearance,
                           float step) noexcept
{
    const float len = assets::length(b - a);
    const int n = std::max(1, static_cast<int>(std::ceil(len / std::max(step, 0.05f))));
    for (int i = 1; i < n; ++i)
    {
        const Vec3 p = assets::lerp(a, b, static_cast<float>(i) / static_cast<float>(n));
        if (p.y < terrain.heightAt(p.x, p.z) + clearance)
            return false;
    }
    return true;
}

LabelOccluder makeOccluder(const Mat4& world, Vec3 localMin, Vec3 localMax) noexcept
{
    LabelOccluder o;
    if (!assets::invert(world, o.toLocal))
        o.toLocal = Mat4::identity();
    o.localMin = localMin;
    o.localMax = localMax;
    const Vec3 c = (localMin + localMax) * 0.5f;
    o.center = assets::transformPoint(world, c);
    // Radius of the transformed box: half diagonal times the largest axis scale.
    const float sx = assets::length(assets::transformVector(world, {1.0f, 0.0f, 0.0f}));
    const float sy = assets::length(assets::transformVector(world, {0.0f, 1.0f, 0.0f}));
    const float sz = assets::length(assets::transformVector(world, {0.0f, 0.0f, 1.0f}));
    o.radius = assets::length(localMax - c) * std::max({sx, sy, sz});
    return o;
}

bool segmentHitsOccluder(const LabelOccluder& box, Vec3 a, Vec3 b) noexcept
{
    // Quick reject: segment vs. bounding sphere.
    const Vec3 ab = b - a;
    const float len2 = assets::dot(ab, ab);
    const float t = len2 > 0.0f ? std::clamp(assets::dot(box.center - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    const Vec3 closest = a + ab * t;
    if (assets::length(box.center - closest) > box.radius)
        return false;
    // Slab test in the box's local frame.
    const Vec3 la = assets::transformPoint(box.toLocal, a);
    const Vec3 lb = assets::transformPoint(box.toLocal, b);
    const Vec3 d = lb - la;
    float t0 = 0.0f;
    float t1 = 1.0f;
    const float o[3] = {la.x, la.y, la.z};
    const float dir[3] = {d.x, d.y, d.z};
    const float lo[3] = {box.localMin.x, box.localMin.y, box.localMin.z};
    const float hi[3] = {box.localMax.x, box.localMax.y, box.localMax.z};
    for (int i = 0; i < 3; ++i)
    {
        if (std::fabs(dir[i]) < 1e-9f)
        {
            if (o[i] < lo[i] || o[i] > hi[i])
                return false;
            continue;
        }
        float n = (lo[i] - o[i]) / dir[i];
        float f = (hi[i] - o[i]) / dir[i];
        if (n > f)
            std::swap(n, f);
        t0 = std::max(t0, n);
        t1 = std::min(t1, f);
        if (t0 > t1)
            return false;
    }
    return true;
}

} // namespace vo::client::world