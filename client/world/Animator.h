// Per-character animation (no D3D): picks idle / walk / run from the movement state, cross-fades
// between them, scales walk/run playback by actual ground speed (rate = speed / clip.moveSpeed, so
// planted feet do not slide) and evaluates the skinning matrices with the shared Pose helpers
// (shared/assets/Pose.h). Evaluation does not allocate once bound.
#pragma once

#include "shared/assets/Animation.h"
#include "shared/assets/Mesh.h"
#include "shared/assets/Pose.h"

#include <cstdint>
#include <span>
#include <vector>

namespace vo::client::world {

enum class AnimState : std::uint8_t
{
    Idle,
    Walk,
    Run,
};

[[nodiscard]] const char* animStateName(AnimState s) noexcept;

// Clips of one skeleton (one race), bound by bone name. Owned by the asset cache.
struct CharacterClips
{
    const assets::AnimationClip* idle = nullptr;
    const assets::AnimationClip* walk = nullptr;
    const assets::AnimationClip* run = nullptr;
    assets::ClipBinding idleBinding;
    assets::ClipBinding walkBinding;
    assets::ClipBinding runBinding;

    void bind(const assets::Skeleton& skeleton);
    [[nodiscard]] const assets::AnimationClip* clip(AnimState s) const noexcept;
    [[nodiscard]] const assets::ClipBinding& binding(AnimState s) const noexcept;
};

// Thresholds with hysteresis: start moving above kStartSpeed, return to idle below kStopSpeed.
inline constexpr float kAnimStartSpeed = 0.35f;
inline constexpr float kAnimStopSpeed = 0.15f;

// State for ground speed `speed` (m/s) and the running flag, given the current state.
[[nodiscard]] AnimState selectAnimState(AnimState current, float speed, bool running) noexcept;
// Playback rate of a locomotion clip at `speed` (1 for in-place clips), clamped to [0.3, 3].
[[nodiscard]] float playbackRate(const assets::AnimationClip& clip, float speed) noexcept;

class Animator
{
public:
    static constexpr float kFadeTime = 0.18f; // seconds per cross-fade

    // `mesh` and `clips` must outlive the animator (asset cache entries).
    void bind(const assets::MeshData* mesh, const CharacterClips* clips);
    [[nodiscard]] bool bound() const noexcept { return m_mesh != nullptr; }

    // Advances time. `speed` = horizontal ground speed in m/s; it is smoothed internally.
    void update(float dt, float speed, bool running) noexcept;
    // Evaluates the blended pose into skinMatrices().
    void evaluate() noexcept;

    [[nodiscard]] std::span<const assets::Mat4> skinMatrices() const noexcept { return m_skin; }
    [[nodiscard]] AnimState state() const noexcept { return m_current.state; }
    [[nodiscard]] bool fading() const noexcept { return m_fading; }
    [[nodiscard]] float fadeWeight() const noexcept; // weight of the current layer (1 when not fading)
    [[nodiscard]] float smoothedSpeed() const noexcept { return m_speed; }
    [[nodiscard]] float phase() const noexcept { return m_current.phase; }
    // Forces a state (no fade), e.g. for the character preview.
    void setState(AnimState s) noexcept;

private:
    struct Layer
    {
        AnimState state = AnimState::Idle;
        float phase = 0.0f; // normalised 0..1 over the clip duration
    };
    void advance(Layer& layer, float dt) const noexcept;
    void sampleLayer(const Layer& layer, std::vector<assets::Transform>& out) const noexcept;

    const assets::MeshData* m_mesh = nullptr;
    const CharacterClips* m_clips = nullptr;
    Layer m_current;
    Layer m_previous;
    float m_fade = 1.0f; // 0..1 progress of the current fade
    bool m_fading = false;
    float m_speed = 0.0f;
    std::vector<assets::Transform> m_poseA;
    std::vector<assets::Transform> m_poseB;
    std::vector<assets::Mat4> m_model;
    std::vector<assets::Mat4> m_skin;
};

} // namespace vo::client::world
