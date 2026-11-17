#include "client/scenes/ShopWindow.h"

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

namespace {

// Layout (logical units, relative to the content rect).
constexpr float kPadX = 12.0f;
constexpr float kPadTop = 4.0f;
constexpr float kPadBottom = 10.0f;
constexpr float kTitleBar = 38.0f;
constexpr float kSubtitleH = 17.0f;
constexpr float kRuleGap = 7.0f;
constexpr float kRowH = 34.0f;
constexpr float kRowGap = 2.0f;
constexpr float kSellH = 40.0f;
constexpr float kMessageH = 16.0f;
constexpr float kFooterH = 22.0f;
constexpr double kMessageSeconds = 5.0;
const Color kRed = Color::hex(0xE5736A);
const Color kGold = Color::hex(0xF2CA64);
constexpr const char* kAkceIcon = "ui/shop/akce.png";
constexpr const char* kSellIcon = "ui/shop/tab_sat.png";

class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

// The sell zone: drawn by the window (it lights while a bag item is dragged); mouse-transparent so a
// click passes to the window (which eats it).
class SellZone final : public ui::Widget
{
public:
    SellZone() { setMouseTransparent(true); }
};

void drawAkce(ui::DrawContext& ctx, const SceneResources& res, float right, float centerY, float size, float alpha = 1.0f)
{
    const Rect r = Rect{std::round(right - size), std::round(centerY - size * 0.5f), size, size}.snapped();
    if (const render::Texture2D* t = res.uiImage(kAkceIcon))
        ctx.batch.drawImage(*t, r, Color{1.0f, 1.0f, 1.0f, alpha});
    else
        ctx.batch.fillRoundedRect(r.inset(ctx.px(1.0f)), r.w * 0.5f, Color::hex(0xC9A247, 0.9f * alpha));
}

} // namespace

// ---- ShopRowWidget ----

bool ShopRowWidget::onMouseDown(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left && e.button != platform::MouseButton::Right)
        return false;
    if (e.clickCount <= 1)
        m_owner.requestBuy(m_offer.templateId);
    return true;
}

void ShopRowWidget::drawSelf(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const float radius = ctx.pxf(4.0f);
    const bool busy = m_owner.busy();
    const bool hovered = isHovered() && !busy;
    b.fillRoundedRect(r, radius, hovered ? t.rowHover.withAlpha(0.95f) : Color::hex(0x0C0F13, 0.72f));
    b.strokeRoundedRect(r, radius, std::max(1.0f, ctx.pxf(1.0f)), hovered ? t.buttonBorderHover : t.insetBorder);

    const float iconSize = r.h - ctx.px(6.0f);
    const Rect well = Rect{r.x + ctx.px(3.0f), r.y + ctx.px(3.0f), iconSize, iconSize}.snapped();
    b.fillRoundedRect(well, ctx.pxf(3.0f), Color::hex(0x000000, 0.4f));
    if (const render::Texture2D* icon = m_owner.resources().itemIcon(m_offer.templateId))
        b.drawImage(*icon, well.inset(ctx.px(1.0f)), busy ? Color{0.6f, 0.6f, 0.6f, 1.0f} : colors::kWhite);

    const game::ItemCatalog* catalog = m_owner.inventory().catalog();
    const game::ItemTemplate* item = catalog != nullptr ? catalog->find(m_offer.templateId) : nullptr;
    const std::string name = item != nullptr ? item->name : std::format("Eşya #{}", m_offer.templateId);
    const world::ShopEntryView view = item != nullptr ? world::shopEntryView(*item, m_offer.price, m_owner.coins(),
                                                                            m_owner.inventory().user())
                                                      : world::ShopEntryView{m_offer.price <= m_owner.coins(), "", false};

    // Price on the right: "<n>" and the Akçe coin; red when the balance is too low.
    const render::Font& pf = ctx.font(ui::TextRole::Normal, render::FontWeight::Bold);
    const std::string price = world::formatThousands(m_offer.price);
    const float coin = ctx.px(16.0f);
    const float priceRight = r.right() - ctx.px(8.0f);
    drawAkce(ctx, m_owner.resources(), priceRight, r.centerY(), coin, view.affordable ? 1.0f : 0.55f);
    const float priceW = render::measureText(pf, price);
    render::TextStyle ps;
    ps.color = view.affordable ? kGold : kRed;
    ps.shadow = true;
    ps.shadowColor = t.textShadow;
    render::drawTextAligned(b, pf, price, {priceRight - coin - ctx.px(4.0f) - priceW, r.y, priceW, r.h},
                            render::HAlign::Left, render::VAlign::Middle, ps);

    // Name (rarity colour) over the detail line.
    const float textX = well.right() + ctx.px(8.0f);
    const float textR = priceRight - coin - ctx.px(10.0f) - priceW;
    const render::Font& nf = ctx.font(14.0f, render::FontWeight::Bold);
    const render::Font& df = ctx.font(12.0f);
    const float h2 = nf.lineHeight() + df.lineHeight();
    const float top = std::round(r.centerY() - h2 * 0.5f);
    render::TextStyle ns;
    ns.color = Color::hex(item != nullptr ? world::rarityRgb(item->rarity) : 0xEDE6D3);
    if (busy)
        ns.color = ns.color.withAlpha(0.6f);
    ns.shadow = true;
    ns.shadowColor = t.textShadow;
    const Rect nameBox{textX, top, std::max(1.0f, textR - textX), nf.lineHeight()};
    b.pushClip({nameBox.x, r.y, nameBox.w, r.h});
    render::drawTextAligned(b, nf, name, nameBox, render::HAlign::Left, render::VAlign::Top, ns);
    if (!view.detail.empty())
    {
        render::TextStyle ds;
        ds.color = view.detailUnmet ? kRed : t.textDim;
        ds.shadow = true;
        ds.shadowColor = t.textShadow;
        render::drawTextAligned(b, df, view.detail, {textX, top + nf.lineHeight(), nameBox.w, df.lineHeight()},
                                render::HAlign::Left, render::VAlign::Top, ds);
    }
    b.popClip();
}

