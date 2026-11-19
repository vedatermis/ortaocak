// Shop window of the world scene (M14 "Pazar", docs/PROTOCOL_SHOP.md §8.2) and the quantity box.
//
// The shop window (left side, placed clear of the HUD by WorldScene::layoutHud; the inventory opens on
// the right next to it): the merchant's name in the title bar, its title under it ("‹Şerbetçi›"), the
// greeting (quoted, italic, warm dim text), the stock as rows (fix1: a stock taller than the window's
// maximum height scrolls: wheel, a scrollbar on the right - drag the thumb, click the track to page) - icon, name in its
// rarity colour, a second line (what a potion restores, a weapon's attack and level; a class or level
// requirement the character does not meet in red), the price with the Akçe coin (red when the balance is
// too low) -, the sell zone ("Sat": a right click on a bag item or a drag onto the window sells it), a
// message line and the live balance ("Akçe: 1.234"). While a buy / sell is in flight the rows wait
// (dimmed). Not modal.
//   click / right click a row   buy (the scene asks for a quantity / a confirmation first)
//   hover a row                 the item's tooltip
// Widget names (automation): shop.window, shop.title, shop.greeting, shop.item.<templateId>, shop.sellzone,
// shop.message, shop.coins, shop.close, shop.contents (invisible: one "<name> <price>" line per entry).
// M16 (docs/PROTOCOL_UPGRADE.md §7.2): a smith's window has two tabs under the greeting, "Satın al" (the stock
// and the sell zone, as before) and "Yükselt" (UpgradePanel instead of them); names shop.tab.buy,
// shop.tab.upgrade.
//
// QuantityDialog: a modal "Kaç tane?" / "Kaç tane satılsın?" box: the item's icon and name, − / value / +
// (arrow keys, wheel, digits), "Toplam: <n> Akçe" / "Alınacak: <n> Akçe", Al / Sat and Vazgeç; Enter
// confirms, Esc cancels. Names: quantity.value, quantity.up, quantity.down, quantity.total, quantity.ok,
// quantity.cancel.
#pragma once

#include "client/scenes/ItemTooltip.h"
#include "client/scenes/UpgradePanel.h"
#include "client/ui/Panel.h"
#include "client/world/ShopView.h"

#include <array>
#include <functional>
#include <string>

namespace vo::client::world {
class ClientInventory;
}
namespace vo::client::render {
class Texture2D;
}
namespace vo::client::ui {
class Button;
class Label;
class UiContext;
} // namespace vo::client::ui

namespace vo::client::scenes {

class SceneResources;
class ShopWindow;

class ShopRowWidget final : public ui::Widget
{
public:
    explicit ShopRowWidget(ShopWindow& owner) : m_owner(owner) {}
    void setOffer(const world::ShopOffer& offer) { m_offer = offer; }
    [[nodiscard]] const world::ShopOffer& offer() const noexcept { return m_offer; }
    void drawSelf(ui::DrawContext& ctx) override;
    bool onMouseDown(const platform::InputEvent& e) override;

private:
    ShopWindow& m_owner;
    world::ShopOffer m_offer;
};

class ShopWindow final : public ui::Panel
{
public:
    static constexpr float kWidth = 300.0f; // logical

    struct Hooks
    {
        std::function<void(std::uint16_t templateId)> buy;
        std::function<void()> close;
        std::function<bool()> sellDragActive; // a bag item is being dragged (the sell zone lights)
        // M16: the Yükselt section's button, a right click on its well, a tab change.
        std::function<void()> upgradeStart;
        std::function<void()> upgradeClear;
        std::function<void(bool upgradeTab)> tabChanged;
    };

    ShopWindow(const SceneResources& resources, const world::ShopSession& session,
               const world::ClientInventory& inventory, const world::UpgradeSession& upgrade, Hooks hooks);

    // The merchant shown (name, title, greeting) and the stock of the session. upgrades: a smith (the
    // two tabs; the window opens on "Satın al").
    void open(std::string_view name, std::string_view title, std::string_view greeting, bool upgrades = false);
    // M16: the Yükselt tab (only for a smith).
    void setUpgradeTab(bool upgrade);
    [[nodiscard]] bool upgradeTab() const noexcept { return m_upgradeTab; }
    [[nodiscard]] bool hasTabs() const noexcept { return m_hasTabs; }
    [[nodiscard]] UpgradePanel* upgradePanel() const noexcept { return m_upgradePanel; }
    void setCoins(std::uint32_t coins, bool known);
    // Short line above the balance (error: red); fades after a few seconds.
    void setMessage(std::string text, bool error);
    [[nodiscard]] const std::string& message() const noexcept;
    // Logical height for the current stock (at most the maximum height when the stock scrolls).
    [[nodiscard]] float preferredHeight() const noexcept;
    // The tallest the window may be (logical; 0: no limit). A longer stock scrolls (wheel, scrollbar).
    void setMaxHeight(float logical);
    // Scrolls the stock list by `rows` (+: further down the list).
    void scrollBy(int rows);
    void scrollTo(std::size_t first);
    [[nodiscard]] const world::ShopListLayout& listLayout() const noexcept { return m_list; }
    [[nodiscard]] bool greetingItalic() const noexcept;
    // The sell zone (physical pixels): a bag item dropped here is sold.
    [[nodiscard]] Rect sellZone() const;

