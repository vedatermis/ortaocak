// Character sheet of the world scene (M11 "Künye", docs/PROTOCOL_STATS.md §7): the character's name,
// race, class, level and experience; the five attributes (base value, item bonus in green, pending
// points in gold) with "−" / "+" buttons; the unspent points; the derived values with a preview
// ("Saldırı Gücü 36 → 41") while points are pending; "Onayla" / "Vazgeç" and a message line.
// Not modal; it sits on the left side of the screen (the inventory window stays on the right).
//
// Two columns: "Nitelikler" (attributes, points, buttons) on the left, "Değerler" (derived values)
// on the right, so the window is wide and low (580 x 388 logical) and fits 1024x640 at 150 %.
//   "+"            one pending point (Shift+click: 5, or the rest); right click on "+": remove one
//   "−"            remove one pending point (shown only while that row has some)
//   Onayla         one STAT_ALLOCATE_REQ with all pending points (the scene asks once per session
//                  whether the player is sure: spending is permanent)
//   Vazgeç         drop the pending points
//   hover a row    what the attribute does for this class
// Everything is disabled while a request is in flight. The window draws world::StatSheet (it owns
// nothing); the scene calls refresh() whenever the sheet, the level or the experience change.
// Widget names (automation): kunye.window, kunye.header (Label: name), kunye.class (Label),
// kunye.xp (Label), kunye.attr.<key> (Label-like row: "Güç 14 (+2) +3"), kunye.plus.<key>,
// kunye.minus.<key>, kunye.unspent (Label), kunye.derived (invisible Label: one row per line, "Can 120 → 135"),
// kunye.confirm, kunye.cancel, kunye.close, kunye.message (Label), kunye.tooltip (invisible Label:
// the hovered attribute's tooltip). <key> = strength, dexterity, vitality, intelligence, spirit.
#pragma once

#include "client/ui/Button.h"
#include "client/ui/Label.h"
#include "client/ui/Panel.h"
#include "client/world/StatsView.h"

#include <array>
#include <functional>
#include <string>

namespace vo::client::scenes {

class KunyeWindow;

// "+" / "−" with a drawn glyph (no font dependency); reports Shift on click, right click separately.
class StatStepButton final : public ui::Button
{
public:
    StatStepButton(bool plus, std::function<void(bool shift)> onClick, std::function<void()> onRightClick);
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    void drawSelf(ui::DrawContext& ctx) override;

private:
    bool m_plus;
    bool m_shift = false;
    std::function<void(bool)> m_onStep;
    std::function<void()> m_onRight;
};

// One attribute row (name and value); a Label so automation can read its text.
class AttributeRowWidget final : public ui::Label
{
public:
    AttributeRowWidget(KunyeWindow& owner, std::size_t index);
    [[nodiscard]] std::size_t index() const noexcept { return m_index; }
    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;

private:
    KunyeWindow& m_owner;
    std::size_t m_index;
};

class KunyeWindow final : public ui::Panel
{
public:
    static constexpr float kWidth = 580.0f;  // logical
    static constexpr float kHeight = 412.0f; // logical (M18: 388 before the two new derived rows)

    struct Hooks
    {
        std::function<void(std::size_t attribute, bool shift)> add;
        std::function<void(std::size_t attribute)> remove;
        std::function<void()> confirm;
        std::function<void()> cancel;
        std::function<void()> close;
    };

    // What the sheet does not carry.
    struct Info
    {
        std::string name;
        std::string classLine; // "Aydor · Savaşçı · Seviye 2"
        std::uint8_t charClass = 1;
        std::uint32_t xp = 0;
        std::uint32_t xpToNext = 0;
        game::EquipBonus equip; // weapon / armour sums for the preview
    };

    KunyeWindow(const world::StatSheet& sheet, Hooks hooks);

    // Rebuilds texts and the derived rows from the sheet.
    void refresh(const Info& info);
    // Short line at the bottom (error: red); fades after a few seconds.
    void setMessage(std::string text, bool error);

    [[nodiscard]] const world::StatSheet& sheet() const noexcept { return m_sheet; }
    [[nodiscard]] std::uint8_t charClass() const noexcept { return m_info.charClass; }
    void requestAdd(std::size_t i, bool shift);
    void requestRemove(std::size_t i);

    void drawSelf(ui::DrawContext& ctx) override;
    void draw(ui::DrawContext& ctx) override;
    void update(double dt) override;
    bool onMouseWheel(const platform::InputEvent& e) override;
    bool onMouseDown(const platform::InputEvent& e) override;

private:
    [[nodiscard]] const AttributeRowWidget* hoveredRow() const;
    void updateEnabled();

    const world::StatSheet& m_sheet;
    Hooks m_hooks;
    Info m_info;
    std::array<AttributeRowWidget*, game::kAttributeCount> m_rows{};
    std::array<StatStepButton*, game::kAttributeCount> m_plus{};
    std::array<StatStepButton*, game::kAttributeCount> m_minus{};
    ui::Label* m_header = nullptr;
    ui::Label* m_class = nullptr;
    ui::Label* m_xp = nullptr;
    ui::Label* m_unspent = nullptr;
    ui::Label* m_message = nullptr;
    ui::Label* m_derivedProbe = nullptr;
    ui::Label* m_tooltipProbe = nullptr;
    ui::Button* m_confirm = nullptr;
    ui::Button* m_cancel = nullptr;
    std::vector<world::DerivedRow> m_derived;
    double m_messageTime = -1.0;
};

} // namespace vo::client::scenes
