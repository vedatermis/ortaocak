#include "client/scenes/InventoryWindow.h"

#include "client/net/NetText.h"
#include "client/render/TextRenderer.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/SkillBar.h"
#include "client/ui/Button.h"
#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"
#include "client/world/ClientInventory.h"
#include "shared/game/Character.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
using game::EquipSlot;
using game::ItemPlace;

namespace {

// Layout (logical units, relative to the window's content rect).
constexpr float kSlot = 48.0f;
constexpr float kWeaponSlot = 56.0f;
constexpr float kGap = 6.0f;
constexpr float kHeader = 22.0f;
constexpr float kEquipWidth = 220.0f;
constexpr float kRightColumn = kEquipWidth - kSlot;
constexpr float kBagX = kEquipWidth + 28.0f;
constexpr float kBagWidth = 7.0f * kSlot + 6.0f * kGap;
constexpr float kWeaponY = kHeader + 3.0f * (kSlot + kGap) + 4.0f;
constexpr float kFooterY = kWeaponY + kWeaponSlot + 10.0f;
constexpr float kPadX = 16.0f;
constexpr float kPadTop = 10.0f;
constexpr float kPadBottom = 12.0f;
constexpr float kContentHeight = kFooterY + 20.0f;
constexpr float kCoinWidth = 170.0f; // footer: Akçe balance, right-aligned under the bag
constexpr float kDragThreshold = 4.0f;
constexpr double kMessageSeconds = 5.0;
const Color kRed = Color::hex(0xE5736A);

struct EquipCell
{
    EquipSlot slot;
    float x;
    float y;
    float size;
    const char* key;
};

// Head, Chest, Legs on the left; Hands, Feet, (reserved) on the right; the hands under the figure.
constexpr EquipCell kEquipCells[] = {
    {EquipSlot::Head, 0.0f, kHeader, kSlot, "head"},
    {EquipSlot::Chest, 0.0f, kHeader + (kSlot + kGap), kSlot, "chest"},
    {EquipSlot::Legs, 0.0f, kHeader + 2.0f * (kSlot + kGap), kSlot, "legs"},
    {EquipSlot::Hands, kRightColumn, kHeader, kSlot, "hands"},
    {EquipSlot::Feet, kRightColumn, kHeader + (kSlot + kGap), kSlot, "feet"},
    {EquipSlot::Reserved, kRightColumn, kHeader + 2.0f * (kSlot + kGap), kSlot, "reserved"},
    {EquipSlot::MainHand, kEquipWidth * 0.5f - 6.0f - kWeaponSlot, kWeaponY, kWeaponSlot, "main"},
    {EquipSlot::OffHand, kEquipWidth * 0.5f + 6.0f, kWeaponY, kWeaponSlot, "off"},
};

bool usableSlot(ItemPlace p)
{
    return !p.isEquipment() || p.is(EquipSlot::MainHand) || p.is(EquipSlot::OffHand);
}

// A Label that is never drawn: carries text for automation (expecttext).
class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

} // namespace

std::string_view equipSlotDisplayName(EquipSlot s) noexcept
{
    switch (s)
    {
    case EquipSlot::MainHand: return "Sağ El";
    case EquipSlot::OffHand: return "Sol El";
    case EquipSlot::Head: return "Baş";
    case EquipSlot::Chest: return "Gövde";
    case EquipSlot::Legs: return "Bacak";
    case EquipSlot::Hands: return "Eller";
    case EquipSlot::Feet: return "Ayak";
    case EquipSlot::Reserved: return "Takı";
    }
    return "?";
}

// ---- ItemSlotWidget ----

ItemSlotWidget::ItemSlotWidget(InventoryWindow& owner, ItemPlace place) : m_owner(owner), m_place(place) {}

bool ItemSlotWidget::onMouseDown(const platform::InputEvent& e)
{
    m_quickDone = false;
    const bool hasItem = !m_owner.itemAt(m_place).empty();
    if (e.button == platform::MouseButton::Right || (e.button == platform::MouseButton::Left && e.clickCount >= 2))
    {
        if (hasItem && e.button == platform::MouseButton::Right)
            m_owner.rightClick(m_place);
        else if (hasItem)
            m_owner.doubleClick(m_place);
        m_quickDone = true;
        m_pressed = false;
        return true;
    }
    if (e.button != platform::MouseButton::Left)
        return false;
    m_pressed = hasItem;
    m_pressX = static_cast<float>(e.x);
    m_pressY = static_cast<float>(e.y);
    return true;
}

