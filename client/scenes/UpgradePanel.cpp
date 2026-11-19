#include "client/scenes/UpgradePanel.h"

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
#include <numbers>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;

namespace {

// Layout (logical, relative to the panel).
constexpr float kWell = 60.0f;
constexpr float kTextX = 70.0f;
constexpr float kRuleY = 72.0f;
constexpr float kRow1 = 78.0f;
constexpr float kRowH = 20.0f;
constexpr float kRowStep = 22.0f;
constexpr float kFailY = kRow1 + 3.0f * kRowStep;
constexpr float kFailH = 32.0f;
constexpr float kButtonY = kFailY + kFailH + 4.0f;
constexpr float kButtonH = 34.0f;
constexpr float kResultY = kButtonY + kButtonH + 4.0f;
constexpr float kResultH = 40.0f;
constexpr float kStoneIcon = 18.0f;
constexpr double kEffectSeconds = 1.8;
constexpr double kResultSeconds = 8.0;
const Color kRed = Color::hex(0xE5736A);
const Color kGold = Color::hex(0xF2CA64);
const Color kGood = Color::hex(0x9AD27A);
constexpr const char* kAkceIcon = "ui/shop/akce.png";

class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

// The well: drawn by the panel; mouse-transparent so a click reaches the panel (right click empties it).
class Well final : public ui::Widget
{
public:
    Well() { setMouseTransparent(true); }
};

ui::Label& label(ui::Widget& parent, ui::TextRole role, float x, float y, float w, float h)
{
    auto& l = parent.add<ui::Label>("", role);
    l.setShadow(true);
    l.setAnchors(anchor::kTopLeft).setMargins({x, y, 0.0f, 0.0f}).setSize(w, h);
    return l;
}

} // namespace

UpgradePanel::UpgradePanel(const SceneResources& resources, const world::ClientInventory& inventory,
                           const world::UpgradeSession& session, Hooks hooks)
    : m_resources(resources), m_inventory(inventory), m_session(session), m_hooks(std::move(hooks))
{
    setName("upgrade.panel");
    const float w = 276.0f;
    m_slot = &add<Well>();
    m_slot->setAnchors(anchor::kTopLeft).setMargins({0.0f, 4.0f, 0.0f, 0.0f}).setSize(kWell, kWell);
    m_slot->setName("upgrade.slot");

    m_name = &label(*this, ui::TextRole::Normal, kTextX, 2.0f, w - kTextX, 20.0f);
    m_name->setWeight(render::FontWeight::Bold);
    m_name->setName("upgrade.name");
    m_level = &label(*this, ui::TextRole::Normal, kTextX, 22.0f, w - kTextX, 20.0f);
    m_level->setWeight(render::FontWeight::Bold).setColor(kGold);
    m_stat = &label(*this, ui::TextRole::Small, kTextX, 42.0f, w - kTextX, 20.0f);
    m_stat->setColor(kGood);
    m_hint = &label(*this, ui::TextRole::Small, kTextX, 4.0f, w - kTextX, kWell);
    m_hint->setWrap(true).setColor(Color::hex(0xD9CFB4)).setAlign(render::HAlign::Left, render::VAlign::Middle);
    m_hint->setName("upgrade.hint");

    m_stoneKey = &label(*this, ui::TextRole::Small, 0.0f, kRow1, 40.0f, kRowH);
    m_stoneKey->setText("Taş:");
    m_stone = &label(*this, ui::TextRole::Small, 44.0f + kStoneIcon + 4.0f, kRow1, 150.0f, kRowH);
    m_stone->setWeight(render::FontWeight::Bold);
    m_owned = &label(*this, ui::TextRole::Small, w - 80.0f, kRow1, 80.0f, kRowH);
    m_owned->setAlign(render::HAlign::Right);
    m_owned->setName("upgrade.owned");
    m_feeKey = &label(*this, ui::TextRole::Small, 0.0f, kRow1 + kRowStep, 120.0f, kRowH);
    m_feeKey->setText("Ücret:");
    m_fee = &label(*this, ui::TextRole::Normal, w - 140.0f, kRow1 + kRowStep, 140.0f - 22.0f, kRowH);
    m_fee->setWeight(render::FontWeight::Bold).setAlign(render::HAlign::Right);
    m_fee->setName("upgrade.fee");
    m_chanceKey = &label(*this, ui::TextRole::Small, 0.0f, kRow1 + 2.0f * kRowStep, 140.0f, kRowH);
    m_chanceKey->setText("Başarı şansı:");
    m_chance = &label(*this, ui::TextRole::Normal, w - 100.0f, kRow1 + 2.0f * kRowStep, 100.0f, kRowH);
    m_chance->setWeight(render::FontWeight::Bold).setAlign(render::HAlign::Right);
    m_chance->setName("upgrade.chance");
    m_fail = &label(*this, ui::TextRole::Small, 0.0f, kFailY, w, kFailH);
    m_fail->setWrap(true).setAlign(render::HAlign::Left, render::VAlign::Middle);
    m_fail->setName("upgrade.fail");

    m_button = &add<ui::Button>(std::string(world::kUpgradeButton), [this]() {
        if (m_hooks.start)
            m_hooks.start();
    });
    m_button->setFocusable(false);
    m_button->setAnchors(anchor::kTopLeft).setMargins({0.0f, kButtonY, 0.0f, 0.0f}).setSize(w, kButtonH);
    m_button->setName("upgrade.button");

    m_result = &label(*this, ui::TextRole::Normal, 0.0f, kResultY, w, kResultH);
    m_result->setWrap(true).setWeight(render::FontWeight::Bold).setAlign(render::HAlign::Center, render::VAlign::Middle);
    m_result->setName("upgrade.result");

    auto probe = std::make_unique<TextProbe>();
    probe->setName("upgrade.contents");
    probe->setSize(0.0f, 0.0f);
    m_contents = probe.get();
    addChild(std::move(probe));
    refresh();
}

