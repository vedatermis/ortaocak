// Screen-specific widgets of the login/character scenes, built on client/ui:
//   Spinner            animated ring of dots (connecting / waiting indicator)
//   ChevronButton      small button with a drawn ◀ / ▶ chevron (appearance pickers)
//   ToggleButton       button with a persistent "selected" look (class choice)
//   SelectableCard     focusable, clickable card with hover/selected/focus visuals; click selects,
//                      double click / Enter activates
//   CharacterSlotCard  one character select slot: a character summary or "Boş Slot — Karakter Yarat"
//   RaceCard           race choice card: portrait/placeholder, name, tagline, lore text
//   NameHints          live character-name rule hints (✓ ok / • not yet)
//   CenteredRow        lays its children out side by side, centred, each at most maxWidth x maxHeight
#pragma once

#include "client/scenes/CharacterPreview.h"
#include "client/scenes/Validation.h"
#include "client/ui/Button.h"
#include "client/ui/TextLayout.h"
#include "client/ui/Widget.h"
#include "shared/net/Messages.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vo::client::scenes {

class SceneResources;

class Spinner : public ui::Widget
{
public:
    Spinner();
    void drawSelf(ui::DrawContext& ctx) override;
};

class ChevronButton : public ui::Button
{
public:
    enum class Direction : std::uint8_t
    {
        Left,
        Right,
    };
    ChevronButton(Direction direction, std::function<void()> onClick);
    void drawSelf(ui::DrawContext& ctx) override;

private:
    Direction m_direction;
};

class ToggleButton : public ui::Button
{
public:
    ToggleButton(std::string text, std::function<void()> onClick);
    void setSelected(bool selected) noexcept { m_selected = selected; }
    [[nodiscard]] bool isSelected() const noexcept { return m_selected; }
    void drawSelf(ui::DrawContext& ctx) override;

private:
    bool m_selected = false;
};

class SelectableCard : public ui::Widget
{
public:
    SelectableCard();

    void setSelected(bool selected) noexcept { m_selected = selected; }
    [[nodiscard]] bool isSelected() const noexcept { return m_selected; }
    void setOnSelect(std::function<void()> cb) { m_onSelect = std::move(cb); }
    void setOnActivate(std::function<void()> cb) { m_onActivate = std::move(cb); }

    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    bool onKeyDown(const platform::InputEvent& e) override;
    void onCaptureLost() override { m_pressed = false; }
    void drawSelf(ui::DrawContext& ctx) override;

protected:
    // Card frame (fill, border, focus ring); subclasses draw their content after it.
    void drawFrame(ui::DrawContext& ctx);

private:
    std::function<void()> m_onSelect;
    std::function<void()> m_onActivate;
    bool m_selected = false;
    bool m_pressed = false;
    bool m_activateOnRelease = false;
};

class CharacterSlotCard : public SelectableCard
{
public:
    CharacterSlotCard();
    // nullptr = empty slot.
    void setCharacter(const vo::net::CharSummary* character);
    // Item icons of the CharSummary weapons are drawn when set (M8).
    void setResources(const SceneResources* resources) noexcept { m_resources = resources; }
    [[nodiscard]] bool isEmptySlot() const noexcept { return !m_character.has_value(); }
    // What the card shows as text (automation: expecttext).
    [[nodiscard]] std::string displayText() const;
    void drawSelf(ui::DrawContext& ctx) override;

private:
    std::optional<vo::net::CharSummary> m_character;
    const SceneResources* m_resources = nullptr;
};

class RaceCard : public SelectableCard
{
public:
    RaceCard(const SceneResources& resources, std::uint8_t race, std::string tagline, std::string lore);
    void drawSelf(ui::DrawContext& ctx) override;
    [[nodiscard]] std::uint8_t race() const noexcept { return m_race; }

private:
    const SceneResources& m_resources;
    std::uint8_t m_race;
    std::string m_tagline;
    std::string m_lore;
    // Wrapped-lore cache.
    std::vector<ui::TextLine> m_lines;
    float m_linesWidth = -1.0f;
    int m_linesFontPx = 0;
};

class NameHints : public ui::Widget
{
public:
    NameHints();
    void setCheck(const CharacterNameCheck& check);
    void drawSelf(ui::DrawContext& ctx) override;

private:
    CharacterNameCheck m_check;
};

class CenteredRow : public ui::Widget
{
public:
    CenteredRow(float maxWidth, float maxHeight, float spacing);
    void layoutChildren(float scale) override;

private:
    float m_maxWidth;
    float m_maxHeight;
    float m_spacing;
};

} // namespace vo::client::scenes