bool ItemSlotWidget::onMouseMove(const platform::InputEvent& e)
{
    if (!m_pressed || !hasCapture())
        return false;
    const auto x = static_cast<float>(e.x);
    const auto y = static_cast<float>(e.y);
    if (!m_owner.dragging() && std::hypot(x - m_pressX, y - m_pressY) > kDragThreshold * m_owner.layoutScaleOf())
        m_owner.beginDrag(m_place);
    if (m_owner.dragging())
        m_owner.dragTo(x, y);
    return true;
}

bool ItemSlotWidget::onMouseUp(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return m_quickDone;
    const bool wasDragging = m_owner.dragging() && m_owner.dragFrom() == m_place;
    m_pressed = false;
    if (wasDragging)
        m_owner.endDrag(static_cast<float>(e.x), static_cast<float>(e.y), true);
    m_quickDone = false;
    return true;
}

void ItemSlotWidget::onCaptureLost()
{
    m_pressed = false;
    if (m_owner.dragging() && m_owner.dragFrom() == m_place)
        m_owner.endDrag(0.0f, 0.0f, false);
}

void ItemSlotWidget::drawSelf(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const float radius = ctx.pxf(4.0f);
    const float border = std::max(1.0f, ctx.pxf(1.0f));
    const bool usable = usableSlot(m_place);
    const bool hovered = isHovered() && !m_owner.dragging();
    const game::ItemStack& item = m_owner.itemAt(m_place);

    // Recessed well; the armour slots of M8 are dimmed (they take nothing yet).
    b.fillRoundedRect(r, radius, usable ? Color::hex(0x0C0F13, 0.92f) : Color::hex(0x0C0F13, 0.55f));
    b.fillRoundedRectGradient(r.inset(border), radius, Color::hex(0x000000, 0.35f), Color::hex(0xFFFFFF, 0.03f));
    Color edge = usable ? t.insetBorder : t.insetBorder.withAlpha(0.45f);
    if (m_place.isEquipment() && usable)
        edge = t.panelBorder;

    // Drop targets while dragging.
    bool dimmed = false;
    if (m_owner.dragging() && !(m_owner.dragFrom() == m_place))
    {
        const bool legal = m_owner.inventory().check(m_owner.dragFrom(), m_place) == game::ItemMoveResult::Ok;
        if (legal)
        {
            b.fillRoundedRect(r.inset(border), radius, t.accent.withAlpha(0.16f));
            edge = t.accentBright;
        }
        else
        {
            dimmed = true;
        }
    }
    else if (hovered && usable)
    {
        edge = t.buttonBorderHover;
    }

    // Empty equipment slot: its name, small and dim.
    if (item.empty() && m_place.isEquipment())
    {
        render::TextStyle s;
        s.color = usable ? t.textDim.withAlpha(0.8f) : t.textDisabled.withAlpha(0.6f);
        render::drawTextAligned(b, ctx.font(ui::TextRole::Small), equipSlotDisplayName(static_cast<EquipSlot>(m_place.slot)),
                                r, render::HAlign::Center, render::VAlign::Middle, s);
    }
    if (!item.empty())
    {
        const bool source = m_owner.dragging() && m_owner.dragFrom() == m_place;
        const render::Texture2D* icon = m_owner.resources().itemIcon(item.templateId);
        const Rect inner = r.inset(ctx.px(3.0f)).snapped();
        if (icon != nullptr)
        {
            b.drawImage(*icon, inner,
                        m_owner.resources().itemTint(item.templateId,
                                                     source ? Color{1.0f, 1.0f, 1.0f, 0.3f} : colors::kWhite));
        }
        else
        {
            render::TextStyle s;
            s.color = t.textDim;
            render::drawTextAligned(b, ctx.font(ui::TextRole::Large, render::FontWeight::Bold), "?", r,
                                    render::HAlign::Center, render::VAlign::Middle, s);
        }
        // M14: the shared potion cooldown sweeps the icon.
        if (const float sweep = m_owner.iconSweep(item.templateId); sweep > 0.0f)
            drawCooldownSweep(b, inner, sweep, Color::hex(0x05070A, 0.62f));
        // M16: the upgrade level in the top-left corner ("+7"; gold, from +7 on in the +7 colour).
        if (item.upgrade > 0)
        {
            render::TextStyle us;
            us.color = Color::hex(item.upgrade >= world::kUpgradeHighLevel ? world::kUpgradeHighRgb : 0xF2CA64,
                                  source ? 0.35f : 1.0f);
            us.outline = true;
            us.outlineColor = Color::hex(0x000000, 0.9f);
            const Rect corner{r.x + ctx.px(4.0f), r.y + ctx.px(2.0f), r.w - ctx.px(6.0f), r.h - ctx.px(4.0f)};
            render::drawTextAligned(b, ctx.font(12.0f, render::FontWeight::Bold), std::format("+{}", item.upgrade), corner,
                                    render::HAlign::Left, render::VAlign::Top, us);
        }
        // M10: the stack size in the bottom-right corner (only stacks hold more than one unit).
        if (item.quantity > 1)
        {
            render::TextStyle cs;
            cs.color = source ? Color::hex(0xFFF3D6, 0.35f) : Color::hex(0xFFF3D6);
            cs.outline = true;
            cs.outlineColor = Color::hex(0x000000, 0.9f);
            const Rect corner{r.x, r.y, r.w - ctx.px(4.0f), r.h - ctx.px(2.0f)};
            render::drawTextAligned(b, ctx.font(13.0f, render::FontWeight::Bold), std::to_string(item.quantity), corner,
                                    render::HAlign::Right, render::VAlign::Bottom, cs);
        }
    }
    if (dimmed)
        b.fillRoundedRect(r, radius, Color::hex(0x000000, 0.45f));
    b.strokeRoundedRect(r, radius, border, edge);
}

