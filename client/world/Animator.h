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
    SwordAttack,
    PunchJab,
    PunchCross,
    SpellEnter,
    SpellIdle,
    SpellShoot,
    HitChest,
    Death,
    SitGroundEnter,
    SitGroundIdle,
    SitGroundExit,
    IdleTalking, // M14 merchants
    Interact,
    Drink, // fix1: ITEM_USED (optional clip)
};
inline constexpr std::size_t kAnimStateCount = kCharacterClipCount;
static_assert(static_cast<std::size_t>(AnimState::Drink) + 1 == kAnimStateCount);

[[nodiscard]] const char* animStateName(AnimState s) noexcept;
[[nodiscard]] constexpr bool isLocomotion(AnimState s) noexcept
{
    return s == AnimState::Idle || s == AnimState::Walk || s == AnimState::Run;
}

enum class AnimAction : std::uint8_t
{
    None,
    Jump,
    Sit,    // sitting down, seated or standing up
    Attack, // M9: one swing / punch / spell shot, then back to locomotion
    Hit,    // M9: hit reaction (hit_chest)
    Cast,   // M9: spell_enter -> spell_idle until endCast() (-> spell_shoot)
    Dead,   // M9: death clip, held on its last frame until revive()
    Interact, // M14: a merchant's reach on a purchase / sale (interact), then back to the idle
    Drink,    // fix1: a potion drunk (drink), then back to locomotion; moving cuts it short
};
[[nodiscard]] const char* animActionName(AnimAction a) noexcept;

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
    // Sits on the ground (sit_ground_*; the chair clips sit_* when the ground ones are missing).
    bool toggleSit() noexcept;
    void standUp() noexcept; // sitting down or seated -> sit_exit
    // Server-driven stance (ENTITY_STATE bit2): sit down / stand up; `instant` jumps straight to the
    // seated loop (a remote that spawns sitting).
    void setSeated(bool seated, bool instant) noexcept;
    [[nodiscard]] bool canJump() const noexcept;
    [[nodiscard]] bool canSit() const noexcept;
    [[nodiscard]] AnimAction action() const noexcept { return m_action; }
    // Seated (or sitting down / standing up): the character must not move.
    [[nodiscard]] bool seated() const noexcept { return m_action == AnimAction::Sit; }
    // In the seated loop (sit_idle or sit_ground_idle).
    [[nodiscard]] bool sittingIdle() const noexcept
    {
        return m_action == AnimAction::Sit &&
               (m_current.state == AnimState::SitIdle || m_current.state == AnimState::SitGroundIdle);
    }

    // ---- Combat (M9, docs/PROTOCOL_COMBAT.md §5) ----
    // A swing / punch / spell shot (`clip` is SwordAttack, PunchJab, PunchCross or SpellShoot) lasting
    // at most `maxSeconds`. Refused while dead or jumping; replaces a hit reaction, ends a cast.
    bool playAttack(AnimState clip, float maxSeconds = 0.9f) noexcept;
    // Hit reaction; only when no other action is running (not mid-attack, casting, seated, dead).
    bool playHit() noexcept;
    // Starts / ends a cast (spell_enter -> spell_idle); endCast(true) plays spell_shoot.
    bool beginCast() noexcept;
    void endCast(bool shoot) noexcept;
    // Death clip held on its last frame; `instant` skips to the last frame (a corpse that spawns dead).
    void die(bool instant = false) noexcept;
    void revive() noexcept;
    [[nodiscard]] bool dead() const noexcept { return m_action == AnimAction::Dead; }
    [[nodiscard]] bool has(AnimState s) const noexcept { return m_clips != nullptr && m_clips->has(s); }
    // ---- Merchants (M14, docs/SHOP_ART.md §1) ----
    // While talking, the standing idle is idle_talking (when the clip exists); cross-faded.
    void setTalking(bool talking) noexcept;
    [[nodiscard]] bool talking() const noexcept { return m_talking; }
    // One interact clip (a purchase / sale), then back to the idle; false without the clip or busy.
    bool playInteract() noexcept;
    // fix1: the drink clip once (a potion, ITEM_USED); false without the clip, seated, dead, jumping or in
    // another action. It never holds the character: moving blends back to the walk / run at once.
    bool playDrink() noexcept;
    // Scales the clips' planted-foot speed (a monster drawn at 1.25x strides 1.25x as far).
    void setMoveScale(float scale) noexcept { m_moveScale = scale > 0.0f ? scale : 1.0f; }
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
    float m_actionRate = 1.0f; // playback rate of the attack clip
    float m_moveScale = 1.0f;
    bool m_castShoot = false; // Cast: spell_shoot after spell_idle
    bool m_talking = false;   // M14: idle_talking instead of idle
    float m_height = 0.0f;
    std::vector<assets::Transform> m_poseA;
    std::vector<assets::Transform> m_poseB;
    std::vector<assets::Mat4> m_model;
    std::vector<assets::Mat4> m_skin;
};

} // namespace vo::client::world
