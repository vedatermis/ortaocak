// Client-side skill logic (M12 "Yetenek Yolları", docs/PROTOCOL_SKILLS.md §7), no D3D, no sockets:
// everything here is pure and unit-tested (tests/client/SkillsViewTests.cpp). The server stays
// authoritative (SKILL_STATE, SKILL_BAR, SKILL_*_RES, EFFECT_*); these helpers keep what it said, the
// one learn / bar request in flight, the timed effects of every entity, and produce the Turkish texts.
//
//   SkillBook             ranks, points, the bar, the learn and bar requests (one in flight each)
//   barWith*()            bar edits of the drag & drop / right click (one SKILL_BAR_SET_REQ each)
//   EffectBook            timed effects per entity (EFFECT_LIST / APPLY / REMOVE, death, despawn)
//   decideSkillUse()      which target a skill of the bar is used on (§7.1) or why not
//   slotLook()            the tint of a bar slot (no mana: red, no / far target: dark, cooldown)
//   skillTipLines()       the tooltip of a skill (§7.3)
//   effectTipText()       the tooltip line of an effect icon (§7.7)
//   learnResultText() ... the texts of §7.4
//   skillUseArt()         the effect meshes a SKILL_USED shows (docs/SKILL_ART.md)
//   effectAura()          the mesh an active effect keeps on its entity (bubble, aura, stun stars)
//   parseSkillsCommand()  the skill `scene` automation commands (syntax errors name the problem)
#pragma once

#include "client/world/LootView.h"
#include "shared/game/Items.h"
#include "shared/game/Skills.h"
#include "shared/net/Messages.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::client::world {

// ---- Ranks, points, bar ----

class SkillBook
{
public:
    // World entry: the class's default ranks (free skill 1) and bar, nothing known from the server yet.
    void reset(std::uint8_t charClass, std::uint16_t level);
    // LEVEL_UP (the local rules use the level; the server's unspent follows with SKILL_STATE).
    void setLevel(std::uint16_t level) noexcept { m_level = level; }

    // SKILL_STATE: the whole state (ranks of the class only, the free skill at least 1).
    void applyState(const vo::net::SkillState& m);
    [[nodiscard]] bool known() const noexcept { return m_known; }
    [[nodiscard]] std::uint8_t charClass() const noexcept { return m_class; }
    [[nodiscard]] std::uint16_t level() const noexcept { return m_level; }
    [[nodiscard]] std::uint16_t points() const noexcept { return m_points; }
    [[nodiscard]] std::uint16_t unspent() const noexcept { return m_unspent; }
    [[nodiscard]] std::uint8_t rank(std::uint16_t skillId) const noexcept;
    [[nodiscard]] const game::SkillRanks& ranks() const noexcept { return m_ranks; }
    // Points spent in a branch of the class (Σ rank - base).
    [[nodiscard]] std::uint32_t branchSpent(std::uint8_t branch) const noexcept;
    // The class's skills of a branch, by tier then id.
    [[nodiscard]] std::vector<const game::SkillDef*> branchSkills(std::uint8_t branch) const;

    // ---- Learning (SKILL_LEARN_REQ) ----
    // The shared rule (game::checkSkillLearn with the local level) and the server's unspent > 0.
    [[nodiscard]] vo::net::SkillLearnResult check(std::uint16_t skillId) const noexcept;
    // The tier gate of the skill is met (points spent in its branch).
    [[nodiscard]] bool gateMet(const game::SkillDef& s) const noexcept;
    // "+" is usable: known, nothing in flight, check() Ok.
    [[nodiscard]] bool canRaise(std::uint16_t skillId) const noexcept;
    [[nodiscard]] bool learnBusy() const noexcept { return m_learnSeq != 0; }
    [[nodiscard]] std::uint16_t learnSkill() const noexcept { return m_learnSkill; }
    // The request for one rank more (nullopt unless canRaise()).
    std::optional<vo::net::SkillLearnReq> beginLearn(std::uint16_t skillId);
    struct LearnAnswer
    {
        bool matched = false; // the seq was the one in flight
        vo::net::SkillLearnResult result = vo::net::SkillLearnResult::Ok;
        std::uint16_t skillId = 0;
        std::uint8_t rank = 0;
    };
    // SKILL_LEARN_RES: the answer's rank and unspent are the truth (SKILL_STATE follows an Ok).
    LearnAnswer finishLearn(const vo::net::SkillLearnRes& m);
    // The request could not be sent: nothing in flight any more.
    void abandonLearn() noexcept;

