#include "client/scenes/LootWindow.h"

#include "client/render/TextRenderer.h"
#include "client/scenes/SceneResources.h"
#include "client/ui/Button.h"
#include "client/ui/DrawContext.h"
#include "client/ui/Label.h"
#include "client/ui/UiContext.h"
#include "client/world/ClientInventory.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;

namespace {

// Layout (logical units, relative to the content rect).
constexpr float kPadX = 12.0f;
constexpr float kPadTop = 6.0f;
constexpr float kPadBottom = 12.0f;
constexpr float kTitleBar = 38.0f;
constexpr float kRowsY = 24.0f;
constexpr float kRowH = 36.0f;
constexpr float kRowGap = 4.0f;
constexpr float kMessageH = 18.0f;
constexpr float kButtonH = 34.0f;
constexpr double kMessageSeconds = 4.0;
const Color kRed = Color::hex(0xE5736A);
const Color kCoinGold = Color::hex(0xF2CA64);

// A Label that is never drawn: carries text for automation (expecttext).
class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

float rowsHeight(std::size_t rows)
{
    const auto n = static_cast<float>(std::max<std::size_t>(rows, 1));
    return n * kRowH + (n - 1.0f) * kRowGap;
}

} // namespace

// ---- LootRowWidget ----

bool LootRowWidget::onMouseDown(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left && e.button != platform::MouseButton::Right)
        return false;
    // One request per click (the second click of a double click would only get AlreadyTaken).
    if (e.clickCount <= 1)
        m_owner.requestTake(m_row.what());
    return true;
}

void LootRowWidget::drawSelf(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const float radius = ctx.pxf(4.0f);
    const bool hovered = isHovered();
    b.fillRoundedRect(r, radius, hovered ? t.rowHover.withAlpha(0.95f) : Color::hex(0x0C0F13, 0.72f));
    b.strokeRoundedRect(r, radius, std::max(1.0f, ctx.pxf(1.0f)), hovered ? t.buttonBorderHover : t.insetBorder);

    // Icon well on the left.
    const float iconSize = r.h - ctx.px(8.0f);
    const Rect well = Rect{r.x + ctx.px(4.0f), r.y + ctx.px(4.0f), iconSize, iconSize}.snapped();
    b.fillRoundedRect(well, ctx.pxf(3.0f), Color::hex(0x000000, 0.4f));
    const render::Texture2D* icon =
        m_row.coins ? m_owner.resources().uiImage("ui/items/coin.png") : m_owner.resources().itemIcon(m_row.templateId);
    if (icon != nullptr)
        b.drawImage(*icon, well.inset(ctx.px(1.0f)), colors::kWhite);
    else
    {
        render::TextStyle q;
        q.color = t.textDim;
        render::drawTextAligned(b, ctx.font(ui::TextRole::Normal, render::FontWeight::Bold), "?", well,
                                render::HAlign::Center, render::VAlign::Middle, q);
    }

    // Name (rarity colour) and count.
    const game::ItemCatalog* catalog = m_owner.inventory().catalog();
    const game::ItemTemplate* item = !m_row.coins && catalog != nullptr ? catalog->find(m_row.templateId) : nullptr;
    std::string name;
    Color color = t.text;
    if (m_row.coins)
    {
        name = world::formatThousands(m_row.quantity) + " Akçe";
        color = kCoinGold;
    }
    else if (item != nullptr)
    {
        name = item->name;
        color = Color::hex(world::rarityRgb(item->rarity));
    }
    else
        name = std::format("Eşya #{}", m_row.templateId);
    const render::Font& nf = ctx.font(ui::TextRole::Normal, render::FontWeight::Bold);
    const render::Font& cf = ctx.font(ui::TextRole::Normal, render::FontWeight::Bold);
    const std::string count = !m_row.coins && m_row.quantity > 1 ? std::format("×{}", m_row.quantity) : std::string();
    const float countW = count.empty() ? 0.0f : render::measureText(cf, count);
    const float textX = well.right() + ctx.px(8.0f);
    const Rect nameBox{textX, r.y, r.right() - textX - countW - ctx.px(10.0f), r.h};
    render::TextStyle ns;
    ns.color = color;
    ns.shadow = true;
    ns.shadowColor = t.textShadow;
    b.pushClip(nameBox);
    render::drawTextAligned(b, nf, name, nameBox, render::HAlign::Left, render::VAlign::Middle, ns);
    b.popClip();
    if (!count.empty())
    {
        render::TextStyle cs;
        cs.color = t.text;
        cs.shadow = true;
        render::drawTextAligned(b, cf, count, {r.x, r.y, r.w - ctx.px(10.0f), r.h}, render::HAlign::Right,
                                render::VAlign::Middle, cs);
    }
}

