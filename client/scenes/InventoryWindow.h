// Inventory window of the world scene (M8 "Kuşan", docs/PROTOCOL_ITEMS.md §6): equipment slots
// around a small silhouette on the left (Head, Chest, Legs | Hands, Feet, reserved; MainHand and
// OffHand under the figure; the armour slots take nothing in M8 and are drawn dimmed), the 7 x 4 bag
// on the right, a message line at the bottom.
//
// Not modal: the world keeps running and the movement keys keep working while it is open (it has no
// text box). It shows ClientInventory::view() (moves appear at once, a refused one snaps back).
//   drag & drop          left button on an item, move > 4 px, release over a slot; while dragging,
//                        slots the item may legally go to are lit, the others darkened
//   double / right click equip a bag item into its natural slot, unequip to the first free bag slot
//   hover                tooltip: name, kind · slot, attack / defense, classes, level (unmet in red),
//                        description; kept inside the screen
// Widget names (automation): inv.window, inv.equip.<main|off|head|chest|legs|hands|feet|reserved>,
// inv.bag.<0..27>, inv.close, inv.message (Label), inv.tooltip (invisible Label carrying the text
// of the tooltip currently shown, "" when none).
#pragma once

#include "client/ui/Label.h"
#include "client/ui/Panel.h"
#include "shared/game/Items.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace vo::client::world {
class ClientInventory;
}

namespace vo::client::scenes {

class SceneResources;
class InventoryWindow;

// One inventory slot.
class ItemSlotWidget final : public ui::Widget
{
public:
    ItemSlotWidget(InventoryWindow& owner, game::ItemPlace place);
    [[nodiscard]] game::ItemPlace place() const noexcept { return m_place; }

    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;
    void onCaptureLost() override;

private:
    InventoryWindow& m_owner;
    game::ItemPlace m_place;
    bool m_pressed = false;
    bool m_quickDone = false; // the press was a double / right click: the release does nothing
    float m_pressX = 0.0f;
    float m_pressY = 0.0f;
};

class InventoryWindow final : public ui::Panel
{
public:
    // move(from, to): ask for a move (the scene sends it). quickMove(from) is done here through the
    // inventory's quick target; close(): the close button.
    struct Hooks
    {
        std::function<void(game::ItemPlace from, game::ItemPlace to)> move;
        std::function<void()> close;
    };

    InventoryWindow(const SceneResources& resources, const world::ClientInventory& inventory, Hooks hooks);

    // Short line at the bottom (error: red); fades after a few seconds.
    void setMessage(std::string text, bool error);
    [[nodiscard]] const world::ClientInventory& inventory() const noexcept { return m_inventory; }
    [[nodiscard]] const SceneResources& resources() const noexcept { return m_resources; }

    // ---- Used by the slots ----
    void beginDrag(game::ItemPlace from);
    void dragTo(float x, float y);
    void endDrag(float x, float y, bool drop);
    void quickMove(game::ItemPlace from);
    [[nodiscard]] bool dragging() const noexcept { return m_dragging; }
    [[nodiscard]] float layoutScaleOf() const noexcept;
    [[nodiscard]] game::ItemPlace dragFrom() const noexcept { return m_dragFrom; }
    // Shown item of a place (empty stack if none).
    [[nodiscard]] const game::ItemStack& itemAt(game::ItemPlace p) const;
    // Tooltip text for a place ("" for an empty bag slot); lines separated by '\n'.
    [[nodiscard]] std::string tooltipText(game::ItemPlace p) const;

    void drawSelf(ui::DrawContext& ctx) override;
    void draw(ui::DrawContext& ctx) override;
    void update(double dt) override;
    bool onMouseWheel(const platform::InputEvent& e) override;
    void onMouseLeave() override;

private:
    struct TipLine
    {
        std::string text;
        Color color;
        bool title = false;
        bool wrap = false;
    };
    void buildTip(game::ItemPlace p, std::vector<TipLine>& out, const ui::Theme& t) const;
    void drawTooltip(ui::DrawContext& ctx);
    void drawSilhouette(ui::DrawContext& ctx) const;
    [[nodiscard]] ItemSlotWidget* slotAt(float x, float y) const;
    [[nodiscard]] ItemSlotWidget* hoveredSlot() const;

    const SceneResources& m_resources;
    const world::ClientInventory& m_inventory;
    Hooks m_hooks;
    std::vector<ItemSlotWidget*> m_slots;
    ui::Label* m_message = nullptr;
    ui::Label* m_tooltipProbe = nullptr;
    ui::Label* m_bagCount = nullptr;
    double m_messageTime = -1.0;
    bool m_dragging = false;
    game::ItemPlace m_dragFrom;
    float m_dragX = 0.0f;
    float m_dragY = 0.0f;
    Rect m_figure; // logical offset of the silhouette inside the content rect
};

// Turkish name of an equipment slot ("Sağ El", "Baş", ...).
[[nodiscard]] std::string_view equipSlotDisplayName(game::EquipSlot s) noexcept;

} // namespace vo::client::scenes
