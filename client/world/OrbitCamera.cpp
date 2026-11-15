#include "client/world/OrbitCamera.h"

#include <algorithm>
#include <cmath>

namespace vo::client::world {

void OrbitCamera::reset(Vec3 focus, float yaw) noexcept
{
    m_focus = focus;
    m_hasFocus = true;
    m_yaw = wrapAngle(yaw);
    m_distance = m_targetDistance;
    m_effective = m_distance;
    m_eye = boomEye(m_focus, m_distance);
}

void OrbitCamera::setPose(float yaw, float pitch, float distance) noexcept
{
    m_yaw = wrapAngle(yaw);
    m_pitch = std::clamp(pitch, m_params.minPitch, m_params.maxPitch);
    m_targetDistance = std::clamp(distance, m_params.minDistance, m_params.maxDistance);
    m_distance = m_targetDistance;
    m_effective = m_distance;
    if (m_hasFocus)
        m_eye = boomEye(m_focus, m_distance);
}

void OrbitCamera::rotate(float dxPixels, float dyPixels) noexcept
{
    m_yaw = wrapAngle(m_yaw + dxPixels * m_params.radiansPerPixel);
    m_pitch = std::clamp(m_pitch + dyPixels * m_params.radiansPerPixel, m_params.minPitch, m_params.maxPitch);
}

void OrbitCamera::zoom(float notches) noexcept
{
    const float factor = std::pow(m_params.zoomStep, -notches);
    m_targetDistance = std::clamp(m_targetDistance * factor, m_params.minDistance, m_params.maxDistance);
}

Vec3 OrbitCamera::boomEye(Vec3 focus, float distance) const noexcept
{
    const Vec3 f = forwardFromYaw(m_yaw);
    const float c = std::cos(m_pitch);
    const float s = std::sin(m_pitch);
    const Vec3 back{-f.x * c, s, -f.z * c};
    return focus + back * distance;
}

float OrbitCamera::collide(const assets::TerrainData& terrain, Vec3 focus, float distance) const noexcept
{
    constexpr int kSamples = 24;
    const float clearance = m_params.groundClearance;
    float safe = 0.0f;
    for (int i = 1; i <= kSamples; ++i)
    {
        const float d = distance * static_cast<float>(i) / static_cast<float>(kSamples);
        const Vec3 p = boomEye(focus, d);
        if (p.y < terrain.heightAt(p.x, p.z) + clearance)
            return std::max(safe, std::min(m_params.minDistance * 0.5f, distance));
        safe = d;
    }
    return distance;
}

void OrbitCamera::update(Vec3 focus, const assets::TerrainData* terrain, float dt) noexcept
{
    if (!m_hasFocus)
    {
        m_focus = focus;
        m_hasFocus = true;
    }
    else
    {
        const float k = smoothingFactor(m_params.followRate, dt);
        m_focus = m_focus + (focus - m_focus) * k;
        // Never trail more than a couple of metres (teleports, corrections).
        if (assets::length(focus - m_focus) > 3.0f)
            m_focus = focus;
    }
    m_distance += (m_targetDistance - m_distance) * smoothingFactor(m_params.zoomRate, dt);

    float wanted = m_distance;
    const bool hasTerrain = terrain != nullptr && terrain->resolution >= 2;
    if (hasTerrain)
        wanted = collide(*terrain, m_focus, m_distance);
    // Pull in at once when blocked, ease back out when the obstruction is gone.
    if (wanted < m_effective)
        m_effective = wanted;
    else
        m_effective += (wanted - m_effective) * smoothingFactor(m_params.zoomRate, dt);

    m_eye = boomEye(m_focus, m_effective);
    if (hasTerrain)
    {
        const float ground = terrain->heightAt(m_eye.x, m_eye.z) + m_params.groundClearance;
        if (m_eye.y < ground)
            m_eye.y = ground;
    }
}

Mat4 OrbitCamera::view() const noexcept
{
    Vec3 target = m_focus;
    if (assets::length(target - m_eye) < 1e-3f)
        target = m_eye + forwardFromYaw(m_yaw);
    return lookAtLH(m_eye, target, Vec3{0.0f, 1.0f, 0.0f});
}

Mat4 OrbitCamera::projection(float aspect) const noexcept
{
    return perspectiveReverseZ(m_params.fovY, std::max(aspect, 0.01f), m_params.nearZ, m_params.farZ);
}

} // namespace vo::client::world