const std::string& UpgradePanel::contents() const noexcept
{
    return m_contents->text();
}

Rect UpgradePanel::slotRect() const
{
    return m_slot->rect();
}

void UpgradePanel::refresh()
{
    const game::ItemCatalog* catalog = m_inventory.catalog();
    m_view = catalog != nullptr ? world::upgradePanelView(*catalog, m_inventory.view(), m_coins, m_session.slot())
                                : world::UpgradePanelView{};
    const auto& v = m_view;
    if (v.hasItem)
        m_lastIcon = v.templateId;
    const bool upgradable = v.hasItem && v.stone != game::StoneGrade::None;
    m_name->setVisible(v.hasItem);
    m_name->setText(v.name);
    m_name->setColor(Color::hex(v.nameRgb));
    m_level->setVisible(upgradable);
    m_level->setText(v.levelLine);
    m_stat->setVisible(upgradable && !v.statLine.empty());
    m_stat->setText(v.statLine);
    m_hint->setVisible(!v.hasItem);
    m_hint->setText(world::kUpgradeHintText);
    const bool rows = upgradable && v.target != 0;
    for (ui::Label* l : {m_stoneKey, m_stone, m_owned, m_feeKey, m_fee, m_chanceKey, m_chance})
        l->setVisible(rows);
    m_stone->setText(v.stoneName);
    m_stone->setColor(Color::hex(v.stone == game::StoneGrade::Usta  ? 0x6FAEF5
                                 : v.stone == game::StoneGrade::Saf ? 0x8FD06A
                                                                    : 0xEDE6D3));
    m_owned->setText(std::format("Elinde: {}", v.stonesOwned));
    m_owned->setColor(v.stonesOwned > 0 ? Color::hex(0xE9E2CF) : kRed);
    m_fee->setText(world::formatThousands(v.fee));
    m_fee->setColor(v.affordable ? kGold : kRed);
    m_chance->setText(v.chanceText);
    m_chance->setColor(v.chancePermille >= 600 ? kGood : v.chancePermille >= 300 ? kGold : kRed);
    // What a failure does; or why the item cannot be upgraded at all (not upgradable, +10).
    if (rows)
    {
        m_fail->setText(v.failText);
        m_fail->setColor(v.failDestroys ? kRed : Color::hex(0xD9CFB4));
        m_fail->setWeight(v.failDestroys ? render::FontWeight::Bold : render::FontWeight::Regular);
    }
    else
    {
        m_fail->setText(v.hasItem ? v.blockText : std::string());
        m_fail->setColor(kRed);
        m_fail->setWeight(render::FontWeight::Regular);
    }
    m_fail->setVisible(true);
    const bool busy = m_session.busy();
    m_button->setEnabled(v.canAttempt && !busy);
    m_button->setText(busy ? std::string("Bekleniyor…") : std::string(world::kUpgradeButton));

    // The result line: the last answer (a refusal or an attempt), else why the button is disabled.
    std::string result;
    Color resultColor = kRed;
    const double now = context() != nullptr ? context()->time() : 0.0;
    if (const auto& o = m_session.outcome(); o && now - o->at < kResultSeconds)
    {
        const game::ItemTemplate* t = catalog != nullptr ? catalog->find(o->templateId) : nullptr;
        result = world::upgradeResultText(o->result, t, o->level);
        resultColor = o->result == vn::UpgradeResult::Success ? kGold
                      : o->result == vn::UpgradeResult::Failed ? Color::hex(0xE8A15A)
                                                               : kRed;
    }
    else if (rows && !v.canAttempt)
        result = v.blockText;
    m_result->setText(result);
    m_result->setColor(resultColor);

    std::string all;
    for (const ui::Label* l : {m_name, m_level, m_stat, m_hint, m_stoneKey, m_stone, m_owned, m_feeKey, m_fee, m_chanceKey,
                               m_chance, m_fail, m_result})
    {
        if (!l->isVisible() || l->text().empty())
            continue;
        if (!all.empty())
            all += '\n';
        all += l->text();
    }
    if (rows)
        all += std::format("\nÜcret: {} Akçe\nBaşarı şansı: {}", world::formatThousands(v.fee), v.chanceText);
    m_contents->setText(all);
}