// ---- ShopWindow ----

ShopWindow::ShopWindow(const SceneResources& resources, const world::ShopSession& session,
                       const world::ClientInventory& inventory, Hooks hooks)
    : ui::Panel(ui::Panel::Style::Window, "Satıcı"), m_resources(resources), m_session(session), m_inventory(inventory),
      m_hooks(std::move(hooks))
{
    setName("shop.window");
    setPadding({kPadX, kPadTop, kPadX, kPadBottom});
    setSize(kWidth, preferredHeight());
    const float w = kWidth - 2.0f * kPadX;

    m_subtitle = &add<ui::Label>("", ui::TextRole::Small);
    m_subtitle->setColor(Color::hex(world::kNpcTitleRgb)).setWeight(render::FontWeight::Bold).setShadow(true);
    m_subtitle->setAlign(render::HAlign::Center, render::VAlign::Middle);
    m_subtitle->setAnchors(anchor::kTopLeft).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(w, kSubtitleH);
    m_subtitle->setName("shop.title");

    m_greeting = &add<ui::Label>("", ui::TextRole::Small);
    m_greeting->setColor(Color::hex(0xCBBE9C)).setWrap(true).setShadow(true);
    m_greeting->setAlign(render::HAlign::Center, render::VAlign::Top);
    m_greeting->setAnchors(anchor::kTopLeft).setMargins({0.0f, kSubtitleH + 1.0f, 0.0f, 0.0f}).setSize(w, m_greetingH);
    m_greeting->setName("shop.greeting");

    for (std::size_t i = 0; i < m_rows.size(); ++i)
    {
        auto& row = add<ShopRowWidget>(*this);
        row.setAnchors(anchor::kTopLeft).setSize(w, kRowH);
        row.setVisible(false);
        m_rows[i] = &row;
    }

    m_sellZone = &add<SellZone>();
    m_sellZone->setAnchors(anchor::kTopLeft).setSize(w, kSellH);
    m_sellZone->setName("shop.sellzone");
    m_sellLabel = &m_sellZone->add<ui::Label>(std::string(world::kSellHintText), ui::TextRole::Small);
    m_sellLabel->setWrap(true).setShadow(true).setColor(Color::hex(0xD9CFB4));
    m_sellLabel->setAlign(render::HAlign::Left, render::VAlign::Middle);
    m_sellLabel->setAnchors(anchor::kFill).setMargins({46.0f, 2.0f, 6.0f, 2.0f});

    m_message = &add<ui::Label>("", ui::TextRole::Small);
    m_message->setShadow(true).setAlign(render::HAlign::Center, render::VAlign::Middle);
    m_message->setAnchors(anchor::kBottomLeft).setMargins({0.0f, 0.0f, 0.0f, kFooterH + 2.0f}).setSize(w, kMessageH);
    m_message->setName("shop.message");

    m_coinLabel = &add<ui::Label>("—", ui::TextRole::Normal);
    m_coinLabel->setWeight(render::FontWeight::Bold).setColor(kGold).setShadow(true);
    m_coinLabel->setAlign(render::HAlign::Right, render::VAlign::Middle);
    m_coinLabel->setAnchors(anchor::kBottomLeft).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(w - 24.0f, kFooterH);
    m_coinLabel->setName("shop.coins");

    auto& close = add<ui::Button>("X", [this]() {
        if (m_hooks.close)
            m_hooks.close();
    });
    close.setFocusable(false);
    close.setAnchors(anchor::kTopRight).setMargins({0.0f, -34.0f, -6.0f, 0.0f}).setSize(28.0f, 28.0f);
    close.setName("shop.close");

    auto probe = std::make_unique<TextProbe>();
    probe->setName("shop.contents");
    probe->setSize(0.0f, 0.0f);
    m_contents = probe.get();
    addChild(std::move(probe));
    relayout();
}

