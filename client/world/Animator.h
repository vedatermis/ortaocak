// Per-character animation (no D3D): picks idle / walk / run from the movement state, cross-fades
// between them, scales walk/run playback by actual ground speed (rate = speed / clip.moveSpeed, so
// planted feet do not slide; clamped to a sane 0.6-1.6x so odd speeds never look broken) and
// evaluates the skinning matrices with the shared Pose helpers (shared/assets/Pose.h).
//
// Cosmetic one-shot actions (local player only, nothing goes over the network):
//   jump()      jump_start -> jump_loop -> jump_land with a small vertical arc (heightOffset());
//   toggleSit() sit_enter -> sit_idle, and back up with sit_exit (standUp()).
// An action replaces the locomotion pose (full body, cross-faded); it needs all of its clips.
// Evaluation does not allocate once bound.
#pragma once

#include "client/world/CharacterAssets.h"
#include "shared/assets/Animation.h"
#include "shared/assets/Mesh.h"
#include "shared/assets/Pose.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vo::client::world {

// Same order as CharacterClip.
enum class AnimState : std::uint8_t
{
    Idle,
    Walk,
    Run,
    JumpStart,
    JumpLoop,
    JumpLand,
    SitEnter,
    SitIdle,
    SitExit,
};
inline constexpr std::size_t kAnimStateCount = kCharacterClipCount;

[[nodiscard]] const char* animStateName(AnimState s) noexcept;
[[nodiscard]] constexpr bool isLocomotion(AnimState s) noexcept
{
    return s == AnimState::Idle || s == AnimState::Walk || s == AnimState::Run;
}

enum class AnimAction : std::uint8_t
{
    None,
    Jump,
    Sit, // sitting down, seated or standing up
};

// Clips of one skeleton (one race + look), bound by bone name. Owned by the asset cache.
struct CharacterClips
{
    std::array<const assets::AnimationClip*, kAnimStateCount> clips{};
    std::array<assets::ClipBinding, kAnimStateCount> bindings;

    void set(AnimState s, const assets::AnimationClip* clip) noexcept { clips[static_cast<std::size_t>(s)] = clip; }
    void bind(const assets::Skeleton& skeleton);
    [[nodiscard]] bool has(AnimState s) const noexcept { return clips[static_cast<std::size_t>(s)] != nullptr; }
    // Locomotion falls back (run -> walk -> idle); an action state without its clip gives nullptr.
    [[nodiscard]] const assets::AnimationClip* clip(AnimState s) const noexcept;
    [[nodiscard]] const assets::ClipBinding& binding(AnimState s) const noexcept;

private:
    [[nodiscard]] AnimState resolve(AnimState s) const noexcept;
};

// Thresholds with hysteresis: start moving above kStartSpeed, return to idle below kStopSpeed.
inline constexpr float kAnimStartSpeed = 0.35f;
inline constexpr float kAnimStopSpeed = 0.15f;
inline constexpr float kMinPlaybackRate = 0.6f;
inline constexpr float kMaxPlaybackRate = 1.6f;

// State for ground speed `speed` (m/s) and the running flag, given the current state.
[[nodiscard]] AnimState selectAnimState(AnimState current, float speed, bool running) noexcept;
// Playback rate of a locomotion clip at `speed` (1 for in-place clips), clamped to
// [kMinPlaybackRate, kMaxPlaybackRate].
[[nodiscard]] float playbackRate(const assets::AnimationClip& clip, float speed) noexcept;

class Animator
{
public:
    static constexpr float kFadeTime = 0.25f;       // seconds per locomotion cross-fade
    static constexpr float kActionFadeTime = 0.12f; // into / between action clips
    // Jump timeline (seconds from the key press): take-off, time in the air, peak height.
    static constexpr float kJumpTakeoff = 0.24f;
    static constexpr float kJumpAirTime = 0.62f;
    static constexpr float kJumpHeight = 0.42f;

    // `mesh` and `clips` must outlive the animator (asset cache entries).
    void bind(const assets::MeshData* mesh, const CharacterClips* clips);
    [[nodiscard]] bool bound() const noexcept { return m_mesh != nullptr; }

    // Advances time. `speed` = horizontal ground speed in m/s; it is smoothed internally.
    void update(float dt, float speed, bool running) noexcept;
    // Evaluates the blended pose into skinMatrices().
    void evaluate() noexcept;

    // ---- One-shot actions (false if busy with another action or the clips are missing) ----
    bool jump() noexcept;
    bool toggleSit() noexcept;
    void standUp() noexcept; // sitting down or seated -> sit_exit
    [[nodiscard]] bool canJump() const noexcept;
    [[nodiscard]] bool canSit() const noexcept;
    [[nodiscard]] AnimAction action() const noexcept { return m_action; }
    // Seated (or sitting down / standing up): the character must not move.
    [[nodiscard]] bool seated() const noexcept { return m_action == AnimAction::Sit; }
    // Vertical offset of the jump arc (0 on the ground), metres.
    [[nodiscard]] float heightOffset() const noexcept { return m_height; }

    [[nodiscard]] std::span<const assets::Mat4> skinMatrices() const noexcept { return m_skin; }
    // Model-space bone transforms of the last evaluate() (automation: foot contact probes).
    [[nodiscard]] std::span<const assets::Mat4> modelMatrices() const noexcept { return m_model; }
    [[nodiscard]] const assets::MeshData* mesh() const noexcept { return m_mesh; }
    [[nodiscard]] AnimState state() const noexcept { return m_current.state; }
    [[nodiscard]] bool fading() const noexcept { return m_fading; }
    [[nodiscard]] float fadeWeight() const noexcept; // weight of the current layer (1 when not fading)
    [[nodiscard]] float smoothedSpeed() const noexcept { return m_speed; }
    [[nodiscard]] float phase() const noexcept { return m_current.phase; }
    // Playback rate of the current layer this frame (automation / tests).
    [[nodiscard]] float currentRate() const noexcept { return m_rate; }
    // Forces a state (no fade), e.g. for the character preview.
    void setState(AnimState s) noexcept;

private:
    struct Layer
    {
        AnimState state = AnimState::Idle;
        float phase = 0.0f; // normalised 0..1 over the clip duration
    };
    [[nodiscard]] float rateOf(AnimState s) const noexcept;
    void advance(Layer& layer, float dt) noexcept;
    void sampleLayer(const Layer& layer, std::vector<assets::Transform>& out) const noexcept;
    void transitionTo(AnimState next, float fadeTime, float phase) noexcept;
    void updateAction(float dt) noexcept;

    const assets::MeshData* m_mesh = nullptr;
    const CharacterClips* m_clips = nullptr;
    Layer m_current;
    Layer m_previous;
    float m_fade = 1.0f; // 0..1 progress of the current fade
    float m_fadeTime = kFadeTime;
    bool m_fading = false;
    float m_speed = 0.0f;
    bool m_running = false;
    float m_rate = 1.0f;
    AnimAction m_action = AnimAction::None;
    float m_actionTime = 0.0f;
    float m_height = 0.0f;
    std::vector<assets::Transform> m_poseA;
    std::vector<assets::Transform> m_poseB;
    std::vector<assets::Mat4> m_model;
    std::vector<assets::Mat4> m_skin;
};

} // namespace vo::client::world
