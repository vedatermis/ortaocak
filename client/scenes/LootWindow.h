// Loot window of the world scene (M10 "Ganimet", docs/PROTOCOL_LOOT.md §9): the contents of one
// open loot container (world::LootWindowModel) as rows - the Akçe row (coin icon, amount) first, then
// every entry with its icon, name in its rarity colour and "×N" - plus "Hepsini Al" and a close
// button. Not modal; it sits on the left side of the screen (the scene places it below the HUD
// panels), so the character in the middle and the inventory window on the right stay visible.
//   click on a row   take it (LOOT_TAKE_REQ with its index, 0xFE for the Akçe row)
//   Hepsini Al / E   take everything (0xFF)
//   hover            item tooltip (client/scenes/ItemTooltip.h)
// Widget names (automation): loot.window, loot.coins, loot.item.<k> (the k-th item row, 0-based), loot.takeall, loot.close,
// loot.message (Label), loot.contents (invisible Label: one row per line, "Akçe 7" / "Kül Tozu x2").
#pragma once

#include "client/scenes/ItemTooltip.h"
#include "client/ui/Panel.h"
#include "client/world/LootView.h"

#include <array>
#include <functional>
#include <string>

namespace vo::client::world {
class ClientInventory;
}
namespace vo::client::ui {
class Button;
class Label;
} // namespace vo::client::ui

namespace vo::client::scenes {

class SceneResources;
class LootWindow;

// One row of the loot window.
class LootRowWidget final : public ui::Widget
{
public:
    explicit LootRowWidget(LootWindow& owner) : m_owner(owner) {}
    void setRow(const world::LootRow& row) { m_row = row; }
    [[nodiscard]] const world::LootRow& row() const noexcept { return m_row; }
    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;

private:
    LootWindow& m_owner;
    world::LootRow m_row;
};

class LootWindow final : public ui::Panel
{
public:
    static constexpr float kWidth = 264.0f; // logical

    struct Hooks
    {
        std::function<void(std::uint8_t what)> take;
        std::function<void()> takeAll;
        std::function<void()> close;
    };

    LootWindow(const SceneResources& resources, const world::LootWindowModel& model,
               const world::ClientInventory& inventory, Hooks hooks);

    // Rebuilds the rows from the model; `source` is the monster the bag came from (subtitle).
    void refresh(std::string_view source, world::LootLabel label);
    // Short line above the button (error: red); fades after a few seconds.
    void setMessage(std::string text, bool error);
    // Logical height for the current rows.
    [[nodiscard]] float preferredHeight() const noexcept;

    [[nodiscard]] const SceneResources& resources() const noexcept { return m_resources; }
    [[nodiscard]] const world::ClientInventory& inventory() const noexcept { return m_inventory; }
    void requestTake(std::uint8_t what);

    void drawSelf(ui::DrawContext& ctx) override;
    void draw(ui::DrawContext& ctx) override;
    void update(double dt) override;
    bool onMouseWheel(const platform::InputEvent& e) override;
    bool onMouseDown(const platform::InputEvent& e) override;

private:
    [[nodiscard]] const LootRowWidget* hoveredRow() const;

    const SceneResources& m_resources;
    const world::LootWindowModel& m_model;
    const world::ClientInventory& m_inventory;
    Hooks m_hooks;
    std::array<LootRowWidget*, vo::net::kMaxLootEntries + 1> m_rows{};
    std::size_t m_rowCount = 0;
    ui::Label* m_source = nullptr;
    ui::Label* m_empty = nullptr;
    ui::Label* m_message = nullptr;
    ui::Label* m_contents = nullptr;
    ui::Button* m_takeAll = nullptr;
    double m_messageTime = -1.0;
    world::LootLabel m_label = world::LootLabel::Free;
};

} // namespace vo::client::scenes