// ---- InventoryWindow ----

InventoryWindow::InventoryWindow(const SceneResources& resources, const world::ClientInventory& inventory, Hooks hooks)
    : ui::Panel(ui::Panel::Style::Window, "Envanter"), m_resources(resources), m_inventory(inventory),
      m_hooks(std::move(hooks))
{
    setName("inv.window");
    setPadding({kPadX, kPadTop, kPadX, kPadBottom});
    setSize(kBagX + kBagWidth + 2.0f * kPadX, 38.0f + kPadTop + kContentHeight + kPadBottom);

    auto header = [&](const char* text, float x, float w) {
        auto& l = add<ui::Label>(text, ui::TextRole::Small);
        l.setWeight(render::FontWeight::Bold).setShadow(true);
        l.setAnchors(anchor::kTopLeft).setMargins({x, 0.0f, 0.0f, 0.0f}).setSize(w, 18.0f);
        return &l;
    };
    header("Kuşanılanlar", 0.0f, kEquipWidth);
    header("Çanta", kBagX, 120.0f);
    m_bagCount = header("", kBagX, kBagWidth);
    m_bagCount->setWeight(render::FontWeight::Regular).setAlign(render::HAlign::Right);
    m_bagCount->setName("inv.bagcount");

    for (const EquipCell& c : kEquipCells)
    {
        auto& s = add<ItemSlotWidget>(*this, ItemPlace::equipment(c.slot));
        s.setAnchors(anchor::kTopLeft).setMargins({c.x, c.y, 0.0f, 0.0f}).setSize(c.size, c.size);
        s.setName(std::string("inv.equip.") + c.key);
        m_slots.push_back(&s);
    }
    for (std::uint8_t i = 0; i < game::kBagSlots; ++i)
    {
        const float x = kBagX + static_cast<float>(i % game::kBagColumns) * (kSlot + kGap);
        const float y = kHeader + static_cast<float>(i / game::kBagColumns) * (kSlot + kGap);
        auto& s = add<ItemSlotWidget>(*this, ItemPlace::bag(i));
        s.setAnchors(anchor::kTopLeft).setMargins({x, y, 0.0f, 0.0f}).setSize(kSlot, kSlot);
        s.setName("inv.bag." + std::to_string(i));
        m_slots.push_back(&s);
    }
    m_figure = {kSlot + 12.0f, kHeader, kEquipWidth - 2.0f * (kSlot + 12.0f), 3.0f * kSlot + 2.0f * kGap};

    m_message = &add<ui::Label>("", ui::TextRole::Small);
    m_message->setShadow(true);
    m_message->setAnchors(anchor::kTopLeft)
        .setMargins({0.0f, kFooterY, 0.0f, 0.0f})
        .setSize(kBagX + kBagWidth - kCoinWidth - 8.0f, 20.0f);
    m_message->setName("inv.message");

    // M10: the Akçe balance, bottom right under the bag (the coin icon is drawn left of the number).
    m_coinLabel = &add<ui::Label>("—", ui::TextRole::Normal);
    m_coinLabel->setWeight(render::FontWeight::Bold).setColor(Color::hex(0xF2CA64)).setShadow(true);
    m_coinLabel->setAlign(render::HAlign::Right, render::VAlign::Middle);
    m_coinLabel->setAnchors(anchor::kTopLeft)
        .setMargins({kBagX + kBagWidth - kCoinWidth, kFooterY - 1.0f, 0.0f, 0.0f})
        .setSize(kCoinWidth, 22.0f);
    m_coinLabel->setName("inv.coins");

    auto& close = add<ui::Button>("X", [this]() {
        if (m_hooks.close)
            m_hooks.close();
    });
    close.setFocusable(false); // Space / Enter keep going to the world
    close.setAnchors(anchor::kTopRight).setMargins({0.0f, -34.0f, -8.0f, 0.0f}).setSize(28.0f, 28.0f);
    close.setName("inv.close");

    auto probe = std::make_unique<TextProbe>();
    probe->setName("inv.tooltip");
    probe->setSize(0.0f, 0.0f);
    m_tooltipProbe = probe.get();
    addChild(std::move(probe));
}