float ShopWindow::preferredHeight() const noexcept
{
    const auto n = static_cast<float>(std::max<std::size_t>(m_rowCount, 1));
    const float rows = n * kRowH + (n - 1.0f) * kRowGap;
    return kTitleBar + kPadTop + kSubtitleH + 1.0f + m_greetingH + kRuleGap + rows + kRuleGap + kSellH + 4.0f + kMessageH +
           2.0f + kFooterH + kPadBottom;
}

void ShopWindow::open(std::string_view name, std::string_view title, std::string_view greeting)
{
    setTitle(std::string(name));
    m_subtitle->setText(world::npcTitlePlate(title));
    m_greeting->setText(greeting.empty() ? std::string() : std::format("“{}”", greeting));
    setMessage("", false);
    relayout();
}

void ShopWindow::relayout()
{
    const std::span<const world::ShopOffer> offers = m_session.offers();
    m_rowCount = std::min(offers.size(), m_rows.size());
    std::string contents;
    const game::ItemCatalog* catalog = m_inventory.catalog();
    const float rowsTop = kSubtitleH + 1.0f + m_greetingH + kRuleGap;
    for (std::size_t i = 0; i < m_rows.size(); ++i)
    {
        ShopRowWidget& w = *m_rows[i];
        if (i >= m_rowCount)
        {
            w.setVisible(false);
            continue;
        }
        w.setOffer(offers[i]);
        w.setName(std::format("shop.item.{}", offers[i].templateId));
        w.setMargins({0.0f, rowsTop + static_cast<float>(i) * (kRowH + kRowGap), 0.0f, 0.0f});
        w.setVisible(true);
        const game::ItemTemplate* t = catalog != nullptr ? catalog->find(offers[i].templateId) : nullptr;
        if (!contents.empty())
            contents += '\n';
        contents += std::format("{} {}", t != nullptr ? t->name : std::format("#{}", offers[i].templateId), offers[i].price);
    }
    const auto n = static_cast<float>(std::max<std::size_t>(m_rowCount, 1));
    m_sellZone->setMargins({0.0f, rowsTop + n * kRowH + (n - 1.0f) * kRowGap + kRuleGap, 0.0f, 0.0f});
    m_contents->setText(contents);
    setSize(kWidth, preferredHeight());
}

void ShopWindow::setCoins(std::uint32_t coins, bool known)
{
    m_coins = coins;
    m_coinsKnown = known;
    m_coinLabel->setText(known ? world::formatThousands(coins) : std::string("—"));
}