    // ---- The bar ----
    [[nodiscard]] const game::SkillBar& bar() const noexcept { return m_bar; }
    [[nodiscard]] std::uint16_t slot(std::size_t i) const noexcept { return i < m_bar.size() ? m_bar[i] : 0; }
    // SKILL_BAR (world entry).
    void applyBar(const vo::net::SkillBar& m);
    [[nodiscard]] bool barBusy() const noexcept { return m_barSeq != 0; }
    [[nodiscard]] bool barQueued() const noexcept { return m_barQueued.has_value(); }
    // A change of the bar: the request to send now, or nullopt (no change; or one is in flight: the
    // wish is kept and sent by finishBar, only the last one).
    std::optional<vo::net::SkillBarSetReq> requestBar(const game::SkillBar& wanted);
    struct BarAnswer
    {
        bool matched = false;
        vo::net::SkillBarResult result = vo::net::SkillBarResult::Ok;
        std::optional<vo::net::SkillBarSetReq> next; // the queued wish, to send now
    };
    // SKILL_BAR_SET_RES: the bar shown becomes the stored one; a queued wish goes out next.
    BarAnswer finishBar(const vo::net::SkillBarSetRes& m);
    void abandonBar() noexcept;

private:
    std::uint8_t m_class = 1;
    std::uint16_t m_level = 1;
    bool m_known = false;
    std::uint16_t m_points = 0;
    std::uint16_t m_unspent = 0;
    game::SkillRanks m_ranks{};
    game::SkillBar m_bar{};
    std::uint32_t m_learnSeq = 0;
    std::uint16_t m_learnSkill = 0;
    std::uint32_t m_barSeq = 0;
    std::optional<game::SkillBar> m_barQueued;
    std::uint32_t m_nextSeq = 1;
};

// Bar edits (slots 0..7; out-of-range slots change nothing).
[[nodiscard]] game::SkillBar barWithPlaced(const game::SkillBar& bar, std::size_t slot, std::uint16_t skillId) noexcept;
[[nodiscard]] game::SkillBar barWithSwapped(const game::SkillBar& bar, std::size_t a, std::size_t b) noexcept;
[[nodiscard]] game::SkillBar barWithCleared(const game::SkillBar& bar, std::size_t slot) noexcept;

// ---- Timed effects ----

struct ShownEffect
{
    std::uint16_t skillId = 0;
    std::uint8_t kind = 1;
    std::uint32_t sourceId = 0;
    std::uint32_t durationMs = 0;
    std::uint32_t amount = 0;
    double end = 0.0; // local clock (s): now + remaining at arrival
};

class EffectBook
{
public:
    // EFFECT_LIST: replaces the entity's list.
    void list(std::uint32_t entity, std::span<const vo::net::EffectEntry> effects, double now);
    // EFFECT_APPLY: new, or a refresh of the same (skill, kind).
    void apply(std::uint32_t entity, const vo::net::EffectEntry& e, double now);
    // EFFECT_REMOVE: false if it was not listed.
    bool remove(std::uint32_t entity, std::uint16_t skillId, std::uint8_t kind);
    // Death (no EFFECT_REMOVE comes), despawn, a fresh spawn.
    void clear(std::uint32_t entity);
    void clearAll() noexcept { m_entries.clear(); }
    // Safety net: drops effects `grace` seconds past their end (the server's EFFECT_REMOVE normally
    // comes first).
    void expire(double now, double grace = 2.0);

    [[nodiscard]] std::span<const ShownEffect> of(std::uint32_t entity) const noexcept;
    [[nodiscard]] bool has(std::uint32_t entity, std::uint8_t kind, std::uint16_t skillId = 0) const noexcept;
    [[nodiscard]] const ShownEffect* find(std::uint32_t entity, std::uint8_t kind, std::uint16_t skillId = 0) const noexcept;
    [[nodiscard]] std::size_t entityCount() const noexcept { return m_entries.size(); }
    [[nodiscard]] std::vector<std::uint32_t> entities() const;

private:
    struct Entry
    {
        std::uint32_t entity = 0;
        std::vector<ShownEffect> effects;
    };
    Entry* entry(std::uint32_t entity, bool create);
    std::vector<Entry> m_entries;
};

// Seconds left (>= 0).
[[nodiscard]] double effectRemaining(const ShownEffect& e, double now) noexcept;
// The small number under an effect icon: whole seconds rounded up ("12"), "" when none left.
[[nodiscard]] std::string effectSecondsText(double remaining);
// Helpful (green frame) or harmful (red frame).
[[nodiscard]] bool effectHelpful(std::uint8_t kind) noexcept;
// The status icon of an effect: ui/status/<name>.png (dot: "burn" for a Mage's skill, "poison" else).
[[nodiscard]] std::string effectStatusIcon(std::uint8_t kind, std::uint16_t skillId);
// The tooltip of an effect icon: skill name, then one line by kind (§7.7): "Saldırı +%15", "Zehir:
// saniyede 12", "Kalkan: 140", "Sersemledi", ... ('\n' between the two).
[[nodiscard]] std::string effectTipText(const ShownEffect& e);
[[nodiscard]] std::string effectKindLine(std::uint8_t kind, std::uint32_t amount, std::uint16_t skillId);