float InventoryWindow::layoutScaleOf() const noexcept
{
    return layoutScale();
}

void InventoryWindow::setMessage(std::string text, bool error)
{
    m_message->setText(text);
    m_message->setColor(error ? kRed : Color::hex(0xE9E2CF));
    m_messageTime = context() != nullptr ? context()->time() : 0.0;
}

const game::ItemStack& InventoryWindow::itemAt(ItemPlace p) const
{
    static const game::ItemStack kEmpty{};
    return game::isValidPlace(p) ? m_inventory.view().at(p) : kEmpty;
}

void InventoryWindow::setLocked(bool locked)
{
    if (locked == m_locked)
        return;
    m_locked = locked;
    m_dragging = false;
    setMessage(locked ? "Ölüyken eşya taşınamaz." : "", locked);
}

void InventoryWindow::beginDrag(ItemPlace from)
{
    if (m_locked)
    {
        setMessage("Ölüyken eşya taşınamaz.", true);
        return;
    }
    m_dragging = true;
    m_dragFrom = from;
}

void InventoryWindow::dragTo(float x, float y)
{
    m_dragX = x;
    m_dragY = y;
}

void InventoryWindow::endDrag(float x, float y, bool drop)
{
    if (!m_dragging)
        return;
    m_dragging = false;
    if (!drop)
        return;
    const ItemSlotWidget* target = slotAt(x, y);
    if (target == nullptr)
    {
        // M14: onto the shop window (sell) or a skill bar slot (a potion); else nothing happens (no
        // dropping on the ground).
        if (m_hooks.dropOutside)
            (void)m_hooks.dropOutside(m_dragFrom, x, y);
        return;
    }
    if (target->place() == m_dragFrom)
        return;
    if (m_hooks.move)
        m_hooks.move(m_dragFrom, target->place());
}

void InventoryWindow::quickMove(ItemPlace from)
{
    if (m_locked)
    {
        setMessage("Ölüyken eşya taşınamaz.", true);
        return;
    }
    if (const auto to = m_inventory.quickTarget(from))
    {
        if (m_hooks.move)
            m_hooks.move(from, *to);
        return;
    }
    // Nowhere to go: say why.
    if (from.isEquipment())
    {
        setMessage(net::itemMoveResultMessage(game::ItemMoveResult::BagFull), true);
        return;
    }
    const game::ItemTemplate* t =
        m_inventory.catalog() != nullptr ? m_inventory.catalog()->find(itemAt(from).templateId) : nullptr;
    const ItemPlace natural = ItemPlace::equipment(t != nullptr && t->slot == game::SlotRule::OffHand ? EquipSlot::OffHand
                                                                                                     : EquipSlot::MainHand);
    const game::ItemMoveResult r = m_inventory.check(from, natural);
    setMessage(net::itemMoveResultMessage(r == game::ItemMoveResult::Ok ? game::ItemMoveResult::WrongSlot : r), true);
}

