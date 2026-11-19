// The "Yükselt" section of a smith's shop window (M16 "Tav", docs/PROTOCOL_UPGRADE.md §7.2), shown instead of
// the stock list when its tab is chosen. Same visual language as the shop rows:
//   the item well (left; a bag item dropped on the window or right-clicked in the bag goes in; a right click on
//   the well empties it), the item's name in its colour with "(+N)", "+4 → +5", what the level changes
//   ("Saldırı 18 → 19"); the needed stone with its icon and how many the bag holds ("Elinde: 3", red at 0),
//   the fee with the Akçe coin (red when the balance is short), the success chance, what a failure does (red
//   when the item would be destroyed); the "Yükselt" button (disabled with the reason when a local check
//   fails); the result line of the last attempt with a short effect over the well (success: a gold ring and
//   rays; failure: the well shakes, grey; destroyed: a red flash and the icon breaking apart).
// Widget names (automation): upgrade.panel, upgrade.slot, upgrade.button, upgrade.result, upgrade.contents
// (invisible: every line the section shows, one per line).
#pragma once

#include "client/ui/Widget.h"
#include "client/world/UpgradeView.h"

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

class UpgradePanel final : public ui::Widget
{
public:
    static constexpr float kHeight = 262.0f; // logical

    struct Hooks
    {
        std::function<void()> start;    // the button
        std::function<void()> clear;    // a right click on the well
    };

    UpgradePanel(const SceneResources& resources, const world::ClientInventory& inventory,
                 const world::UpgradeSession& session, Hooks hooks);

    void setCoins(std::uint32_t coins) noexcept { m_coins = coins; }
    // Recomputes the view (every frame while shown).
    void refresh();
    [[nodiscard]] const world::UpgradePanelView& view() const noexcept { return m_view; }
    // Every line the section shows (also in upgrade.contents).
    [[nodiscard]] const std::string& contents() const noexcept;
    // The item well (physical pixels).
    [[nodiscard]] Rect slotRect() const;

    void update(double dt) override;
    void drawSelf(ui::DrawContext& ctx) override;
    void draw(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;

private:
    void drawEffect(ui::DrawContext& ctx, const Rect& well, double age) const;

    const SceneResources& m_resources;
    const world::ClientInventory& m_inventory;
    const world::UpgradeSession& m_session;
    Hooks m_hooks;
    std::uint32_t m_coins = 0;
    world::UpgradePanelView m_view;
    std::uint16_t m_lastIcon = 0; // the template drawn in the well (kept for the breaking effect)
    ui::Label* m_name = nullptr;
    ui::Label* m_level = nullptr;
    ui::Label* m_stat = nullptr;
    ui::Label* m_hint = nullptr;
    ui::Label* m_stoneKey = nullptr;
    ui::Label* m_stone = nullptr;
    ui::Label* m_owned = nullptr;
    ui::Label* m_feeKey = nullptr;
    ui::Label* m_fee = nullptr;
    ui::Label* m_chanceKey = nullptr;
    ui::Label* m_chance = nullptr;
    ui::Label* m_fail = nullptr;
    ui::Label* m_result = nullptr;
    ui::Label* m_contents = nullptr;
    ui::Button* m_button = nullptr;
    ui::Widget* m_slot = nullptr;
};

} // namespace vo::client::scenes