// ---- Using a skill of the bar (§7.1) ----

struct SkillSelection
{
    std::uint32_t id = 0; // the selected entity (0 none; may be the local player itself)
    bool self = false;    // the local player is selected
    bool monster = false;
    bool player = false;  // another player
    bool dead = false;
    float distance = 0.0f; // horizontal, metres
};
enum class SkillUseVerdict : std::uint8_t
{
    Send,       // send SKILL_REQ(skill, targetId)
    WalkFirst,  // walk into range of targetId, then send
    NotLearned, // rank 0
    NoTarget,   // an enemy skill without a selected monster: "Önce bir hedef seç."
    WrongTarget,
    TargetDead,
};
struct SkillUse
{
    SkillUseVerdict verdict = SkillUseVerdict::Send;
    std::uint32_t targetId = 0;
    float range = 0.0f; // of the rank used
};
// enemy -> the selected monster (walking into range); friendly -> the selected living player if one
// is selected, else the player itself (0); self -> 0.
[[nodiscard]] SkillUse decideSkillUse(const game::SkillDef& skill, std::uint8_t rank, const SkillSelection& sel) noexcept;

// How a bar slot is drawn (besides the cooldown sweep).
enum class SlotLook : std::uint8_t
{
    Empty,
    Ready,
    NoMana,   // red tint: mana below the rank's cost
    NoTarget, // darker: an enemy skill without a monster selected, or out of its range
    Dead,     // grey
};
[[nodiscard]] SlotLook slotLook(const game::SkillDef* skill, std::uint8_t rank, std::uint32_t mp, bool dead,
                                const SkillSelection& sel) noexcept;
[[nodiscard]] std::string_view slotLookName(SlotLook l) noexcept; // "empty", "ready", "nomana", "notarget", "dead"

// ---- Texts (§7.3, §7.4) ----

// Tooltip of a skill at the character's rank (0 = not learned): name; "<branch> · Kademe k/3" (or
// "Öğrenilmedi"); "Mana: 12   Bekleme: 6 sn"; "Menzil: 2,5 m" / "Kendine" / "Çevrene"; "Hazırlık: 0,6
// sn" / "Anında"; the description; "Sonraki kademe: ..." (Dim) for ranks 1 and 2.
[[nodiscard]] std::vector<TipLine> skillTipLines(const game::SkillDef& skill, std::uint8_t rank);
// "<name> kademe 2 oldu.", "Bu yetenek için bu yolda önce 4 puan harcamalısın.", ...
[[nodiscard]] std::string learnResultText(vo::net::SkillLearnResult r, const game::SkillDef* skill, std::uint8_t rank);
[[nodiscard]] std::string_view barResultText(vo::net::SkillBarResult r) noexcept; // "" for Ok
// v8 (docs/PROTOCOL_SHOP.md §8.3): the line for a refused ITEM_USE_RES ("Elinde Kızılca Şerbet
// kalmadı.", "Canın zaten dolu.", ...); "" for Ok. `item` may be null (unknown template).
[[nodiscard]] std::string itemUseResultText(vo::net::ItemUseResult r, const game::ItemTemplate* item);
[[nodiscard]] std::string skillPointsText(std::uint16_t unspent);          // "Yetenek puanı: 3"
[[nodiscard]] std::string branchSpentText(std::uint32_t spent);            // "Bu yolda harcanan: 2"
[[nodiscard]] std::string tierGateText(std::uint16_t points);              // "Bu yolda 2 puan gerekli"
[[nodiscard]] std::string rankText(std::uint8_t rank);                     // "2/3", "Öğrenilmedi"
[[nodiscard]] std::string levelUpSkillPointsText(std::uint32_t points);    // "+1 yetenek puanı  ·  Yetenekler: K"
inline constexpr std::string_view kNoTargetText = "Önce bir hedef seç.";
inline constexpr std::string_view kNotLearnedText = "Bu yeteneği henüz öğrenmedin.";
inline constexpr std::string_view kSkillConfirmText = "Harcanan yetenek puanları geri alınamaz. Onaylıyor musun?";
// ASCII file key of a branch plaque: ui/skillbar/branch_<key>.png ("hucum", "kiragi", ...).
[[nodiscard]] std::string_view branchArtKey(std::uint8_t charClass, std::uint8_t branch) noexcept;
// ui/skills/<key>.png
[[nodiscard]] std::string skillIconPath(const game::SkillDef& skill);

// ---- Effect art (docs/SKILL_ART.md §3) ----

