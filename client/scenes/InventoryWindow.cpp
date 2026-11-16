#include "client/scenes/InventoryWindow.h"

#include "client/net/NetText.h"
#include "client/render/TextRenderer.h"
#include "client/scenes/SceneResources.h"
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
constexpr float kTooltipWidth = 270.0f;
constexpr float kDragThreshold = 4.0f;
constexpr double kMessageSeconds = 5.0;
const Color kRed = Color::hex(0xE5736A);
const Color kGood = Color::hex(0x9FD27A);

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

std::string classList(const game::ItemTemplate& t)
{
    std::string out;
    for (std::uint8_t c = 1; c <= 4; ++c)
    {
        if (!t.allowsClass(c))
            continue;
        if (!out.empty())
            out += ", ";
        out += game::className(static_cast<game::CharClass>(c));
    }
    return out.empty() ? std::string("—") : out;
}

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
        if (hasItem)
            m_owner.quickMove(m_place);
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
            b.drawImage(*icon, inner, source ? Color{1.0f, 1.0f, 1.0f, 0.3f} : colors::kWhite);
        }
        else
        {
            render::TextStyle s;
            s.color = t.textDim;
            render::drawTextAligned(b, ctx.font(ui::TextRole::Large, render::FontWeight::Bold), "?", r,
                                    render::HAlign::Center, render::VAlign::Middle, s);
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
    m_message->setAnchors(anchor::kTopLeft).setMargins({0.0f, kFooterY, 0.0f, 0.0f}).setSize(kBagX + kBagWidth, 20.0f);
    m_message->setName("inv.message");

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

void InventoryWindow::beginDrag(ItemPlace from)
{
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
    if (target == nullptr || target->place() == m_dragFrom)
        return; // dropped outside a slot: nothing happens (no dropping on the ground in M8)
    if (m_hooks.move)
        m_hooks.move(m_dragFrom, target->place());
}

void InventoryWindow::quickMove(ItemPlace from)
{
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

void InventoryWindow::buildTip(ItemPlace p, std::vector<TipLine>& out, const ui::Theme& t) const
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
    const game::ItemUser& user = m_inventory.user();
    out.push_back({item->name, t.textTitle, true, false});
    out.push_back({std::format("{} · {}", game::itemKindDisplayName(item->kind), game::slotRuleDisplayName(item->slot)),
                   t.textDim, false, false});
    if (item->attack != 0)
        out.push_back({std::format("Saldırı +{}", item->attack), kGood, false, false});
    if (item->defense != 0)
        out.push_back({std::format("Savunma +{}", item->defense), kGood, false, false});
    out.push_back({"Sınıf: " + classList(*item), item->allowsClass(user.charClass) ? t.text : kRed, false, true});
    out.push_back({std::format("Gereken seviye: {}", item->level), user.level >= item->level ? t.text : kRed, false,
                   false});
    if (!item->description.empty())
        out.push_back({item->description, t.textDim, false, true});
}

std::string InventoryWindow::tooltipText(ItemPlace p) const
{
    std::vector<TipLine> lines;
    const ui::Theme theme{};
    buildTip(p, lines, context() != nullptr ? context()->theme() : theme);
    std::string out;
    for (const TipLine& l : lines)
    {
        if (!out.empty())
            out += '\n';
        out += l.text;
        if (l.color.r == kRed.r && l.color.g == kRed.g && l.color.b == kRed.b)
            out += " (karşılanmıyor)";
    }
    return out;
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
                                Color{1.0f, 1.0f, 1.0f, 0.9f});
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
    std::vector<TipLine> lines;
    buildTip(slot->place(), lines, ctx.theme);
    if (lines.empty())
        return;
    auto& b = ctx.batch;
    const ui::Theme& t = ctx.theme;
    const float pad = ctx.px(10.0f);
    const float gap = ctx.px(2.0f);
    const float maxW = ctx.px(kTooltipWidth);
    const render::Font& titleFont = ctx.font(ui::TextRole::Large, render::FontWeight::Bold);
    const render::Font& bodyFont = ctx.font(ui::TextRole::Small);

    // Measure.
    struct Laid
    {
        const TipLine* line;
        const render::Font* font;
        std::vector<ui::TextLine> wrapped;
        float h;
    };
    std::vector<Laid> laid;
    float w = 0.0f;
    float h = 0.0f;
    for (const TipLine& l : lines)
    {
        Laid x{&l, l.title ? &titleFont : &bodyFont, {}, 0.0f};
        ui::wrapText(*x.font, l.text, l.wrap ? maxW : 0.0f, x.wrapped);
        for (const ui::TextLine& tl : x.wrapped)
            w = std::max(w, tl.width);
        x.h = x.font->lineHeight() * static_cast<float>(std::max<std::size_t>(1, x.wrapped.size()));
        h += x.h + gap;
        laid.push_back(std::move(x));
    }
    w = std::min(std::max(w, ctx.px(150.0f)), maxW);
    const float boxW = std::round(w + 2.0f * pad);
    const float boxH = std::round(h - gap + 2.0f * pad + ctx.px(4.0f));

    // Right of the slot, else left of it; inside the screen vertically.
    const Rect sr = slot->rect();
    const float vw = context()->viewportWidth();
    const float vh = context()->viewportHeight();
    const float margin = ctx.px(4.0f);
    float x = sr.right() + ctx.px(8.0f);
    if (x + boxW > vw - margin)
        x = sr.x - ctx.px(8.0f) - boxW;
    x = std::clamp(x, margin, std::max(margin, vw - margin - boxW));
    float y = std::clamp(sr.y, margin, std::max(margin, vh - margin - boxH));
    const Rect box = Rect{x, y, boxW, boxH}.snapped();

    b.fillRoundedRect({box.x, box.y + ctx.px(3.0f), box.w, box.h}, ctx.pxf(6.0f), Color::hex(0x000000, 0.45f));
    b.fillRoundedRectGradient(box, ctx.pxf(5.0f), Color::hex(0x1E242C, 0.98f), Color::hex(0x0E1216, 0.98f));
    b.strokeRoundedRect(box, ctx.pxf(5.0f), std::max(1.0f, ctx.pxf(1.0f)), t.panelBorder);
    float pen = box.y + pad;
    for (std::size_t i = 0; i < laid.size(); ++i)
    {
        const Laid& l = laid[i];
        render::TextStyle s;
        s.color = l.line->color;
        s.shadow = true;
        s.shadowColor = t.textShadow;
        render::drawTextLines(b, *l.font, l.line->text, l.wrapped, {box.x + pad, pen, box.w - 2.0f * pad, l.h},
                              render::HAlign::Left, render::VAlign::Top, s);
        pen += l.h + gap;
        if (i == 0) // gold rule under the name
        {
            const float ry = std::round(pen + ctx.px(1.0f)) + 0.5f;
            b.drawLine({box.x + pad, ry}, {box.right() - pad, ry}, std::max(1.0f, ctx.pxf(1.0f)), t.ornament.withAlpha(0.5f));
            pen += ctx.px(4.0f);
        }
    }
}

} // namespace vo::client::scenes