void UpgradePanel::update(double dt)
{
    ui::Widget::update(dt);
    refresh();
}

bool UpgradePanel::onMouseDown(const platform::InputEvent& e)
{
    // A right click on the well takes the item out of the section.
    if (e.button == platform::MouseButton::Right && m_slot->rect().contains(static_cast<float>(e.x), static_cast<float>(e.y)))
    {
        if (m_hooks.clear)
            m_hooks.clear();
    }
    return true;
}

void UpgradePanel::drawSelf(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const double now = ctx.time;
    const auto& o = m_session.outcome();
    const double age = o ? now - o->at : 1e9;

    // The well; it shakes for a moment after a failure.
    Rect well = m_slot->rect();
    if (o && o->result == vn::UpgradeResult::Failed && age < 0.5)
        well.x += std::round(static_cast<float>(std::sin(age * 48.0) * (1.0 - age / 0.5)) * ctx.pxf(5.0f));
    const float radius = ctx.pxf(5.0f);
    b.fillRoundedRect(well, radius, Color::hex(0x000000, 0.45f));
    const bool lit = m_view.hasItem && m_view.level >= world::kUpgradeHighLevel;
    b.strokeRoundedRect(well, radius, std::max(1.0f, ctx.pxf(lit ? 2.0f : 1.0f)),
                        lit ? Color::hex(world::kUpgradeHighRgb, 0.9f) : t.ornament.withAlpha(0.7f));
    if (m_view.hasItem)
    {
        const Rect inner = well.inset(ctx.px(5.0f)).snapped();
        if (const render::Texture2D* icon = m_resources.itemIcon(m_view.templateId))
            b.drawImage(*icon, inner, m_resources.itemTint(m_view.templateId));
        if (m_view.level > 0)
        {
            render::TextStyle us;
            us.color = Color::hex(m_view.level >= world::kUpgradeHighLevel ? world::kUpgradeHighRgb : 0xF2CA64);
            us.outline = true;
            us.outlineColor = Color::hex(0x000000, 0.9f);
            render::drawTextAligned(b, ctx.font(14.0f, render::FontWeight::Bold), std::format("+{}", m_view.level),
                                    well.inset(ctx.px(4.0f)), render::HAlign::Left, render::VAlign::Top, us);
        }
    }
    else
    {
        // An empty well: a faint "+" to say "put something here".
        render::TextStyle ps;
        ps.color = t.textDim.withAlpha(0.6f);
        render::drawTextAligned(b, ctx.font(26.0f, render::FontWeight::Bold), "+", well, render::HAlign::Center,
                                render::VAlign::Middle, ps);
    }

    // A gold rule between the item and its numbers.
    const float ry = std::round(r.y + ctx.px(kRuleY)) + 0.5f;
    b.drawLine({r.x, ry}, {r.right(), ry}, std::max(1.0f, ctx.pxf(1.0f)), t.ornament.withAlpha(0.3f));

    // The stone icon (tinted like the item) and the Akçe coin after the fee.
    if (m_fee->isVisible())
    {
        const Rect sr = m_stoneKey->rect();
        const float s = ctx.px(kStoneIcon);
        const Rect icon = Rect{r.x + ctx.px(44.0f), std::round(sr.centerY() - s * 0.5f), s, s}.snapped();
        if (const render::Texture2D* st = m_view.stoneTemplate != 0 ? m_resources.itemIcon(m_view.stoneTemplate) : nullptr)
            b.drawImage(*st, icon, m_resources.itemTint(m_view.stoneTemplate));
        const Rect fr = m_fee->rect();
        const float coin = ctx.px(16.0f);
        const Rect cr = Rect{std::round(r.right() - coin), std::round(fr.centerY() - coin * 0.5f), coin, coin}.snapped();
        if (const render::Texture2D* c = m_resources.uiImage(kAkceIcon))
            b.drawImage(*c, cr, Color{1.0f, 1.0f, 1.0f, m_view.affordable ? 1.0f : 0.55f});
        else
            b.fillRoundedRect(cr.inset(ctx.px(1.0f)), cr.w * 0.5f, Color::hex(0xC9A247, 0.9f));
        // A thin warning frame round the failure line when it would destroy the item.
        if (m_view.failDestroys)
        {
            const Rect fl = m_fail->rect().inset(-ctx.px(3.0f));
            b.fillRoundedRect(fl, ctx.pxf(4.0f), Color::hex(0x5A1410, 0.35f));
            b.strokeRoundedRect(fl, ctx.pxf(4.0f), std::max(1.0f, ctx.pxf(1.0f)), kRed.withAlpha(0.8f));
        }
    }
}