void ShopWindow::setMessage(std::string text, bool error)
{
    m_message->setText(text);
    m_message->setColor(error ? kRed : Color::hex(0xE9E2CF));
    m_messageTime = context() != nullptr ? context()->time() : 0.0;
}

const std::string& ShopWindow::message() const noexcept
{
    return m_message->text();
}

Rect ShopWindow::sellZone() const
{
    return m_sellZone->rect();
}

void ShopWindow::requestBuy(std::uint16_t templateId)
{
    if (m_session.busy())
        return; // one request at a time: the rows wait
    if (m_hooks.buy)
        m_hooks.buy(templateId);
}

void ShopWindow::update(double dt)
{
    ui::Panel::update(dt);
    if (m_messageTime >= 0.0 && context() != nullptr && context()->time() - m_messageTime > kMessageSeconds)
    {
        m_message->setText("");
        m_messageTime = -1.0;
    }
    if (m_rowCount != std::min(m_session.offers().size(), m_rows.size()))
        relayout();
}

bool ShopWindow::onMouseWheel(const platform::InputEvent&)
{
    return true;
}

bool ShopWindow::onMouseDown(const platform::InputEvent&)
{
    return true; // a click on the frame does not walk the character there
}

const ShopRowWidget* ShopWindow::hoveredRow() const
{
    if (context() == nullptr)
        return nullptr;
    for (std::size_t i = 0; i < m_rowCount; ++i)
        if (context()->hover() == m_rows[i])
            return m_rows[i];
    return nullptr;
}

void ShopWindow::drawSelf(ui::DrawContext& ctx)
{
    ui::Panel::drawSelf(ctx);
    auto& b = ctx.batch;
    const ui::Theme& t = ctx.theme;
    const Rect c = contentRect();
    // Gold rules: under the greeting and above the sell zone.
    const float rowsTop = std::round(c.y + ctx.px(kSubtitleH + 1.0f + m_greetingH + kRuleGap * 0.5f)) + 0.5f;
    b.drawLine({c.x, rowsTop}, {c.right(), rowsTop}, std::max(1.0f, ctx.pxf(1.0f)), t.ornament.withAlpha(0.3f));

    // The sell zone: an inset well with the "Sat" glyph; lit while a bag item is dragged.
    const Rect z = m_sellZone->rect();
    const bool lit = m_hooks.sellDragActive && m_hooks.sellDragActive();
    b.fillRoundedRect(z, ctx.pxf(5.0f), lit ? t.accent.withAlpha(0.22f) : Color::hex(0x0A0D10, 0.6f));
    b.strokeRoundedRect(z, ctx.pxf(5.0f), std::max(1.0f, ctx.pxf(lit ? 2.0f : 1.0f)),
                        lit ? t.accentBright : t.panelBorder.withAlpha(0.7f));
    const float glyph = ctx.px(34.0f);
    const Rect gr = Rect{z.x + ctx.px(6.0f), std::round(z.centerY() - glyph * 0.5f), glyph, glyph}.snapped();
    if (const render::Texture2D* sell = m_resources.uiImage(kSellIcon))
        b.drawImage(*sell, gr);
    else
    {
        render::TextStyle s;
        s.color = kGold;
        render::drawTextAligned(b, ctx.font(ui::TextRole::Normal, render::FontWeight::Bold), "Sat", gr,
                                render::HAlign::Center, render::VAlign::Middle, s);
    }

    // Footer: "Akçe:" and the coin left of the right-aligned balance.
    const Rect cr = m_coinLabel->rect();
    const render::Font& f = ctx.font(ui::TextRole::Normal, render::FontWeight::Bold);
    const float textW = render::measureText(f, m_coinLabel->text());
    const float coin = ctx.px(18.0f);
    const float coinRight = std::round(cr.right() + ctx.px(4.0f) + coin);
    drawAkce(ctx, m_resources, coinRight, cr.centerY(), coin, m_coinsKnown ? 1.0f : 0.4f);
    render::TextStyle s;
    s.color = t.textDim;
    s.shadow = true;
    const render::Font& wf = ctx.font(ui::TextRole::Small);
    const float wordW = render::measureText(wf, "Akçe:");
    render::drawTextAligned(b, wf, "Akçe:", {cr.right() - textW - ctx.px(6.0f) - wordW, cr.y, wordW, cr.h},
                            render::HAlign::Left, render::VAlign::Middle, s);
    if (m_session.busy())
    {
        render::TextStyle bs;
        bs.color = t.textDim;
        bs.shadow = true;
        render::drawTextAligned(b, wf, "Bekleniyor…", {c.x, cr.y, c.w * 0.5f, cr.h}, render::HAlign::Left,
                                render::VAlign::Middle, bs);
    }
}

