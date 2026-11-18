// Skill window of the world scene ("Yetenekler", M12, docs/PROTOCOL_SKILLS.md §7.2): the character's
// two branches side by side (branch plaque, three tier rows, tier 1 at the top, connectors between the
// tiers lit once a tier is open), "Yetenek puanı: N" (gold while N > 0), "Bu yolda harcanan: N" under
// each branch, and the 8 bar slots again at the bottom (so a skill can be dragged onto the bar even
// where the window covers the HUD bar: 1024x640 at 150 %).
//   cell          icon (dimmed at rank 0, grey while its tier is locked), rank pips, name, "k/3" or
//                 "Öğrenilmedi"; drag a learned skill's cell onto a bar slot to place it there
//   "+"           shown while the skill can be raised (points left, rank < 3, tier gate met); one
//                 SKILL_LEARN_REQ (the scene asks once per session first); disabled while one is in flight
//   locked row    a band "Bu yolda N puan gerekli" over the greyed cells of a tier whose gate is not met
//   hover         the skill's tooltip (drawn by the scene)
// Not modal; it sits on the left (like the Künye; drawn above it). The window draws world::SkillBook
// through SkillUiHost every frame (it owns nothing).
// Widget names: skills.window, skills.points (Label), skills.class (Label), skills.branch.<0|1> (Label),
// skills.cell.<key>, skills.plus.<key>, skills.rank.<key> (invisible Label: "2/3" / "Öğrenilmedi"),
// skills.gate.<branch>.<tier> (Label, visible while locked), skills.spent.<0|1> (Label), skills.bar.<1-8>,
// skills.message (Label), skills.close.
#pragma once

#include "client/scenes/SkillBar.h"
#include "client/ui/Label.h"
#include "client/ui/Panel.h"

#include <string>
#include <vector>

namespace vo::client::scenes {

class StatStepButton;

// One skill of the tree.
class SkillCellWidget final : public ui::Widget
{
public:
    SkillCellWidget(SkillUiHost& host, const game::SkillDef& skill);
    [[nodiscard]] const game::SkillDef& skill() const noexcept { return m_skill; }

    // fix1: the cell's name is one line, shrunk (from the Small size, in 0.5 steps, down to kMinNameFont)
    // until it fits `widthPx`; returns the logical font size.
    static constexpr float kMinNameFont = 10.0f;
    [[nodiscard]] static float skillNameFontSize(render::FontLibrary& fonts, const ui::Theme& theme, float scale,
                                                 std::string_view name, float widthPx);
    // Every skill of every class fits its cell at `scale` (name on one line, rank line beside the "+").
    [[nodiscard]] static bool checkAllNamesFit(render::FontLibrary& fonts, const ui::Theme& theme, float scale,
                                               std::string& error);

    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    void onCaptureLost() override;

private:
    SkillUiHost& m_host;
    const game::SkillDef& m_skill;
    bool m_pressed = false;
    bool m_dragged = false;
    float m_pressX = 0.0f;
    float m_pressY = 0.0f;
};

class SkillWindow final : public ui::Panel
{
public:
    static constexpr float kWidth = 656.0f;  // logical
    static constexpr float kHeight = 424.0f; // logical (fits 1024x640 at 150 %: 427 logical)

    SkillWindow(SkillUiHost& host, std::uint8_t charClass);

    // "Savaşçı · Seviye 10" (right of the points).
    void setClassLine(std::string text);
    // Short line at the bottom (error: red); fades after a few seconds.
    void setMessage(std::string text, bool error);
    [[nodiscard]] const SkillSlotsWidget* barSlots() const noexcept { return m_bar; }

    void drawSelf(ui::DrawContext& ctx) override;
    void update(double dt) override;
    bool onMouseWheel(const platform::InputEvent& e) override;
    bool onMouseDown(const platform::InputEvent& e) override;

private:
    struct Cell
    {
        SkillCellWidget* widget = nullptr;
        StatStepButton* plus = nullptr;
        ui::Label* rank = nullptr;
        const game::SkillDef* skill = nullptr;
        Rect logical; // relative to the content rect
    };
    struct Gate
    {
        ui::Label* label = nullptr;
        std::uint8_t branch = 0;
        std::uint8_t tier = 2;
    };

    SkillUiHost& m_host;
    std::uint8_t m_class;
    std::vector<Cell> m_cells;
    std::vector<Gate> m_gates;
    ui::Label* m_points = nullptr;
    ui::Label* m_classLine = nullptr;
    std::array<ui::Label*, 2> m_spent{};
    SkillSlotsWidget* m_bar = nullptr;
    ui::Label* m_message = nullptr;
    double m_messageTime = -1.0;
};

} // namespace vo::client::scenes
