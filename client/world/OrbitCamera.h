// Third-person orbit camera (no D3D): yaw/pitch/distance around a smoothed focus point above the
// character, with zoom limits, a pitch clamp and collision: the eye never goes below the heightfield
// (the boom is shortened at the first obstruction and the eye is lifted above the ground) and never
// into a static obstacle box (CameraObstacles: houses, towers, wells, stalls, big rocks; the boom is
// pulled in along the view ray to just outside the first box). Pull-in is immediate (the eye never
// shows the inside of a wall), easing back out is smooth. Right-mouse dragging feeds rotate(), the
// wheel feeds zoom().
#pragma once

#include "client/world/WorldMath.h"
#include "shared/assets/Terrain.h"

namespace vo::client::world {

class CameraObstacles;

struct OrbitCameraParams
{
    float minDistance = 2.0f;
    float maxDistance = 28.0f;
    float minPitch = -0.35f; // radians; negative looks up from below the focus
    float maxPitch = 1.40f;
    float radiansPerPixel = 0.0055f;
    float zoomStep = 1.15f;       // distance factor per wheel notch
    float followRate = 14.0f;     // 1/s focus smoothing
    float zoomRate = 10.0f;       // 1/s distance smoothing
    float groundClearance = 0.45f; // metres the eye keeps above the terrain
    float fovY = 1.0472f;          // 60 degrees
    float nearZ = 0.1f;
    float farZ = 1500.0f;
};

class OrbitCamera
{
public:
    static constexpr float kMinObstacleDistance = 0.15f; // boom never shorter than this
    explicit OrbitCamera(OrbitCameraParams params = {}) : m_params(params) {}

    // Places the camera behind a character facing `yaw` (default view) without smoothing.
    void reset(Vec3 focus, float yaw) noexcept;
    void setPose(float yaw, float pitch, float distance) noexcept;

    void rotate(float dxPixels, float dyPixels) noexcept;
    void zoom(float notches) noexcept; // + = closer
    // Smooths towards `focus` and resolves terrain collision. terrain may be null (tests).
    void update(Vec3 focus, const assets::TerrainData* terrain, float dt) noexcept;
    // Static obstacles (may be null; must outlive the camera's use of it).
    void setObstacles(const CameraObstacles* obstacles) noexcept { m_obstacles = obstacles; }
    [[nodiscard]] const CameraObstacles* obstacles() const noexcept { return m_obstacles; }
    // Boom length kept clear of the obstacles (<= distance).
    [[nodiscard]] float collideObstacles(Vec3 focus, float distance) const noexcept;

    [[nodiscard]] float yaw() const noexcept { return m_yaw; }
    [[nodiscard]] float pitch() const noexcept { return m_pitch; }
    [[nodiscard]] float distance() const noexcept { return m_distance; }        // smoothed boom length
    [[nodiscard]] float targetDistance() const noexcept { return m_targetDistance; }
    [[nodiscard]] float effectiveDistance() const noexcept { return m_effective; } // after collision
    [[nodiscard]] Vec3 eye() const noexcept { return m_eye; }
    [[nodiscard]] Vec3 focus() const noexcept { return m_focus; }
    [[nodiscard]] const OrbitCameraParams& params() const noexcept { return m_params; }

    [[nodiscard]] Mat4 view() const noexcept;
    [[nodiscard]] Mat4 projection(float aspect) const noexcept;

    // Eye position for a boom of `distance` (no collision).
    [[nodiscard]] Vec3 boomEye(Vec3 focus, float distance) const noexcept;
    // Collision: shortest boom length <= distance that keeps the eye clear of the terrain.
    [[nodiscard]] float collide(const assets::TerrainData& terrain, Vec3 focus, float distance) const noexcept;

private:
    OrbitCameraParams m_params;
    const CameraObstacles* m_obstacles = nullptr;
    float m_yaw = 0.0f;
    float m_pitch = 0.35f;
    float m_distance = 7.0f;
    float m_targetDistance = 7.0f;
    float m_effective = 7.0f;
    Vec3 m_focus{};
    Vec3 m_eye{};
    bool m_hasFocus = false;
};

} // namespace vo::client::world