    [[nodiscard]] const SceneResources& resources() const noexcept { return m_resources; }
    [[nodiscard]] const world::ClientInventory& inventory() const noexcept { return m_inventory; }
    [[nodiscard]] std::uint32_t coins() const noexcept { return m_coins; }
    [[nodiscard]] bool busy() const noexcept { return m_session.busy(); }
    void requestBuy(std::uint16_t templateId);

    void drawSelf(ui::DrawContext& ctx) override;
    void draw(ui::DrawContext& ctx) override;
    void update(double dt) override;
    bool onMouseWheel(const platform::InputEvent& e) override;
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;

private:
    void relayout();
    [[nodiscard]] const ShopRowWidget* hoveredRow() const;
    [[nodiscard]] float fixedHeight() const noexcept; // everything but the list (logical)
    [[nodiscard]] float rowsTop() const noexcept;     // list top, relative to the content rect (logical)
    [[nodiscard]] Rect scrollTrack() const;           // physical; empty when nothing scrolls

    [[nodiscard]] float tabsHeight() const noexcept;  // the tab row (0 without tabs)

    bool m_hasTabs = false;
    bool m_upgradeTab = false;
    ui::Button* m_tabBuy = nullptr;
    ui::Button* m_tabUpgrade = nullptr;
    UpgradePanel* m_upgradePanel = nullptr;
    float m_maxHeight = 0.0f;
    std::size_t m_first = 0;
    world::ShopListLayout m_list;
    bool m_thumbDrag = false;
    float m_thumbGrab = 0.0f; // physical: cursor y - thumb top when the drag began

    const SceneResources& m_resources;
    const world::ShopSession& m_session;
    const world::ClientInventory& m_inventory;
    Hooks m_hooks;
    std::array<ShopRowWidget*, vo::net::kMaxShopEntries> m_rows{};
    std::size_t m_rowCount = 0;
    ui::Label* m_subtitle = nullptr;
    ui::Label* m_greeting = nullptr;
    ui::Label* m_message = nullptr;
    ui::Label* m_coinLabel = nullptr;
    ui::Label* m_contents = nullptr;
    ui::Label* m_sellLabel = nullptr;
    ui::Widget* m_sellZone = nullptr;
    std::uint32_t m_coins = 0;
    bool m_coinsKnown = false;
    double m_messageTime = -1.0;
    float m_greetingH = 34.0f;
};

struct QuantityDesc
{
    std::string title;           // the item's name
    std::string prompt;          // "Kaç tane?" / "Kaç tane satılsın?"
    const render::Texture2D* icon = nullptr;
    Color iconTint = colors::kWhite; // M15: the item's tint
    std::uint16_t max = 1;
    std::uint16_t initial = 1;
    std::uint32_t unitPrice = 0; // per unit (buy: price, sell: sell price)
    bool sell = false;           // "Alınacak" / "Sat" instead of "Toplam" / "Al"
    std::function<void(std::uint16_t quantity)> onConfirm;
    std::function<void()> onCancel;
};

class QuantityDialog final : public ui::Widget
{
public:
    explicit QuantityDialog(QuantityDesc desc);
    static QuantityDialog& show(ui::UiContext& ui, QuantityDesc desc);

    void close(bool confirmed);
    void setValue(int v);
    [[nodiscard]] std::uint16_t value() const noexcept { return m_model.value; }
    [[nodiscard]] bool closed() const noexcept { return m_closed; }

    void drawSelf(ui::DrawContext& ctx) override;
    void draw(ui::DrawContext& ctx) override;
    bool onKeyDown(const platform::InputEvent& e) override;
    bool onChar(const platform::InputEvent& e) override;
    bool onMouseDown(const platform::InputEvent&) override { return true; }
    bool onMouseUp(const platform::InputEvent&) override { return true; }
    bool onMouseWheel(const platform::InputEvent& e) override;

private:
    void refresh();

    QuantityDesc m_desc;
    world::QuantityModel m_model;
    ui::Panel* m_panel = nullptr;
    ui::Label* m_value = nullptr;
    ui::Label* m_total = nullptr;
    bool m_typed = false; // digits replace the value on the first key, then append
    bool m_closed = false;
};

} // namespace vo::client::scenes
