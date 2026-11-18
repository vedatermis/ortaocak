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
// M9 combat clips.
constexpr float kSpellEnterRate = 1.4f;
constexpr float kHitRate = 1.4f;
constexpr float kDeathRate = 1.0f;

constexpr bool isAttackClip(AnimState s) noexcept
{
    return s == AnimState::SwordAttack || s == AnimState::PunchJab || s == AnimState::PunchCross ||
           s == AnimState::SpellShoot;
}

struct SitStates
{
    AnimState enter;
    AnimState idle;
    AnimState exit;
};

// The ground sit when its three clips exist (M9 stance), else the chair clips of the polish round.
SitStates sitStatesOf(const CharacterClips* clips) noexcept
{
    if (clips != nullptr && clips->has(AnimState::SitGroundEnter) && clips->has(AnimState::SitGroundIdle) &&
        clips->has(AnimState::SitGroundExit))
        return {AnimState::SitGroundEnter, AnimState::SitGroundIdle, AnimState::SitGroundExit};
    return {AnimState::SitEnter, AnimState::SitIdle, AnimState::SitExit};
}

} // namespace

const char* animStateName(AnimState s) noexcept
{
    switch (s)
    {
    case AnimState::Idle: return "idle";
    case AnimState::Walk: return "walk";
    case AnimState::Run: return "run";
    case AnimState::JumpStart: return "jump_start";
    case AnimState::JumpLoop: return "jump_loop";
    case AnimState::JumpLand: return "jump_land";
    case AnimState::SitEnter: return "sit_enter";
    case AnimState::SitIdle: return "sit_idle";
    case AnimState::SitExit: return "sit_exit";
    case AnimState::SwordAttack: return "sword_attack";
    case AnimState::PunchJab: return "punch_jab";
    case AnimState::PunchCross: return "punch_cross";
    case AnimState::SpellEnter: return "spell_enter";
    case AnimState::SpellIdle: return "spell_idle";
    case AnimState::SpellShoot: return "spell_shoot";
    case AnimState::HitChest: return "hit_chest";
    case AnimState::Death: return "death";
    case AnimState::SitGroundEnter: return "sit_ground_enter";
    case AnimState::SitGroundIdle: return "sit_ground_idle";
    case AnimState::SitGroundExit: return "sit_ground_exit";
    case AnimState::IdleTalking: return "idle_talking";
    case AnimState::Interact: return "interact";
    case AnimState::Drink: return "drink";
    }
    return "?";
}