enum class ArtMotion : std::uint8_t
{
    Projectile, // from the caster's hand to the target's chest (nose along the flight)
    Ring,       // a ground ring at the centre (scale XZ = radius), scaling in, slow spin
    Strike,     // light_strike at the target's feet: Y 0 -> 1, hold, XZ -> 0
    Fall,       // `count` motes falling into the area around the target, staggered
    Burst,      // an aura / bubble flash at the entity's feet, fading out
    Cloud,      // fog_cloud at the target point for the duration, slow spin
    Wisp,       // a tick wisp rising from the chest
    Trail,      // leap_trail at the landing point, scale Z = distance, fading
};
enum class ArtAt : std::uint8_t
{
    Caster,
    Target,
};
struct ArtSpawn
{
    std::string_view mesh; // effects/<mesh>.vmesh
    ArtMotion motion = ArtMotion::Burst;
    ArtAt at = ArtAt::Caster;
    float scale = 1.0f;    // rings: the radius
    float duration = 1.0f; // seconds
    float opacity = 1.0f;  // < 1: drawn translucent
    int count = 1;         // Fall: motes
    std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
    bool operator==(const ArtSpawn&) const = default;
};
// What a SKILL_USED of `skill` at `rank` shows (may be empty: melee skills are only the swing).
[[nodiscard]] std::vector<ArtSpawn> skillUseArt(const game::SkillDef& skill, std::uint8_t rank);
// Tick wisp of a periodic damage tick of the skill ("" if none).
[[nodiscard]] std::string_view tickArt(std::uint16_t skillId);

enum class AuraAt : std::uint8_t
{
    Feet,
    Head,
};
struct AuraArt
{
    std::string_view mesh;
    AuraAt at = AuraAt::Feet;
    float scale = 1.0f;
    float spin = 0.0f; // rad/s about Y
    float opacity = 1.0f;
    bool operator==(const AuraArt&) const = default;
};
// The mesh an active effect keeps on its entity while it lasts (stun stars, root spikes, shield
// bubbles, a faint buff aura), nullopt for none.
[[nodiscard]] std::optional<AuraArt> effectAura(std::uint16_t skillId, std::uint8_t kind);

// ---- Input ----

// Esc order (§7.2): loot window, Yetenekler, Künye, inventory, target, menu.
[[nodiscard]] EscapeAction escapeActionWithSkills(bool lootOpen, bool skillsOpen, bool kunyeOpen, bool inventoryOpen,
                                                  bool hasTarget, bool menuOpen) noexcept;
// A skill by automation name: its id ("5") or its key ("yarma").
[[nodiscard]] const game::SkillDef* parseSkillName(std::string_view s) noexcept;
// An effect kind by name: the skills.toml keys ("attack_up", "dot", ...) or its number.
[[nodiscard]] std::optional<std::uint8_t> parseEffectKind(std::string_view s) noexcept;

// ---- Automation (the skill `scene` commands of WorldScene) ----

enum class SkillsVerb : std::uint8_t
{
    Window,         // skills open|close
    Learn,          // skilllearn <skill>: as a click on its "+" (the first one asks to confirm)
    BarSet,         // barset <slot 1-8> <skill|none>: as a drop on the bar (one SKILL_BAR_SET_REQ)
    UseSlot,        // useslot <1-8>: as the key
    ExpectRank,     // expectrank <skill> <rank>
    ExpectPoints,   // expectskillpoints <unspent>
    ExpectSlot,     // expectslot <1-8> <skill|empty>
    ExpectSlotLook, // expectslotlook <1-8> ready|nomana|notarget|dead|empty
    ExpectCooldown, // expectcooldown <skill> <minSec> <maxSec>: remaining own cooldown
    ExpectEffect,   // expecteffect self|target <kind> [skill]: the entity has it (model and HUD icon)
    ExpectNoEffect, // expectnoeffect self|target <kind> [skill]
    ExpectEffectTip,// expecteffecttip self|target <kind> <text>: the icon's tooltip contains <text>
    ExpectArt,      // expectart <mesh> <min>: at least <min> live instances of effects/<mesh>
    ExpectMessage,  // expectskillmessage <text>: the last skill message contains <text>
    PullPack,       // pullpack <monster name> <n>: walk near idle monsters of that name until n chase us
    Log,            // skilllog
};

struct SkillsCommand
{
    SkillsVerb verb = SkillsVerb::Log;
    std::string mode;  // open/close; self/target; look name
    std::string text;  // messages, mesh, monster name
    const game::SkillDef* skill = nullptr;
    std::uint8_t kind = 0;
    std::uint32_t a = 0; // slot 1-8 / rank / points / count
    double lo = 0.0;
    double hi = 0.0;
};

// nullopt with `error` empty: not a skill verb; nullopt with a message: a malformed skill command.
[[nodiscard]] std::optional<SkillsCommand> parseSkillsCommand(std::string_view line, std::string& error);

} // namespace vo::client::world