// ---- LootWindow ----

LootWindow::LootWindow(const SceneResources& resources, const world::LootWindowModel& model,
                       const world::ClientInventory& inventory, Hooks hooks)
    : ui::Panel(ui::Panel::Style::Window, "Ganimet"), m_resources(resources), m_model(model), m_inventory(inventory),
      m_hooks(std::move(hooks))
{
    setName("loot.window");
    setPadding({kPadX, kPadTop, kPadX, kPadBottom});
    setSize(kWidth, preferredHeight());

    m_source = &add<ui::Label>("", ui::TextRole::Small);
    m_source->setColor(Color::hex(0xA39B88)).setShadow(true);
    m_source->setAnchors(anchor::kTopLeft).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(kWidth - 2.0f * kPadX, 18.0f);
    m_source->setName("loot.source");

    for (std::size_t i = 0; i < m_rows.size(); ++i)
    {
        auto& row = add<LootRowWidget>(*this);
        row.setAnchors(anchor::kTopLeft).setSize(kWidth - 2.0f * kPadX, kRowH);
        row.setVisible(false);
        m_rows[i] = &row;
    }
    m_empty = &add<ui::Label>("Boş", ui::TextRole::Normal);
    m_empty->setColor(Color::hex(0x8F8A82)).setShadow(true).setAlign(render::HAlign::Center, render::VAlign::Middle);
    m_empty->setAnchors(anchor::kTopLeft).setMargins({0.0f, kRowsY, 0.0f, 0.0f}).setSize(kWidth - 2.0f * kPadX, kRowH);
    m_empty->setName("loot.empty");

    m_message = &add<ui::Label>("", ui::TextRole::Small);
    m_message->setShadow(true).setAlign(render::HAlign::Center, render::VAlign::Middle);
    m_message->setAnchors(anchor::kBottomLeft).setMargins({0.0f, 0.0f, 0.0f, kButtonH + 4.0f}).setSize(kWidth - 2.0f * kPadX,
                                                                                                         kMessageH);
    m_message->setName("loot.message");

    m_takeAll = &add<ui::Button>("Hepsini Al  ·  E", [this]() {
        if (m_hooks.takeAll)
            m_hooks.takeAll();
    });
    m_takeAll->setFocusable(false); // Space / Enter keep going to the world
    m_takeAll->setAnchors(anchor::kBottomLeft).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(kWidth - 2.0f * kPadX, kButtonH);
    m_takeAll->setName("loot.takeall");

    auto& close = add<ui::Button>("X", [this]() {
        if (m_hooks.close)
            m_hooks.close();
    });
    close.setFocusable(false);
    close.setAnchors(anchor::kTopRight).setMargins({0.0f, -34.0f, -6.0f, 0.0f}).setSize(28.0f, 28.0f);
    close.setName("loot.close");

    auto probe = std::make_unique<TextProbe>();
    probe->setName("loot.contents");
    probe->setSize(0.0f, 0.0f);
    m_contents = probe.get();
    addChild(std::move(probe));
    refresh("", world::LootLabel::Free);
}

float LootWindow::preferredHeight() const noexcept
{
    return kTitleBar + kPadTop + kRowsY + rowsHeight(m_rowCount) + 6.0f + kMessageH + 4.0f + kButtonH + kPadBottom;
}