void InventoryWindow::rightClick(ItemPlace from)
{
    if (m_hooks.rightClick && m_hooks.rightClick(from))
        return;
    quickMove(from);
}

void InventoryWindow::doubleClick(ItemPlace from)
{
    if (m_hooks.doubleClick && m_hooks.doubleClick(from))
        return;
    quickMove(from);
}

float InventoryWindow::iconSweep(std::uint16_t templateId) const
{
    return m_hooks.iconSweep ? m_hooks.iconSweep(templateId) : 0.0f;
}

ItemSlotWidget* InventoryWindow::slotAt(float x, float y) const
{
    for (ItemSlotWidget* s : m_slots)
        if (s->isVisible() && s->rect().contains(x, y))
            return s;
    return nullptr;
}

ItemSlotWidget* InventoryWindow::hoveredSlot() const
{
    if (context() == nullptr)
        return nullptr;
    for (ItemSlotWidget* s : m_slots)
        if (context()->hover() == s)
            return s;
    return nullptr;
}

bool InventoryWindow::onMouseWheel(const platform::InputEvent&)
{
    return true; // the wheel over the window does not zoom the camera
}

void InventoryWindow::onMouseLeave() {}

void InventoryWindow::update(double dt)
{
    ui::Panel::update(dt);
    const ItemSlotWidget* hover = m_dragging ? nullptr : hoveredSlot();
    m_tooltipProbe->setText(hover != nullptr ? tooltipText(hover->place()) : std::string());
    std::size_t used = 0;
    for (std::uint8_t i = 0; i < game::kBagSlots; ++i)
        used += m_inventory.view().at(ItemPlace::bag(i)).empty() ? 0u : 1u;
    m_bagCount->setText(std::format("{}/{}", used, game::kBagSlots));
    if (m_messageTime >= 0.0 && context() != nullptr)
    {
        const double age = context()->time() - m_messageTime;
        if (age > kMessageSeconds)
        {
            m_message->setText("");
            m_messageTime = -1.0;
        }
    }
}

void InventoryWindow::buildTip(ItemPlace p, std::vector<ShownTipLine>& out, const ui::Theme& t) const
{
    out.clear();
    const game::ItemStack& s = itemAt(p);
    if (s.empty())
    {
        if (!p.isEquipment())
            return;
        out.push_back({std::string(equipSlotDisplayName(static_cast<EquipSlot>(p.slot))), t.textTitle, true, false});
        out.push_back({usableSlot(p) ? "Boş" : "Bu yuva henüz kullanılmıyor.", t.textDim, false, false});
        return;
    }
    const game::ItemTemplate* item = m_inventory.catalog() != nullptr ? m_inventory.catalog()->find(s.templateId) : nullptr;
    if (item == nullptr)
    {
        out.push_back({std::format("Bilinmeyen eşya (#{})", s.templateId), t.textTitle, true, false});
        out.push_back({"Bu istemci bu eşyayı tanımıyor.", t.textDim, false, true});
        return;
    }
    std::vector<world::TipLine> tip = world::itemTipLines(*item, m_inventory.user(), s.quantity, s.upgrade);
    if (m_hooks.extraTip)
        m_hooks.extraTip(p, tip); // M14: "Satış: <n> Akçe" while a shop is open
    out = shownTipLines(tip, t);
}

std::string InventoryWindow::tooltipText(ItemPlace p) const
{
    std::vector<ShownTipLine> lines;
    const ui::Theme theme{};
    buildTip(p, lines, context() != nullptr ? context()->theme() : theme);
    return tooltipPlainText(lines);
}

void InventoryWindow::setCoins(std::uint32_t coins, bool known)
{
    m_coins = coins;
    m_coinsKnown = known;
    m_coinLabel->setText(known ? world::formatThousands(coins) : std::string("—"));
}

