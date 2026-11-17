// Skill bar widgets of the world scene (M12 "Yetenek Yolları", docs/PROTOCOL_SKILLS.md §7.1, §7.7;
// art docs/SKILL_ART.md §2): the 8 slots (keys 1-8) at the bottom centre of the HUD (and the same
// slots again inside the "Yetenekler" window) and the rows of effect icons under the player's panel and
// the target frame. The framed HUD bar around the slots and its "Yetenekler (K)" button: HudBar.h.
//
// The widgets own nothing: they draw world::SkillBook / world::EffectBook through SkillUiHost (the
// world scene) and report clicks and drags to it. A slot shows the skill's icon in the slot well, the
// cooldown sweep with a local countdown (the global 500 ms sweep on every filled slot), a red tint
// without mana, a dark tint while an enemy skill has no monster selected or it is out of range, the
// key badge and a short gold flash when a cooldown ends.
//   left click (HUD bar)   use the slot (as its key)
//   drag                   a slot onto another (swap) or off the bars (clear); one SKILL_BAR_SET_REQ
//   right click            clear the slot
// Widget names: skillbar (the HUD bar's slots), skillbar.slot.<1-8>, skills.bar.<1-8> (window),
// effects.self / effects.target (rows), effects.self.<i> / effects.target.<i> (icons).
#pragma once

#include "client/ui/Button.h"
#include "client/ui/Label.h"
#include "client/ui/Widget.h"
#include "client/world/CombatView.h"
#include "client/world/SkillsView.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace vo::client::render {
class Texture2D;
}

namespace vo::client::scenes {

// What the skill widgets need from the world scene.
class SkillUiHost
{
public:
    virtual ~SkillUiHost() = default;

    struct SlotView
    {
        const game::SkillDef* skill = nullptr;
        std::uint8_t rank = 0;
        world::SlotLook look = world::SlotLook::Empty;
        float sweep = 0.0f;     // 0 ready .. 1 just started (own or global cooldown)
        double remaining = 0.0; // seconds of the binding cooldown
        float readyFlash = 0.0f; // 1 -> 0 over the 0.25 s after its own cooldown ended
        bool active = false;    // being cast / queued (walking into range)
    };
    [[nodiscard]] virtual const world::SkillBook& skillBook() const = 0;
    [[nodiscard]] virtual SlotView slotView(std::size_t slot) const = 0;
    [[nodiscard]] virtual const render::Texture2D* skillImage(std::string_view path) const = 0;
    [[nodiscard]] virtual std::span<const world::ShownEffect> shownEffects(bool target) const = 0;
    [[nodiscard]] virtual double skillClock() const = 0;

    virtual void useBarSlot(std::size_t slot) = 0;
    virtual void clearBarSlot(std::size_t slot) = 0;
    // Drag & drop of a skill: from a bar slot (fromSlot 0..7) or from the window (fromSlot -1).
    virtual void beginSkillDrag(std::uint16_t skillId, int fromSlot) = 0;
    virtual void moveSkillDrag(float x, float y) = 0;
    virtual void endSkillDrag(float x, float y, bool drop) = 0;
    [[nodiscard]] virtual bool skillDragActive() const = 0;
    [[nodiscard]] virtual std::uint16_t skillDragSkill() const = 0;
    virtual void raiseSkill(std::uint16_t skillId) = 0;
    virtual void closeSkillWindow() = 0;
};

// Draws one skill in a slot rect (well, icon, tint, sweep, frame, key badge, flash).
void drawSkillSlot(ui::DrawContext& ctx, const SkillUiHost& host, const Rect& r, const SkillUiHost::SlotView& v,
                   int keyNumber, bool hovered, bool pressed, bool dropTarget);

// One slot of the bar.
class SkillSlotWidget final : public ui::Widget
{
public:
    SkillSlotWidget(SkillUiHost& host, std::size_t index, bool clickUses);
    [[nodiscard]] std::size_t index() const noexcept { return m_index; }

    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    void onCaptureLost() override;

private:
    SkillUiHost& m_host;
    std::size_t m_index;
    bool m_clickUses;
    bool m_pressed = false;
    bool m_dragged = false;
    float m_pressX = 0.0f;
    float m_pressY = 0.0f;
};

// The 8 slots in one row (the HUD bar, or the row inside the window).
class SkillSlotsWidget final : public ui::Widget
{
public:
    // `prefix`: widget names <prefix>.<1-8>; slot size and gap in logical units.
    SkillSlotsWidget(SkillUiHost& host, std::string_view prefix, float slot, float gap, bool clickUses);
    [[nodiscard]] static float widthFor(float slot, float gap) noexcept { return 8.0f * slot + 7.0f * gap; }
    // The slot under a screen point (-1 if none).
    [[nodiscard]] int slotAt(float x, float y) const;
    [[nodiscard]] const SkillSlotWidget* slot(std::size_t i) const { return i < m_slots.size() ? m_slots[i] : nullptr; }
    // New slot size and gap (logical): the HUD bar shrinks on narrow screens.
    void setMetrics(float slot, float gap);

private:
    std::array<SkillSlotWidget*, 8> m_slots{};
};

// One effect icon (the skill's icon, the effect's status icon in the corner, a green / red frame,
// the seconds left; a shield also its absorb left).
class EffectIconWidget final : public ui::Widget
{
public:
    EffectIconWidget(SkillUiHost& host, bool target, std::size_t index);
    void drawSelf(ui::DrawContext& ctx) override;
    // The effect it shows now (nullptr when none).
    [[nodiscard]] const world::ShownEffect* effect() const;

private:
    SkillUiHost& m_host;
    bool m_target;
    std::size_t m_index;
};

// A row of up to 16 effect icons (wrapping after `perRow`); the icons are its children.
class EffectRowWidget final : public ui::Widget
{
public:
    static constexpr float kIcon = 26.0f;    // logical
    static constexpr float kGap = 4.0f;
    static constexpr float kRowH = kIcon + 14.0f; // icon + seconds
    EffectRowWidget(SkillUiHost& host, bool target, std::size_t perRow);
    void update(double dt) override;

private:
    SkillUiHost& m_host;
    bool m_target;
    std::size_t m_perRow;
    std::array<EffectIconWidget*, 16> m_icons{};
};

} // namespace vo::client::scenes