void ShopWindow::draw(ui::DrawContext& ctx)
{
    if (!isVisible())
        return;
    ui::Panel::draw(ctx);
    const ShopRowWidget* row = hoveredRow();
    if (row == nullptr || context() == nullptr)
        return;
    const game::ItemCatalog* catalog = m_inventory.catalog();
    const game::ItemTemplate* item = catalog != nullptr ? catalog->find(row->offer().templateId) : nullptr;
    if (item == nullptr)
        return;
    std::vector<world::TipLine> tip = world::itemTipLines(*item, m_inventory.user(), 0);
    std::erase_if(tip, [](const world::TipLine& l) { return l.tone == world::TipTone::Value; }); // the price says it
    std::vector<ShownTipLine> lines = shownTipLines(tip, ctx.theme);
    lines.push_back({std::format("Fiyat: {}", world::priceText(row->offer().price)),
                     row->offer().price <= m_coins ? Color::hex(0xE9C46A) : kRed, false, false,
                     row->offer().price > m_coins});
    drawTooltipBox(ctx, lines, row->rect(), context()->viewportWidth(), context()->viewportHeight());
}

// ---- QuantityDialog ----

QuantityDialog::QuantityDialog(QuantityDesc desc) : m_desc(std::move(desc)), m_model(m_desc.max, m_desc.initial)
{
    setName("quantity");
    setFocusable(true);
    m_panel = &add<ui::Panel>(ui::Panel::Style::Window, m_desc.title);
    m_panel->setAnchors(anchor::kCenter).setSize(330.0f, 214.0f);
    m_panel->setPadding({18.0f, 10.0f, 18.0f, 14.0f});
    m_panel->setName("quantity.panel");

    auto& prompt = m_panel->add<ui::Label>(m_desc.prompt, ui::TextRole::Normal);
    prompt.setWeight(render::FontWeight::Bold).setShadow(true).setAlign(render::HAlign::Center);
    prompt.setAnchors(anchor::kTopLeft | anchor::kRight).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(0.0f, 22.0f);
    prompt.setName("quantity.prompt");

    auto& down = m_panel->add<ui::Button>("-", [this]() { setValue(static_cast<int>(m_model.value) - 1); });
    down.setFocusable(false);
    down.setAnchors(anchor::kTopLeft).setMargins({60.0f, 32.0f, 0.0f, 0.0f}).setSize(38.0f, 38.0f);
    down.setName("quantity.down");
    m_value = &m_panel->add<ui::Label>("", ui::TextRole::Large);
    m_value->setWeight(render::FontWeight::Bold).setShadow(true).setAlign(render::HAlign::Center);
    m_value->setAnchors(anchor::kTopLeft).setMargins({104.0f, 32.0f, 0.0f, 0.0f}).setSize(86.0f, 38.0f);
    m_value->setName("quantity.value");
    auto& up = m_panel->add<ui::Button>("+", [this]() { setValue(static_cast<int>(m_model.value) + 1); });
    up.setFocusable(false);
    up.setAnchors(anchor::kTopLeft).setMargins({196.0f, 32.0f, 0.0f, 0.0f}).setSize(38.0f, 38.0f);
    up.setName("quantity.up");

    m_total = &m_panel->add<ui::Label>("", ui::TextRole::Normal);
    m_total->setColor(kGold).setWeight(render::FontWeight::Bold).setShadow(true).setAlign(render::HAlign::Center);
    m_total->setAnchors(anchor::kTopLeft | anchor::kRight).setMargins({0.0f, 78.0f, 0.0f, 0.0f}).setSize(0.0f, 22.0f);
    m_total->setName("quantity.total");

    auto& ok = m_panel->add<ui::Button>(m_desc.sell ? "Sat" : "Al", [this]() { close(true); });
    ok.setFocusable(false);
    ok.setAnchors(anchor::kBottomLeft).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(140.0f, 36.0f);
    ok.setName("quantity.ok");
    auto& cancel = m_panel->add<ui::Button>("Vazgeç", [this]() { close(false); });
    cancel.setFocusable(false);
    cancel.setAnchors(anchor::kBottomRight).setMargins({0.0f, 0.0f, 0.0f, 0.0f}).setSize(140.0f, 36.0f);
    cancel.setName("quantity.cancel");
    refresh();
}