void InventoryWindow::drawCoins(ui::DrawContext& ctx) const
{
    // Coin icon left of the right-aligned number, a small dim "Akçe" before it.
    const Rect r = m_coinLabel->rect();
    const render::Font& f = ctx.font(ui::TextRole::Normal, render::FontWeight::Bold);
    const float textW = render::measureText(f, m_coinLabel->text());
    const float icon = ctx.px(22.0f);
    const float iconX = std::round(r.right() - textW - ctx.px(6.0f) - icon);
    const Rect iconRect = Rect{iconX, std::round(r.centerY() - icon * 0.5f), icon, icon}.snapped();
    if (const render::Texture2D* coin = m_resources.uiImage("ui/items/coin.png"))
        ctx.batch.drawImage(*coin, iconRect, m_coinsKnown ? colors::kWhite : Color{1.0f, 1.0f, 1.0f, 0.4f});
    else
        ctx.batch.fillRoundedRect(iconRect.inset(ctx.px(4.0f)), ctx.pxf(7.0f), Color::hex(0xC9A247, 0.85f));
    render::TextStyle s;
    s.color = ctx.theme.textDim;
    s.shadow = true;
    const render::Font& wordFont = ctx.font(ui::TextRole::Small);
    const float wordW = render::measureText(wordFont, "Akçe");
    render::drawTextAligned(ctx.batch, wordFont, "Akçe", {iconRect.x - ctx.px(6.0f) - wordW, r.y, wordW, r.h},
                            render::HAlign::Left, render::VAlign::Middle, s);
}

void InventoryWindow::drawSilhouette(ui::DrawContext& ctx) const
{
    auto& b = ctx.batch;
    const Rect c = contentRect();
    const Rect f{c.x + ctx.px(m_figure.x), c.y + ctx.px(m_figure.y), ctx.px(m_figure.w), ctx.px(m_figure.h)};
    b.fillRoundedRect(f, ctx.pxf(6.0f), Color::hex(0x000000, 0.25f));
    const float u = f.h;
    const float cx = f.centerX();
    const Color body = Color::hex(0x7A6A4A, 0.22f);
    const auto shape = [&](float x, float y, float w, float h, float r) {
        b.fillRoundedRect({cx + x * u, f.y + y * u, w * u, h * u}, r * u, body);
    };
    shape(-0.075f, 0.05f, 0.15f, 0.15f, 0.075f);  // head
    shape(-0.15f, 0.22f, 0.30f, 0.07f, 0.03f);    // shoulders
    shape(-0.11f, 0.24f, 0.22f, 0.32f, 0.04f);    // torso
    shape(-0.20f, 0.25f, 0.065f, 0.33f, 0.032f);  // arms
    shape(0.135f, 0.25f, 0.065f, 0.33f, 0.032f);
    shape(-0.10f, 0.56f, 0.085f, 0.40f, 0.04f);   // legs
    shape(0.015f, 0.56f, 0.085f, 0.40f, 0.04f);
    b.strokeRoundedRect(f, ctx.pxf(6.0f), std::max(1.0f, ctx.pxf(1.0f)), ctx.theme.insetBorder.withAlpha(0.5f));
}

void InventoryWindow::drawSelf(ui::DrawContext& ctx)
{
    ui::Panel::drawSelf(ctx);
    drawSilhouette(ctx);
    // Thin divider between equipment and bag.
    const Rect c = contentRect();
    const float x = std::round(c.x + ctx.px(kEquipWidth + 14.0f)) + 0.5f;
    ctx.batch.drawLine({x, c.y + ctx.px(kHeader)}, {x, c.y + ctx.px(kFooterY - 6.0f)}, std::max(1.0f, ctx.pxf(1.0f)),
                       ctx.theme.ornament.withAlpha(0.25f));
    drawCoins(ctx);
}

void InventoryWindow::draw(ui::DrawContext& ctx)
{
    if (!isVisible())
        return;
    ui::Panel::draw(ctx);
    if (m_dragging)
    {
        const game::ItemStack& s = itemAt(m_dragFrom);
        if (const render::Texture2D* icon = s.empty() ? nullptr : m_resources.itemIcon(s.templateId))
        {
            const float size = ctx.px(kSlot * 0.9f);
            ctx.batch.drawImage(*icon, Rect{m_dragX - size * 0.5f, m_dragY - size * 0.5f, size, size}.snapped(),
                                m_resources.itemTint(s.templateId, Color{1.0f, 1.0f, 1.0f, 0.9f}));
        }
    }
    else
    {
        drawTooltip(ctx);
    }
}

void InventoryWindow::drawTooltip(ui::DrawContext& ctx)
{
    const ItemSlotWidget* slot = hoveredSlot();
    if (slot == nullptr || context() == nullptr)
        return;
    std::vector<ShownTipLine> lines;
    buildTip(slot->place(), lines, ctx.theme);
    drawTooltipBox(ctx, lines, slot->rect(), context()->viewportWidth(), context()->viewportHeight());
}

} // namespace vo::client::scenes
