#include "client/world/Animator.h"

#include "client/world/WorldMath.h"

#include <algorithm>
#include <cmath>

namespace vo::client::world {

namespace {

constexpr std::size_t idx(AnimState s) noexcept
{
    return static_cast<std::size_t>(s);
}

// Fixed playback rates of the action clips (the library jump/sit clips are slow and theatrical).
constexpr float kJumpStartRate = 1.3f;
constexpr float kJumpLoopRate = 1.0f;
constexpr float kJumpLandRate = 1.4f;
constexpr float kSitEnterRate = 1.0f;
constexpr float kSitIdleRate = 1.0f;
constexpr float kSitExitRate = 1.15f;

} // namespace

const char* animStateName(AnimState s) noexcept
{
    switch (s)
    {
    case AnimState::Idle:
        return "idle";
    case AnimState::Walk:
        return "walk";
    case AnimState::Run:
        return "run";
    case AnimState::JumpStart:
        return "jump_start";
    case AnimState::JumpLoop:
        return "jump_loop";
    case AnimState::JumpLand:
        return "jump_land";
    case AnimState::SitEnter:
        return "sit_enter";
    case AnimState::SitIdle:
        return "sit_idle";
    case AnimState::SitExit:
        return "sit_exit";
    }
    return "?";
}

void CharacterClips::bind(const assets::Skeleton& skeleton)
{
    for (std::size_t i = 0; i < kAnimStateCount; ++i)
        bindings[i] = clips[i] != nullptr ? assets::bindClip(*clips[i], skeleton) : assets::ClipBinding{};
}

AnimState CharacterClips::resolve(AnimState s) const noexcept
{
    switch (s)
    {
    case AnimState::Run:
        if (has(AnimState::Run))
            return AnimState::Run;
        [[fallthrough]];
    case AnimState::Walk:
        if (has(AnimState::Walk))
            return AnimState::Walk;
        return AnimState::Idle;
    default:
        return s;
    }
}

const assets::AnimationClip* CharacterClips::clip(AnimState s) const noexcept
{
    return clips[idx(resolve(s))];
}

const assets::ClipBinding& CharacterClips::binding(AnimState s) const noexcept
{
    return bindings[idx(resolve(s))];
}

AnimState selectAnimState(AnimState current, float speed, bool running) noexcept
{
    const bool wasMoving = current == AnimState::Walk || current == AnimState::Run;
    const bool moving = wasMoving ? speed > kAnimStopSpeed : speed > kAnimStartSpeed;
    if (!moving)
        return AnimState::Idle;
    return running ? AnimState::Run : AnimState::Walk;
}

float playbackRate(const assets::AnimationClip& clip, float speed) noexcept
{
    if (!(clip.moveSpeed > 0.0f))
        return 1.0f;
    return std::clamp(speed / clip.moveSpeed, kMinPlaybackRate, kMaxPlaybackRate);
}

void Animator::bind(const assets::MeshData* mesh, const CharacterClips* clips)
{
    m_mesh = mesh;
    m_clips = clips;
    m_current = {};
    m_previous = {};
    m_fade = 1.0f;
    m_fadeTime = kFadeTime;
    m_fading = false;
    m_speed = 0.0f;
    m_rate = 1.0f;
    m_action = AnimAction::None;
    m_actionTime = 0.0f;
    m_height = 0.0f;
    const std::size_t bones = mesh != nullptr ? mesh->skeleton.size() : 0;
    m_poseA.resize(bones);
    m_poseB.resize(bones);
    m_model.resize(bones);
    m_skin.assign(bones, assets::Mat4::identity());
}

void Animator::setState(AnimState s) noexcept
{
    m_current.state = s;
    m_fading = false;
    m_fade = 1.0f;
    m_action = AnimAction::None;
    m_height = 0.0f;
}

float Animator::fadeWeight() const noexcept
{
    if (!m_fading)
        return 1.0f;
    const float t = std::clamp(m_fade, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t); // smoothstep
}

float Animator::rateOf(AnimState s) const noexcept
{
    switch (s)
    {
    case AnimState::Idle:
        return 1.0f;
    case AnimState::Walk:
    case AnimState::Run:
    {
        const assets::AnimationClip* clip = m_clips != nullptr ? m_clips->clip(s) : nullptr;
        return clip != nullptr ? playbackRate(*clip, m_speed) : 1.0f;
    }
    case AnimState::JumpStart:
        return kJumpStartRate;
    case AnimState::JumpLoop:
        return kJumpLoopRate;
    case AnimState::JumpLand:
        return kJumpLandRate;
    case AnimState::SitEnter:
        return kSitEnterRate;
    case AnimState::SitIdle:
        return kSitIdleRate;
    case AnimState::SitExit:
        return kSitExitRate;
    }
    return 1.0f;
}

void Animator::advance(Layer& layer, float dt) noexcept
{
    const assets::AnimationClip* clip = m_clips != nullptr ? m_clips->clip(layer.state) : nullptr;
    if (clip == nullptr || !(clip->duration > 0.0f))
    {
        layer.phase = std::min(layer.phase + dt, 1.0f); // missing clip: still lets one-shots finish
        return;
    }
    layer.phase += dt * rateOf(layer.state) / clip->duration;
    if (clip->loop)
        layer.phase -= std::floor(layer.phase);
    else
        layer.phase = std::min(layer.phase, 1.0f);
}

void Animator::transitionTo(AnimState next, float fadeTime, float phase) noexcept
{
    // Fading out the layer that dominates right now keeps a fade-during-fade smooth enough.
    m_previous = (m_fading && fadeWeight() < 0.5f) ? m_previous : m_current;
    m_current = Layer{next, phase};
    m_fade = 0.0f;
    m_fadeTime = std::max(fadeTime, 1e-3f);
    m_fading = true;
}

bool Animator::canJump() const noexcept
{
    return m_mesh != nullptr && m_clips != nullptr && m_action == AnimAction::None &&
           m_clips->has(AnimState::JumpStart) && m_clips->has(AnimState::JumpLoop) && m_clips->has(AnimState::JumpLand);
}

bool Animator::canSit() const noexcept
{
    return m_mesh != nullptr && m_clips != nullptr && m_action == AnimAction::None && m_speed < kAnimStartSpeed &&
           m_clips->has(AnimState::SitEnter) && m_clips->has(AnimState::SitIdle) && m_clips->has(AnimState::SitExit);
}

bool Animator::jump() noexcept
{
    if (!canJump())
        return false;
    m_action = AnimAction::Jump;
    m_actionTime = 0.0f;
    m_height = 0.0f;
    transitionTo(AnimState::JumpStart, kActionFadeTime, 0.0f);
    return true;
}

bool Animator::toggleSit() noexcept
{
    if (m_action == AnimAction::Sit)
    {
        standUp();
        return true;
    }
    if (!canSit())
        return false;
    m_action = AnimAction::Sit;
    m_actionTime = 0.0f;
    transitionTo(AnimState::SitEnter, 0.2f, 0.0f);
    return true;
}

void Animator::standUp() noexcept
{
    if (m_action != AnimAction::Sit)
        return;
    if (m_current.state == AnimState::SitEnter || m_current.state == AnimState::SitIdle)
        transitionTo(AnimState::SitExit, 0.15f, 0.0f);
}

void Animator::updateAction(float dt) noexcept
{
    m_actionTime += dt;
    const AnimState s = m_current.state;
    if (m_action == AnimAction::Jump)
    {
        const float t = m_actionTime;
        const float airStart = kJumpTakeoff;
        const float airEnd = kJumpTakeoff + kJumpAirTime;
        if (t > airStart && t < airEnd)
        {
            const float u = (t - airStart) / kJumpAirTime;
            m_height = kJumpHeight * 4.0f * u * (1.0f - u);
        }
        else
        {
            m_height = 0.0f;
        }
        if (s == AnimState::JumpStart && t >= airStart + 0.16f)
            transitionTo(AnimState::JumpLoop, kActionFadeTime, 0.0f);
        else if ((s == AnimState::JumpStart || s == AnimState::JumpLoop) && t >= airEnd - 0.04f)
            transitionTo(AnimState::JumpLand, 0.08f, 0.0f);
        else if (s == AnimState::JumpLand)
        {
            // Moving characters skip most of the recovery crouch.
            const bool moving = m_speed > kAnimStartSpeed;
            if (m_current.phase >= (moving ? 0.3f : 0.6f))
            {
                m_action = AnimAction::None;
                m_height = 0.0f;
                transitionTo(selectAnimState(AnimState::Idle, m_speed, m_running), kFadeTime, 0.0f);
            }
        }
        return;
    }
    if (m_action == AnimAction::Sit)
    {
        if (s == AnimState::SitEnter && m_current.phase >= 1.0f)
            transitionTo(AnimState::SitIdle, 0.2f, 0.0f);
        else if (s == AnimState::SitExit && m_current.phase >= 0.92f)
        {
            m_action = AnimAction::None;
            transitionTo(selectAnimState(AnimState::Idle, m_speed, m_running), kFadeTime, 0.0f);
        }
    }
}

void Animator::update(float dt, float speed, bool running) noexcept
{
    if (!(dt > 0.0f))
        return;
    // Light smoothing: remote speeds come from 10 Hz snapshots.
    m_speed += (std::max(speed, 0.0f) - m_speed) * smoothingFactor(12.0f, dt);
    if (speed <= 0.0f && m_speed < 0.05f)
        m_speed = 0.0f;
    m_running = running;

    if (m_action != AnimAction::None)
    {
        updateAction(dt);
    }
    else
    {
        const AnimState next = selectAnimState(m_current.state, m_speed, running);
        if (next != m_current.state)
        {
            // Walk <-> run: same normalised phase (both clips start on the same foot), so the stride
            // continues instead of restarting.
            const bool stride = next != AnimState::Idle &&
                                (m_current.state == AnimState::Walk || m_current.state == AnimState::Run);
            transitionTo(next, kFadeTime, stride ? m_current.phase : 0.0f);
        }
    }
    advance(m_current, dt);
    m_rate = rateOf(m_current.state);
    if (m_fading)
    {
        advance(m_previous, dt);
        m_fade += dt / m_fadeTime;
        if (m_fade >= 1.0f)
        {
            m_fade = 1.0f;
            m_fading = false;
        }
    }
}

void Animator::sampleLayer(const Layer& layer, std::vector<assets::Transform>& out) const noexcept
{
    const assets::AnimationClip* clip = m_clips != nullptr ? m_clips->clip(layer.state) : nullptr;
    if (clip == nullptr)
    {
        if (m_clips != nullptr && m_clips->clip(AnimState::Idle) != nullptr)
        {
            const assets::AnimationClip* idle = m_clips->clip(AnimState::Idle);
            assets::samplePose(*idle, m_clips->binding(AnimState::Idle), m_mesh->skeleton, 0.0f, true, out);
            return;
        }
        assets::bindPose(m_mesh->skeleton, out);
        return;
    }
    assets::samplePose(*clip, m_clips->binding(layer.state), m_mesh->skeleton, layer.phase * clip->duration,
                       clip->loop, out);
}

void Animator::evaluate() noexcept
{
    if (m_mesh == nullptr || m_mesh->skeleton.empty())
        return;
    sampleLayer(m_current, m_poseA);
    if (m_fading)
    {
        sampleLayer(m_previous, m_poseB);
        const float w = fadeWeight();
        for (std::size_t b = 0; b < m_poseA.size(); ++b)
        {
            assets::Transform& dst = m_poseA[b];
            const assets::Transform& from = m_poseB[b];
            dst.translation = assets::lerp(from.translation, dst.translation, w);
            dst.rotation = assets::nlerp(from.rotation, dst.rotation, w);
            dst.scale = assets::lerp(from.scale, dst.scale, w);
        }
    }
    assets::localToModel(m_mesh->skeleton, m_poseA, m_model);
    assets::computeSkinningMatrices(m_mesh->skeleton, m_model, m_skin);
}

} // namespace vo::client::world