const char* animActionName(AnimAction a) noexcept
{
    switch (a)
    {
    case AnimAction::None: return "none";
    case AnimAction::Jump: return "jump";
    case AnimAction::Sit: return "sit";
    case AnimAction::Attack: return "attack";
    case AnimAction::Hit: return "hit";
    case AnimAction::Cast: return "cast";
    case AnimAction::Dead: return "dead";
    case AnimAction::Interact: return "interact";
    case AnimAction::Drink: return "drink";
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
    m_actionRate = 1.0f;
    m_castShoot = false;
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
    case AnimState::IdleTalking:
    case AnimState::Interact:
    case AnimState::Drink:
        return 1.0f;
    case AnimState::Walk:
    case AnimState::Run:
    {
        // A model drawn at scale k strides k times as far per cycle.
        const assets::AnimationClip* clip = m_clips != nullptr ? m_clips->clip(s) : nullptr;
        return clip != nullptr ? playbackRate(*clip, m_speed / m_moveScale) : 1.0f;
    }
    case AnimState::JumpStart:
        return kJumpStartRate;
    case AnimState::JumpLoop:
        return kJumpLoopRate;
    case AnimState::JumpLand:
        return kJumpLandRate;
    case AnimState::SitEnter:
    case AnimState::SitGroundEnter:
        return kSitEnterRate;
    case AnimState::SitIdle:
    case AnimState::SitGroundIdle:
        return kSitIdleRate;
    case AnimState::SitExit:
    case AnimState::SitGroundExit:
        return kSitExitRate;
    case AnimState::SwordAttack:
    case AnimState::PunchJab:
    case AnimState::PunchCross:
    case AnimState::SpellShoot:
        return m_actionRate;
    case AnimState::SpellEnter:
        return kSpellEnterRate;
    case AnimState::SpellIdle:
        return 1.0f;
    case AnimState::HitChest:
        return kHitRate;
    case AnimState::Death:
        return kDeathRate;
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
    const SitStates s = sitStatesOf(m_clips);
    return m_mesh != nullptr && m_clips != nullptr && m_action == AnimAction::None && m_speed < kAnimStartSpeed &&
           m_clips->has(s.enter) && m_clips->has(s.idle) && m_clips->has(s.exit);
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
    transitionTo(sitStatesOf(m_clips).enter, 0.2f, 0.0f);
    return true;
}

void Animator::standUp() noexcept
{
    if (m_action != AnimAction::Sit)
        return;
    const AnimState s = m_current.state;
    if (s == AnimState::SitEnter || s == AnimState::SitIdle)
        transitionTo(AnimState::SitExit, 0.15f, 0.0f);
    else if (s == AnimState::SitGroundEnter || s == AnimState::SitGroundIdle)
        transitionTo(AnimState::SitGroundExit, 0.15f, 0.0f);
}

void Animator::setSeated(bool seated, bool instant) noexcept
{
    if (!seated)
    {
        standUp();
        return;
    }
    if (m_action == AnimAction::Sit)
    {
        const AnimState s = m_current.state;
        if (s != AnimState::SitExit && s != AnimState::SitGroundExit)
            return; // already sitting (or sitting down)
    }
    else if (m_action != AnimAction::None && m_action != AnimAction::Hit && m_action != AnimAction::Attack)
    {
        return; // dead, jumping or casting: the server only seats a standing, idle character
    }
    const SitStates st = sitStatesOf(m_clips);
    if (m_mesh == nullptr || m_clips == nullptr || !m_clips->has(st.enter) || !m_clips->has(st.idle) ||
        !m_clips->has(st.exit))
        return;
    m_action = AnimAction::Sit;
    m_actionTime = 0.0f;
    if (instant)
    {
        m_current = Layer{st.idle, 0.0f};
        m_fading = false;
        m_fade = 1.0f;
    }
    else
    {
        transitionTo(st.enter, 0.2f, 0.0f);
    }
}

bool Animator::playAttack(AnimState clip, float maxSeconds) noexcept
{
    if (m_mesh == nullptr || m_clips == nullptr || !isAttackClip(clip) || !m_clips->has(clip))
        return false;
    if (m_action == AnimAction::Dead || m_action == AnimAction::Jump || m_action == AnimAction::Sit)
        return false; // (the server stands an attacker up first; its ENTITY_STATE follows)
    const assets::AnimationClip* c = m_clips->clip(clip);
    m_actionRate = 1.0f;
    if (c != nullptr && c->duration > 0.0f && maxSeconds > 0.0f)
        m_actionRate = std::max(1.0f, c->duration / maxSeconds);
    m_action = AnimAction::Attack;
    m_actionTime = 0.0f;
    m_castShoot = false;
    transitionTo(clip, 0.08f, 0.0f);
    return true;
}

bool Animator::playHit() noexcept
{
    if (m_mesh == nullptr || m_clips == nullptr || !m_clips->has(AnimState::HitChest))
        return false;
    if (m_action != AnimAction::None && m_action != AnimAction::Hit)
        return false;
    m_action = AnimAction::Hit;
    m_actionTime = 0.0f;
    transitionTo(AnimState::HitChest, 0.06f, 0.0f);
    return true;
}

bool Animator::beginCast() noexcept
{
    if (m_mesh == nullptr || m_clips == nullptr || !m_clips->has(AnimState::SpellIdle))
        return false;
    if (m_action == AnimAction::Dead || m_action == AnimAction::Jump || m_action == AnimAction::Sit)
        return false;
    m_action = AnimAction::Cast;
    m_actionTime = 0.0f;
    m_castShoot = false;
    transitionTo(m_clips->has(AnimState::SpellEnter) ? AnimState::SpellEnter : AnimState::SpellIdle, 0.1f, 0.0f);
    return true;
}

void Animator::endCast(bool shoot) noexcept
{
    if (m_action != AnimAction::Cast)
        return;
    m_action = AnimAction::None;
    if (shoot && playAttack(AnimState::SpellShoot, 0.7f))
        return;
    transitionTo(selectAnimState(AnimState::Idle, m_speed, m_running), kFadeTime, 0.0f);
}

void Animator::die(bool instant) noexcept
{
    if (m_mesh == nullptr || m_clips == nullptr || !m_clips->has(AnimState::Death))
        return;
    if (m_action == AnimAction::Dead)
        return;
    m_action = AnimAction::Dead;
    m_actionTime = 0.0f;
    m_height = 0.0f;
    if (instant)
    {
        m_current = Layer{AnimState::Death, 1.0f};
        m_fading = false;
        m_fade = 1.0f;
    }
    else
    {
        transitionTo(AnimState::Death, 0.12f, 0.0f);
    }
}

void Animator::revive() noexcept
{
    if (m_action != AnimAction::Dead)
        return;
    m_action = AnimAction::None;
    transitionTo(AnimState::Idle, 0.2f, 0.0f);
}

void Animator::updateAction(float dt) noexcept
{
    m_actionTime += dt;
    const AnimState s = m_current.state;
    const auto backToLocomotion = [this](float fade) {
        m_action = AnimAction::None;
        transitionTo(selectAnimState(AnimState::Idle, m_speed, m_running), fade, 0.0f);
    };
    switch (m_action)
    {
    case AnimAction::Jump:
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
                m_height = 0.0f;
                backToLocomotion(kFadeTime);
            }
        }
        return;
    }
    case AnimAction::Sit:
        if ((s == AnimState::SitEnter || s == AnimState::SitGroundEnter) && m_current.phase >= 1.0f)
            transitionTo(s == AnimState::SitEnter ? AnimState::SitIdle : AnimState::SitGroundIdle, 0.2f, 0.0f);
        else if ((s == AnimState::SitExit || s == AnimState::SitGroundExit) && m_current.phase >= 0.92f)
            backToLocomotion(kFadeTime);
        return;
    case AnimAction::Attack:
        // Most of the follow-through, then blend back (a moving character cuts it shorter).
        if (m_current.phase >= (m_speed > kAnimStartSpeed ? 0.7f : 0.92f))
            backToLocomotion(0.18f);
        return;
    case AnimAction::Hit:
        if (m_current.phase >= 0.85f || m_speed > kAnimStartSpeed)
            backToLocomotion(0.15f);
        return;
    case AnimAction::Cast:
        if (s == AnimState::SpellEnter && m_current.phase >= 1.0f)
            transitionTo(AnimState::SpellIdle, 0.12f, 0.0f);
        return;
    case AnimAction::Interact:
        if (m_current.phase >= 0.95f || m_speed > kAnimStartSpeed)
            backToLocomotion(0.25f);
        return;
    case AnimAction::Drink:
        // fix1: never holds the character (moving blends to the walk / run at once).
        if (m_current.phase >= 0.95f || m_speed > kAnimStartSpeed)
            backToLocomotion(m_speed > kAnimStartSpeed ? 0.15f : 0.25f);
        return;
    case AnimAction::Dead:
    case AnimAction::None:
        return;
    }
}

