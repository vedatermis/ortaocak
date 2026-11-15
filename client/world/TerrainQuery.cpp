#include "client/world/TerrainQuery.h"

#include <algorithm>
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

} // namespace vo::client::world
