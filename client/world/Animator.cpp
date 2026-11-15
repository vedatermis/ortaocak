#include "client/world/Animator.h"

#include "client/world/WorldMath.h"

#include <algorithm>
#include <cmath>

namespace vo::client::world {

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
    }
    return "?";
}

void CharacterClips::bind(const assets::Skeleton& skeleton)
{
    idleBinding = idle != nullptr ? assets::bindClip(*idle, skeleton) : assets::ClipBinding{};
    walkBinding = walk != nullptr ? assets::bindClip(*walk, skeleton) : assets::ClipBinding{};
    runBinding = run != nullptr ? assets::bindClip(*run, skeleton) : assets::ClipBinding{};
}

const assets::AnimationClip* CharacterClips::clip(AnimState s) const noexcept
{
    switch (s)
    {
    case AnimState::Idle:
        return idle;
    case AnimState::Walk:
        return walk != nullptr ? walk : idle;
    case AnimState::Run:
        return run != nullptr ? run : (walk != nullptr ? walk : idle);
    }
    return idle;
}

const assets::ClipBinding& CharacterClips::binding(AnimState s) const noexcept
{
    switch (s)
    {
    case AnimState::Idle:
        return idleBinding;
    case AnimState::Walk:
        return walk != nullptr ? walkBinding : idleBinding;
    case AnimState::Run:
        return run != nullptr ? runBinding : (walk != nullptr ? walkBinding : idleBinding);
    }
    return idleBinding;
}

AnimState selectAnimState(AnimState current, float speed, bool running) noexcept
{
    const bool wasMoving = current != AnimState::Idle;
    const bool moving = wasMoving ? speed > kAnimStopSpeed : speed > kAnimStartSpeed;
    if (!moving)
        return AnimState::Idle;
    return running ? AnimState::Run : AnimState::Walk;
}

float playbackRate(const assets::AnimationClip& clip, float speed) noexcept
{
    if (!(clip.moveSpeed > 0.0f))
        return 1.0f;
    return std::clamp(speed / clip.moveSpeed, 0.3f, 3.0f);
}

void Animator::bind(const assets::MeshData* mesh, const CharacterClips* clips)
{
    m_mesh = mesh;
    m_clips = clips;
    m_current = {};
    m_previous = {};
    m_fade = 1.0f;
    m_fading = false;
    m_speed = 0.0f;
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
}

float Animator::fadeWeight() const noexcept
{
    if (!m_fading)
        return 1.0f;
    const float t = std::clamp(m_fade, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t); // smoothstep
}

void Animator::advance(Layer& layer, float dt) const noexcept
{
    const assets::AnimationClip* clip = m_clips != nullptr ? m_clips->clip(layer.state) : nullptr;
    if (clip == nullptr || !(clip->duration > 0.0f))
        return;
    const float rate = layer.state == AnimState::Idle ? 1.0f : playbackRate(*clip, m_speed);
    layer.phase += dt * rate / clip->duration;
    layer.phase -= std::floor(layer.phase);
}

void Animator::update(float dt, float speed, bool running) noexcept
{
    if (!(dt > 0.0f))
        return;
    // Light smoothing: remote speeds come from 10 Hz snapshots.
    m_speed += (std::max(speed, 0.0f) - m_speed) * smoothingFactor(12.0f, dt);
    if (speed <= 0.0f && m_speed < 0.05f)
        m_speed = 0.0f;

    const AnimState next = selectAnimState(m_current.state, m_speed, running);
    if (next != m_current.state)
    {
        // Fading out the layer that dominates right now keeps a fade-during-fade smooth enough.
        m_previous = (m_fading && fadeWeight() < 0.5f) ? m_previous : m_current;
        Layer incoming;
        incoming.state = next;
        // Walk <-> run: same normalised phase (both clips start on the same foot), so the stride
        // continues instead of restarting.
        if (next != AnimState::Idle && m_current.state != AnimState::Idle)
            incoming.phase = m_current.phase;
        m_current = incoming;
        m_fade = 0.0f;
        m_fading = true;
    }
    advance(m_current, dt);
    if (m_fading)
    {
        advance(m_previous, dt);
        m_fade += dt / kFadeTime;
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
        assets::bindPose(m_mesh->skeleton, out);
        return;
    }
    assets::samplePose(*clip, m_clips->binding(layer.state), m_mesh->skeleton, layer.phase * clip->duration, true,
                       out);
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