void UpgradePanel::drawEffect(ui::DrawContext& ctx, const Rect& well, double age) const
{
    const auto& o = m_session.outcome();
    if (!o || age < 0.0 || age > kEffectSeconds)
        return;
    auto& b = ctx.batch;
    const float k = static_cast<float>(age / kEffectSeconds); // 0 .. 1
    const float cx = well.centerX();
    const float cy = well.centerY();
    if (o->result == vn::UpgradeResult::Success)
    {
        // A gold ring that grows and fades, rays turning out of the well, the well lit.
        const float rad = well.w * (0.55f + 0.6f * k);
        const Rect ring{cx - rad, cy - rad, rad * 2.0f, rad * 2.0f};
        b.strokeRoundedRect(ring, rad, std::max(1.0f, ctx.pxf(3.0f * (1.0f - k) + 1.0f)), kGold.withAlpha(0.9f * (1.0f - k)));
        for (int i = 0; i < 10; ++i)
        {
            const float a = static_cast<float>(i) * std::numbers::pi_v<float> * 0.2f + k * 0.8f;
            const float r0 = well.w * (0.45f + 0.3f * k);
            const float r1 = r0 + well.w * (0.25f + 0.35f * k);
            b.drawLine({cx + std::cos(a) * r0, cy + std::sin(a) * r0}, {cx + std::cos(a) * r1, cy + std::sin(a) * r1},
                       std::max(1.0f, ctx.pxf(2.0f)), Color::hex(0xFFE7A3, 0.85f * (1.0f - k)));
        }
        b.fillRoundedRect(well, ctx.pxf(5.0f), kGold.withAlpha(0.35f * (1.0f - k)));
        return;
    }
    if (o->result == vn::UpgradeResult::Failed)
    {
        b.fillRoundedRect(well, ctx.pxf(5.0f), Color::hex(0x2A2A2A, 0.55f * (1.0f - k)));
        return;
    }
    if (o->result == vn::UpgradeResult::Destroyed)
    {
        // A red flash, then the item's icon in two halves drifting apart and fading.
        b.fillRoundedRect(well.inset(-ctx.px(6.0f * (1.0f - k))), ctx.pxf(6.0f), kRed.withAlpha(0.45f * (1.0f - k)));
        const render::Texture2D* icon = m_lastIcon != 0 ? m_resources.itemIcon(m_lastIcon) : nullptr;
        if (icon == nullptr)
            return;
        const Rect inner = well.inset(ctx.px(5.0f));
        const float drift = inner.w * 0.3f * k;
        const float drop = inner.h * 0.3f * k * k;
        const Color tint = m_resources.itemTint(m_lastIcon, Color{1.0f, 0.8f, 0.75f, 1.0f - k * k});
        const float half = std::round(inner.w * 0.5f);
        b.pushClip({inner.x - drift, inner.y - inner.h, half, inner.h * 3.0f});
        b.drawImage(*icon, Rect{inner.x - drift, inner.y + drop, inner.w, inner.h}, tint);
        b.popClip();
        b.pushClip({inner.x + half + drift, inner.y - inner.h, inner.w - half, inner.h * 3.0f});
        b.drawImage(*icon, Rect{inner.x + drift, inner.y + drop * 1.3f, inner.w, inner.h}, tint);
        b.popClip();
        // Cracks: three short dark lines through the centre.
        for (int i = 0; i < 3; ++i)
        {
            const float a = 1.2f + static_cast<float>(i) * 1.9f;
            b.drawLine({cx, cy}, {cx + std::cos(a) * inner.w * 0.5f, cy + std::sin(a) * inner.h * 0.5f},
                       std::max(1.0f, ctx.pxf(2.0f)), Color::hex(0x1A0806, 0.9f * (1.0f - k)));
        }
    }
}

void UpgradePanel::draw(ui::DrawContext& ctx)
{
    if (!isVisible())
        return;
    ui::Widget::draw(ctx);
    if (const auto& o = m_session.outcome())
    {
        // Inside the shop window (the ring may grow past the section, never past the window's frame).
        const Rect clip = parent() != nullptr ? parent()->rect().inset(ctx.px(4.0f)) : rect();
        ctx.batch.pushClip(clip);
        drawEffect(ctx, m_slot->rect(), ctx.time - o->at);
        ctx.batch.popClip();
    }
}

} // namespace vo::client::scenes