void Animator::setTalking(bool talking) noexcept
{
    m_talking = talking;
}

bool Animator::playInteract() noexcept
{
    if (m_mesh == nullptr || m_clips == nullptr || !m_clips->has(AnimState::Interact))
        return false;
    if (m_action != AnimAction::None && m_action != AnimAction::Interact)
        return false;
    m_action = AnimAction::Interact;
    m_actionTime = 0.0f;
    transitionTo(AnimState::Interact, 0.15f, 0.0f);
    return true;
}

bool Animator::playDrink() noexcept
{
    if (m_mesh == nullptr || m_clips == nullptr || !m_clips->has(AnimState::Drink))
        return false;
    if (m_action != AnimAction::None && m_action != AnimAction::Drink && m_action != AnimAction::Hit)
        return false; // seated, dead, jumping, attacking, casting: no gesture
    if (m_speed > kAnimStartSpeed)
        return false; // on the move: no gesture (it would stop the stride)
    m_action = AnimAction::Drink;
    m_actionTime = 0.0f;
    transitionTo(AnimState::Drink, 0.15f, 0.0f);
    return true;
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
        AnimState next = selectAnimState(m_current.state == AnimState::IdleTalking ? AnimState::Idle : m_current.state,
                                         m_speed, running);
        if (next == AnimState::Idle && m_talking && m_clips != nullptr && m_clips->has(AnimState::IdleTalking))
            next = AnimState::IdleTalking; // M14: a merchant whose shop is open
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
