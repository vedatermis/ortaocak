// The world scene's HUD pieces (docs/CLIENT_RENDERING.md §15; layout and texts in
// client/world/HudView.h):
//   HudBarFrame     the framed action bar at the bottom centre: the frame, the group separators and
//                   the experience bar; its children are the four action buttons, the 8 skill slots
//                   (SkillSlotsWidget, SkillBar.h) and the five menu buttons
//   HudIconButton   an icon button with its key in a corner badge: lit while its toggle / window is
//                   on, a soft pulse when it wants attention, a gold marker with unspent points
//   HudFrameWidget  a panel drawn by a callback (the player frame: medallion, name, bars)
//   ControlsWindow  the "Tuşlar" window: every control, grouped, as key caps and a line each
// Widget names: hud.bar, hud.attack, hud.sit, hud.loot, hud.run, world.inventory, world.kunye,
// world.skills, hud.controls, hud.menu, controls.window, controls.close, controls.text (probe).
#pragma once

#include "client/scenes/SkillBar.h"
#include "client/ui/Panel.h"
#include "client/ui/Widget.h"
#include "client/world/HudView.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace vo::client::scenes {

class HudIconButton final : public ui::Widget
{
public:
    struct Hooks
    {
        std::function<void()> click;
        std::function<world::HudButtonState()> state; // icon, lit, points
        std::function<bool()> active;                 // lit (toggle on / window open)
        std::function<bool()> attention;              // a soft pulse (loot in reach)
        std::function<bool()> dim;                    // cannot be used now (drawn darker; still clickable)
        std::function<std::uint16_t()> points;        // gold marker with this number (0: none)
    };
    HudIconButton(world::HudButton kind, const SkillUiHost& art, Hooks hooks);
    [[nodiscard]] world::HudButton kind() const noexcept { return m_kind; }
    [[nodiscard]] bool markerShown() const;
    [[nodiscard]] bool lit() const { return m_hooks.active && m_hooks.active(); }

    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    void onCaptureLost() override { m_pressed = false; }

private:
    world::HudButton m_kind;
    const SkillUiHost& m_art;
    Hooks m_hooks;
    bool m_pressed = false;
};

// Draws a key badge ("F", "Esc") with its top-left corner near `corner` (the plate art, nine-sliced
// for longer keys). Shared by the HUD buttons and the "Tuşlar" window.
void drawKeyBadge(ui::DrawContext& ctx, const SkillUiHost& art, Vec2 corner, float height, std::string_view key);

class HudBarFrame final : public ui::Widget
{
public:
    struct Xp
    {
        std::uint16_t level = 1;
        std::uint32_t xp = 0;
        std::uint32_t toNext = 0;
    };
    explicit HudBarFrame(std::function<Xp()> xp);
    // The layout this frame and its children were placed with (logical units).
    void setLayout(const world::HudBarLayout& layout) { m_layout = layout; }
    [[nodiscard]] const world::HudBarLayout& layout() const noexcept { return m_layout; }
    // The experience bar on screen (physical pixels).
    [[nodiscard]] Rect xpRect() const;

    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent&) override { return true; } // the frame is not the ground
    bool onMouseUp(const platform::InputEvent&) override { return true; }

private:
    std::function<Xp()> m_xp;
    world::HudBarLayout m_layout;
};

// A panel whose look is drawn by a callback (the player frame); children (labels) draw on top.
class HudFrameWidget final : public ui::Widget
{
public:
    explicit HudFrameWidget(std::function<void(ui::DrawContext&, const Rect&)> draw) : m_draw(std::move(draw)) {}
    void drawSelf(ui::DrawContext& ctx) override
    {
        if (m_draw)
            m_draw(ctx, rect());
    }

private:
    std::function<void(ui::DrawContext&, const Rect&)> m_draw;
};

class ControlsWindow final : public ui::Panel
{
public:
    ControlsWindow(const SkillUiHost& art, std::function<void()> onClose);
    // Sizes the window for a screen of `maxW` x `maxH` logical units (content shrunk to fit if needed).
    // Returns the window size (logical).
    Vec2 fit(float maxW, float maxH);
    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent&) override { return true; }

private:
    struct Column
    {
        float keyW = 0.0f;
        float textW = 0.0f;
        float h = 0.0f;
    };
    void measure();
    const SkillUiHost& m_art;
    std::array<Column, 2> m_cols{};
    float m_measuredScale = -1.0f;
    float m_k = 1.0f;
};

} // namespace vo::client::scenes