QuantityDialog& QuantityDialog::show(ui::UiContext& ui, QuantityDesc desc)
{
    auto dialog = std::make_unique<QuantityDialog>(std::move(desc));
    QuantityDialog& ref = *dialog;
    ui.pushModal(std::move(dialog));
    ui.setFocus(&ref);
    return ref;
}

void QuantityDialog::setValue(int v)
{
    m_model.set(v);
    refresh();
}

void QuantityDialog::refresh()
{
    m_value->setText(std::to_string(m_model.value));
    const std::uint64_t total = m_model.total(m_desc.unitPrice);
    m_total->setText(m_desc.sell ? world::sellTotalText(total) : world::buyTotalText(total));
}

void QuantityDialog::close(bool confirmed)
{
    if (m_closed)
        return;
    m_closed = true;
    const std::uint16_t q = m_model.value;
    auto onConfirm = m_desc.onConfirm;
    auto onCancel = m_desc.onCancel;
    if (context() != nullptr)
        context()->closeModal(*this); // (this may be destroyed now: only the copies are used below)
    if (confirmed && onConfirm)
        onConfirm(q);
    else if (!confirmed && onCancel)
        onCancel();
}

bool QuantityDialog::onKeyDown(const platform::InputEvent& e)
{
    namespace k = platform::keys;
    switch (e.key)
    {
    case k::kEscape: close(false); return true;
    case k::kEnter: close(true); return true;
    case k::kUp:
    case k::kRight: setValue(static_cast<int>(m_model.value) + 1); m_typed = true; return true;
    case k::kDown:
    case k::kLeft: setValue(static_cast<int>(m_model.value) - 1); m_typed = true; return true;
    case k::kPageUp: setValue(static_cast<int>(m_model.value) + 5); m_typed = true; return true;
    case k::kPageDown: setValue(static_cast<int>(m_model.value) - 5); m_typed = true; return true;
    case k::kHome: setValue(1); m_typed = true; return true;
    case k::kEnd: setValue(m_model.max); m_typed = true; return true;
    case k::kBackspace:
        setValue(static_cast<int>(m_model.value) / 10);
        m_typed = true;
        return true;
    default: break;
    }
    return true; // modal: nothing goes through
}

bool QuantityDialog::onChar(const platform::InputEvent& e)
{
    if (e.codePoint < U'0' || e.codePoint > U'9')
        return true;
    const int digit = static_cast<int>(e.codePoint - U'0');
    const int next = m_typed ? static_cast<int>(m_model.value) * 10 + digit : digit;
    m_typed = true;
    setValue(next);
    return true;
}

bool QuantityDialog::onMouseWheel(const platform::InputEvent& e)
{
    setValue(static_cast<int>(m_model.value) + (e.wheel > 0.0f ? 1 : -1));
    return true;
}

void QuantityDialog::drawSelf(ui::DrawContext& ctx)
{
    ctx.batch.fillRect(rect(), ctx.theme.overlayDim);
}

void QuantityDialog::draw(ui::DrawContext& ctx)
{
    ui::Widget::draw(ctx);
    // The item's icon left of the number (over the panel).
    if (m_desc.icon != nullptr && m_panel != nullptr && isVisible())
    {
        const Rect c = m_panel->contentRect();
        const float s = ctx.px(40.0f);
        ctx.batch.drawImage(*m_desc.icon, Rect{c.x + ctx.px(6.0f), c.y + ctx.px(31.0f), s, s}.snapped());
    }
}

} // namespace vo::client::scenes