void LootWindow::refresh(std::string_view source, world::LootLabel label)
{
    m_label = label;
    m_source->setText(source);
    const std::vector<world::LootRow> rows = m_model.rows();
    m_rowCount = std::min(rows.size(), m_rows.size());
    std::string contents;
    const game::ItemCatalog* catalog = m_inventory.catalog();
    for (std::size_t i = 0; i < m_rows.size(); ++i)
    {
        LootRowWidget& w = *m_rows[i];
        if (i >= m_rowCount)
        {
            w.setVisible(false);
            continue;
        }
        const world::LootRow& r = rows[i];
        w.setRow(r);
        w.setName(r.coins ? std::string("loot.coins") : "loot.entry." + std::to_string(r.index));
        w.setMargins({0.0f, kRowsY + static_cast<float>(i) * (kRowH + kRowGap), 0.0f, 0.0f});
        w.setVisible(true);
        if (!contents.empty())
            contents += '\n';
        if (r.coins)
            contents += "Akçe " + std::to_string(r.quantity);
        else
        {
            const game::ItemTemplate* t = catalog != nullptr ? catalog->find(r.templateId) : nullptr;
            contents += t != nullptr ? t->name : std::format("#{}", r.templateId);
            if (r.quantity > 1)
                contents += std::format(" x{}", r.quantity);
        }
    }
    m_empty->setText(m_model.hasContents() ? "Boş" : "Açılıyor…");
    m_empty->setVisible(m_rowCount == 0);
    m_takeAll->setEnabled(m_rowCount > 0);
    m_contents->setText(contents);
    setSize(kWidth, preferredHeight());
}

void LootWindow::setMessage(std::string text, bool error)
{
    m_message->setText(text);
    m_message->setColor(error ? kRed : Color::hex(0xE9E2CF));
    m_messageTime = context() != nullptr ? context()->time() : 0.0;
}

void LootWindow::requestTake(std::uint8_t what)
{
    if (m_hooks.take)
        m_hooks.take(what);
}

void LootWindow::update(double dt)
{
    ui::Panel::update(dt);
    if (m_messageTime >= 0.0 && context() != nullptr && context()->time() - m_messageTime > kMessageSeconds)
    {
        m_message->setText("");
        m_messageTime = -1.0;
    }
}

bool LootWindow::onMouseWheel(const platform::InputEvent&)
{
    return true; // the wheel over the window does not zoom the camera
}

bool LootWindow::onMouseDown(const platform::InputEvent&)
{
    return true; // a click on the frame does not walk the character there
}

const LootRowWidget* LootWindow::hoveredRow() const
{
    if (context() == nullptr)
        return nullptr;
    for (std::size_t i = 0; i < m_rowCount; ++i)
        if (context()->hover() == m_rows[i])
            return m_rows[i];
    return nullptr;
}

void LootWindow::drawSelf(ui::DrawContext& ctx)
{
    ui::Panel::drawSelf(ctx);
    // Who it belongs to: a small marker right of the subtitle (gold: ours, grey: someone else's).
    const Rect c = contentRect();
    const char* owner = m_label == world::LootLabel::Mine     ? "Senin"
                        : m_label == world::LootLabel::Locked ? "Başkasının"
                                                              : "Herkesin";
    const world::LootLabelStyle st = world::lootLabelStyle(m_label);
    render::TextStyle s;
    s.color = Color::hex(st.labelRgb, st.labelAlpha);
    s.shadow = true;
    render::drawTextAligned(ctx.batch, ctx.font(ui::TextRole::Small, render::FontWeight::Bold), owner,
                            {c.x, c.y, c.w, ctx.px(18.0f)}, render::HAlign::Right, render::VAlign::Middle, s);
    // Gold rule under the subtitle.
    const float y = std::round(c.y + ctx.px(20.0f)) + 0.5f;
    ctx.batch.drawLine({c.x, y}, {c.right(), y}, std::max(1.0f, ctx.pxf(1.0f)), ctx.theme.ornament.withAlpha(0.3f));
}

void LootWindow::draw(ui::DrawContext& ctx)
{
    if (!isVisible())
        return;
    ui::Panel::draw(ctx);
    const LootRowWidget* row = hoveredRow();
    if (row == nullptr || row->row().coins || context() == nullptr)
        return;
    const game::ItemCatalog* catalog = m_inventory.catalog();
    const game::ItemTemplate* item = catalog != nullptr ? catalog->find(row->row().templateId) : nullptr;
    if (item == nullptr)
        return;
    const std::vector<ShownTipLine> lines =
        shownTipLines(world::itemTipLines(*item, m_inventory.user(), row->row().quantity), ctx.theme);
    drawTooltipBox(ctx, lines, row->rect(), context()->viewportWidth(), context()->viewportHeight());
}

} // namespace vo::client::scenes
