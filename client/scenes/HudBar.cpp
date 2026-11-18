#include "client/scenes/HudBar.h"

#include "client/render/TextRenderer.h"
#include "client/render/Texture2D.h"
#include "client/ui/Button.h"
#include "client/ui/DrawContext.h"
#include "client/ui/Label.h"
#include "client/ui/UiContext.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace vo::client::scenes {

namespace anchor = ui::anchor;

namespace {

// Art slot geometry (docs/SKILL_ART.md §2): 64 px piece, icon window (6, 6)-(58, 58).
Rect iconWindow(const Rect& r)
{
    const float k = r.w / 64.0f;
    return Rect{r.x + 6.0f * k, r.y + 6.0f * k, 52.0f * k, 52.0f * k}.snapped();
}

// A Label that is never drawn: carries text for automation (expecttext).
class TextProbe final : public ui::Label
{
public:
    TextProbe() : ui::Label(std::string(), ui::TextRole::Small) {}
    void drawSelf(ui::DrawContext&) override {}
};

// "Tuşlar" window metrics (logical, before the fit scale).
constexpr float kPadX = 16.0f;
constexpr float kPadTop = 10.0f;
constexpr float kPadBottom = 12.0f;
constexpr float kTitleBar = 38.0f;
constexpr float kHeadH = 26.0f;
constexpr float kRowH = 23.0f;
constexpr float kGroupGap = 8.0f;
constexpr float kColGap = 30.0f;
constexpr float kTextGap = 12.0f;
constexpr float kCapH = 19.0f;
constexpr float kCapPad = 6.0f;
constexpr float kCapGap = 4.0f;
constexpr float kFooterH = 24.0f;
constexpr float kCapFont = 12.0f;
constexpr float kRowFont = 13.5f;
constexpr float kHeadFont = 14.5f;

const Color kXpTop = Color::hex(0xB98CF0);
const Color kXpBottom = Color::hex(0x6A3FB0);

} // namespace

// ---------------------------------------------------------------------------------------------
// Key badge
// ---------------------------------------------------------------------------------------------

void drawKeyBadge(ui::DrawContext& ctx, const SkillUiHost& art, Vec2 corner, float height, std::string_view key)
{
    auto& b = ctx.batch;
    const float h = std::round(height);
    const render::Font& f = ctx.fonts.get(render::FontWeight::Bold, std::max(10.0f, std::round(h * 0.66f)));
    const float tw = render::measureText(f, key);
    const float w = std::max(h, std::round(tw + h * 0.55f));
    const Rect br = Rect{corner.x, corner.y, w, h}.snapped();
    if (const render::Texture2D* kb = art.skillImage("ui/skillbar/key_badge.png"))
    {
        if (w <= h + 0.5f)
            b.drawImage(*kb, br);
        else
        {
            const float d = std::round(h * 0.34f);
            b.drawNineSlice(*kb, br, Insets{10.0f, 10.0f, 10.0f, 10.0f}, Insets{d, d, d, d});
        }
    }
    else
    {
        b.fillRoundedRect(br, h * 0.25f, Color::hex(0x101317, 0.92f));
        b.strokeRoundedRect(br, h * 0.25f, std::max(1.0f, ctx.pxf(1.0f)), Color::hex(0x9C7D3C));
    }
    render::TextStyle ks;
    ks.color = ctx.theme.text;
    render::drawTextAligned(b, f, key, br, render::HAlign::Center, render::VAlign::Middle, ks);
}

// ---------------------------------------------------------------------------------------------
// HudIconButton
// ---------------------------------------------------------------------------------------------

HudIconButton::HudIconButton(world::HudButton kind, const SkillUiHost& art, Hooks hooks)
    : m_kind(kind), m_art(art), m_hooks(std::move(hooks))
{
    setName(std::string(world::hudButtonInfo(kind).widget));
}

bool HudIconButton::markerShown() const
{
    return isVisible() && m_hooks.points && m_hooks.points() > 0;
}

bool HudIconButton::onMouseDown(const platform::InputEvent& e)
{
    if (e.button == platform::MouseButton::Left)
        m_pressed = true;
    return true; // never reaches the world (no walk, no camera drag)
}

bool HudIconButton::onMouseUp(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return true;
    const bool was = m_pressed;
    m_pressed = false;
    if (was && rect().contains(static_cast<float>(e.x), static_cast<float>(e.y)) && m_hooks.click)
        m_hooks.click();
    return true;
}

void HudIconButton::drawSelf(ui::DrawContext& ctx)
{
    auto& b = ctx.batch;
    const Rect r = rect();
    const float k = r.w / 64.0f;
    const Rect win = iconWindow(r);
    const world::HudButtonState st = m_hooks.state ? m_hooks.state() : world::HudButtonState{};
    const bool on = lit();
    const bool dim = m_hooks.dim && m_hooks.dim();
    const bool hot = isHovered();
    const bool down = m_pressed && hot;

    if (const render::Texture2D* back = m_art.skillImage("ui/skillbar/slot_back.png"))
        b.drawImage(*back, r);
    else
        b.fillRoundedRect(r, 7.0f * k, Color::hex(0x0C0F13, 0.9f));
    if (on) // a warm lining behind the glyph while the toggle / window is on
        b.fillRoundedRect(win, 7.0f * k, Color::hex(0xC98A2A, 0.28f));
    const Color tint = dim ? Color{0.55f, 0.55f, 0.57f, 1.0f} : colors::kWhite;
    const Rect glyph = down ? Rect{win.x, win.y + std::max(1.0f, std::round(k)), win.w, win.h} : win;
    if (const render::Texture2D* icon = m_art.skillImage(world::hudButtonIcon(m_kind, st)))
        b.drawImage(*icon, glyph, tint);
    else
    {
        render::TextStyle s;
        s.color = ctx.theme.text;
        render::drawTextAligned(b, ctx.fonts.get(render::FontWeight::Bold, std::round(win.h * 0.5f)),
                                world::hudButtonInfo(m_kind).title.substr(0, 1), win, render::HAlign::Center,
                                render::VAlign::Middle, s);
    }
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ctx.time) * 4.5f);
    const render::Texture2D* flash = m_art.skillImage("ui/skillbar/slot_ready_flash.png");
    if (flash != nullptr && (on || (m_hooks.attention && m_hooks.attention())))
        b.drawImage(*flash, r, Color{1.0f, 1.0f, 1.0f, on ? 0.7f : 0.25f + 0.5f * pulse});
    const char* frame = down ? "ui/skillbar/slot_frame_pressed.png"
                             : (hot || on ? "ui/skillbar/slot_frame_hover.png" : "ui/skillbar/slot_frame_normal.png");
    if (const render::Texture2D* f = m_art.skillImage(frame))
        b.drawImage(*f, r);
    else
        b.strokeRoundedRect(r, 7.0f * k, std::max(1.0f, ctx.pxf(1.0f)), hot || on ? ctx.theme.buttonBorderHover : ctx.theme.buttonBorder);

    // Key badge over the top-left corner (as the skill slots).
    // (A longer key, "Esc", hangs out less to the left: it would touch the neighbouring button.)
    const float badge = std::round(std::max(15.0f, r.w * 0.40f));
    const std::string_view key = world::hudButtonInfo(m_kind).key;
    const float hang = key.size() > 1 ? std::round(badge * 0.05f) : std::round(badge * 0.14f);
    drawKeyBadge(ctx, m_art, {r.x - hang, r.y - std::round(badge * 0.14f)}, badge, key);

    // Unspent points: a pulsing gold disc with the number over the top-right corner.
    const std::uint16_t n = m_hooks.points ? m_hooks.points() : 0;
    if (n > 0)
    {
        const float d = std::round(std::max(16.0f, r.w * 0.5f));
        // Inside the button's own top-right corner: the next button's key badge stays clear.
        const Vec2 c{std::round(r.right() - d * 0.5f), std::round(r.y + d * 0.05f)};
        for (int i = 3; i >= 1; --i)
        {
            const float rr = d * 0.5f + ctx.pxf(1.5f + 2.5f * static_cast<float>(i)) * (0.8f + 0.4f * pulse);
            b.fillRoundedRect({c.x - rr, c.y - rr, 2.0f * rr, 2.0f * rr}, rr,
                              Color::hex(0xF5C84C, (0.10f + 0.06f * pulse) / static_cast<float>(i)));
        }
        const Rect disc = Rect{c.x - d * 0.5f, c.y - d * 0.5f, d, d}.snapped();
        if (const render::Texture2D* marker = m_art.skillImage("ui/skillbar/skill_point_marker_blank.png"))
            b.drawImage(*marker, disc);
        else
        {
            b.fillRoundedRectGradient(disc, d * 0.5f, Color::hex(0xFFE08A), Color::hex(0xC98A1E));
            b.strokeRoundedRect(disc, d * 0.5f, std::max(1.0f, ctx.pxf(1.0f)), Color::hex(0x5A3A08));
        }
        render::TextStyle s;
        s.color = Color::hex(0x2A1A02);
        render::drawTextAligned(b, ctx.fonts.get(render::FontWeight::Bold, std::round(d * (n >= 10 ? 0.5f : 0.6f))),
                                std::to_string(n), disc, render::HAlign::Center, render::VAlign::Middle, s);
    }
}

// ---------------------------------------------------------------------------------------------
// HudBarFrame
// ---------------------------------------------------------------------------------------------

HudBarFrame::HudBarFrame(std::function<Xp()> xp) : m_xp(std::move(xp))
{
    setName("hud.bar");
}

Rect HudBarFrame::xpRect() const
{
    const Rect r = rect();
    const float s = r.w / std::max(m_layout.frame.w, 1.0f); // physical per logical
    return Rect{r.x + m_layout.xp.x * s, r.y + m_layout.xp.y * s, m_layout.xp.w * s, m_layout.xp.h * s}.snapped();
}

void HudBarFrame::drawSelf(ui::DrawContext& ctx)
{
    auto& b = ctx.batch;
    const ui::Theme& t = ctx.theme;
    const Rect r = rect();
    const float s = r.w / std::max(m_layout.frame.w, 1.0f);
    const float k = m_layout.k;
    const float radius = std::round(8.0f * s * k);
    const float line = std::max(1.0f, std::round(s));

    // Shadow, body, bronze border and a faint inner bevel (the windows' look, a little darker).
    const float sh = std::round(5.0f * s);
    b.fillRoundedRect({r.x - sh * 0.5f, r.y + sh * 0.3f, r.w + sh, r.h + sh}, radius + sh, Color::hex(0x000000, 0.28f));
    b.fillRoundedRectGradient(r, radius, Color::hex(0x2A313A, 0.95f), Color::hex(0x101419, 0.97f));
    b.strokeRoundedRect(r, radius, line, t.panelBorder);
    const float inner = std::round(3.0f * s);
    b.strokeRoundedRect(r.inset(inner), std::max(0.0f, radius - inner * 0.5f), line, t.panelHighlight);

    // A gold rule along the top edge with a diamond in the middle and at both ends.
    const float topY = r.y + 0.5f * line;
    const float cx = std::round(r.centerX());
    const float half = r.w * 0.5f - radius - std::round(10.0f * s);
    b.drawLine({cx - half, topY}, {cx - std::round(10.0f * s), topY}, line, t.ornament.withAlpha(0.7f));
    b.drawLine({cx + std::round(10.0f * s), topY}, {cx + half, topY}, line, t.ornament.withAlpha(0.7f));
    b.fillDiamond({cx, topY}, 5.0f * s, t.ornament);
    b.fillDiamond({cx, topY}, 2.2f * s, Color::hex(0x2A2214));
    b.fillDiamond({cx - half, topY}, 2.8f * s, t.ornament);
    b.fillDiamond({cx + half, topY}, 2.8f * s, t.ornament);

    // Group separators: a thin gold rule fading at both ends with a diamond in the middle.
    const Rect slots = m_layout.slots;
    for (const float sx : m_layout.separators)
    {
        const float x = std::round(r.x + sx * s) + 0.5f;
        const float y0 = r.y + (slots.y + 4.0f * k) * s;
        const float y1 = r.y + (slots.bottom() - 4.0f * k) * s;
        const float ym = std::round((y0 + y1) * 0.5f);
        b.fillRectGradient({x - line * 0.5f, y0, line, ym - y0}, t.ornament.withAlpha(0.0f), t.ornament.withAlpha(0.55f));
        b.fillRectGradient({x - line * 0.5f, ym, line, y1 - ym}, t.ornament.withAlpha(0.55f), t.ornament.withAlpha(0.0f));
        b.fillDiamond({x, ym}, 3.5f * s * std::max(k, 0.8f), t.ornament);
    }

    // Corner brackets (the windows' ornament).
    {
        const float len = std::round(9.0f * s * k);
        const float o = std::round(5.0f * s);
        const float w = std::max(1.0f, 1.5f * s);
        const Color c = t.ornament.withAlpha(0.8f);
        const float l = r.x + o;
        const float tp = r.y + o;
        const float rt = r.right() - o;
        const float bt = r.bottom() - o;
        b.drawLine({l, tp}, {l + len, tp}, w, c);
        b.drawLine({l, tp}, {l, tp + len}, w, c);
        b.drawLine({rt, tp}, {rt - len, tp}, w, c);
        b.drawLine({rt, tp}, {rt, tp + len}, w, c);
        b.drawLine({l, bt}, {l + len, bt}, w, c);
        b.drawLine({l, bt}, {l, bt - len}, w, c);
        b.drawLine({rt, bt}, {rt - len, bt}, w, c);
        b.drawLine({rt, bt}, {rt, bt - len}, w, c);
    }

    // Experience bar: inset track, violet fill, tenth ticks, the level and numbers inside.
    const Xp xp = m_xp ? m_xp() : Xp{};
    const Rect bar = xpRect();
    const float br = std::min(bar.h * 0.5f, std::round(4.0f * s));
    b.fillRoundedRect(bar, br, Color::hex(0x07090C, 0.92f));
    const float f = world::xpFraction(xp.xp, xp.toNext);
    Rect fill = bar;
    fill.w = std::round(bar.w * f);
    if (fill.w >= 1.0f)
    {
        b.fillRoundedRectGradient(fill, std::min(br, fill.w * 0.5f), kXpTop, kXpBottom);
        b.fillRect({fill.x + br, fill.y + line, std::max(0.0f, fill.w - 2.0f * br), line}, Color::hex(0xFFFFFF, 0.22f));
    }
    for (int i = 1; i < 10; ++i)
    {
        const float x = std::round(bar.x + bar.w * static_cast<float>(i) / 10.0f);
        b.fillRect({x, bar.y + std::round(bar.h * 0.25f), line, std::round(bar.h * 0.5f)}, Color::hex(0x000000, 0.45f));
    }
    b.strokeRoundedRect(bar, br, line, Color::hex(0x6E5A33, 0.95f));
    const render::Font& xf = ctx.fonts.get(render::FontWeight::Bold, std::max(10.0f, std::round(bar.h * 0.8f)));
    std::string text = world::xpBarText(xp.level, xp.xp, xp.toNext);
    if (render::measureText(xf, text) > bar.w - std::round(12.0f * s))
        text = world::xpBarShortText(xp.level, xp.xp, xp.toNext);
    render::TextStyle ts;
    ts.color = Color::hex(0xF6F0DE);
    ts.outline = true;
    ts.outlineColor = Color::hex(0x000000, 0.85f);
    render::drawTextAligned(b, xf, text, bar, render::HAlign::Center, render::VAlign::Middle, ts);
}

// ---------------------------------------------------------------------------------------------
// ControlsWindow
// ---------------------------------------------------------------------------------------------

ControlsWindow::ControlsWindow(const SkillUiHost& art, std::function<void()> onClose)
    : ui::Panel(ui::Panel::Style::Window, "Tuşlar"), m_art(art)
{
    setName("controls.window");
    setPadding({kPadX, kPadTop, kPadX, kPadBottom});
    setSize(640.0f, 420.0f);
    auto& close = add<ui::Button>("X", std::move(onClose));
    close.setFocusable(false);
    close.setAnchors(anchor::kTopRight).setMargins({0.0f, -34.0f, -6.0f, 0.0f}).setSize(28.0f, 28.0f);
    close.setName("controls.close");
    auto probe = std::make_unique<TextProbe>();
    probe->setName("controls.text");
    probe->setSize(0.0f, 0.0f);
    probe->setText(world::controlsPlainText());
    addChild(std::move(probe));
}

void ControlsWindow::measure()
{
    ui::UiContext* ui = context();
    if (ui == nullptr)
        return;
    const float s = std::max(ui->scale(), 0.01f);
    if (m_measuredScale == s)
        return;
    m_measuredScale = s;
    // Measured at the full scale in physical pixels, kept in logical units.
    const render::Font& capF = ui->fonts().get(render::FontWeight::Bold, kCapFont * s);
    const render::Font& rowF = ui->fonts().get(render::FontWeight::Regular, kRowFont * s);
    const render::Font& headF = ui->fonts().get(render::FontWeight::Bold, kHeadFont * s);
    m_cols = {};
    for (const world::ControlGroup& g : world::controlsReference())
    {
        Column& c = m_cols[static_cast<std::size_t>(std::clamp(g.column, 0, 1))];
        if (c.h > 0.0f)
            c.h += kGroupGap;
        c.h += kHeadH + kRowH * static_cast<float>(g.rows.size());
        c.textW = std::max(c.textW, render::measureText(headF, g.title) / s);
        for (const world::ControlRow& row : g.rows)
        {
            float kw = 0.0f;
            for (const std::string_view key : row.keys)
                kw += std::max(kCapH, render::measureText(capF, key) / s + 2.0f * kCapPad) + kCapGap;
            c.keyW = std::max(c.keyW, kw - kCapGap);
            c.textW = std::max(c.textW, render::measureText(rowF, row.text) / s);
        }
    }
}

ControlsWindow::Drawn ControlsWindow::measureDrawn(float k) const
{
    Drawn d;
    ui::UiContext* ui = context();
    if (ui == nullptr)
        return d;
    const float s = std::max(ui->scale(), 0.01f);
    const auto L = [&](float logical) { return std::round(logical * k * s); };
    render::FontLibrary& fonts = ui->fonts();
    const render::Font& capF = fonts.get(render::FontWeight::Bold, std::round(kCapFont * k * s));
    const render::Font& rowF = fonts.get(render::FontWeight::Regular, std::round(kRowFont * k * s));
    const render::Font& headF = fonts.get(render::FontWeight::Bold, std::round(kHeadFont * k * s));
    for (const world::ControlGroup& g : world::controlsReference())
    {
        const std::size_t ci = static_cast<std::size_t>(std::clamp(g.column, 0, 1));
        d.textPx[ci] = std::max(d.textPx[ci], render::measureText(headF, g.title));
        for (const world::ControlRow& row : g.rows)
        {
            float kw = 0.0f;
            for (std::size_t i = 0; i < row.keys.size(); ++i)
            {
                kw += std::max(L(kCapH), std::round(render::measureText(capF, row.keys[i]) + L(2.0f * kCapPad)));
                if (i + 1 < row.keys.size())
                    kw += L(kCapGap);
            }
            d.keyPx[ci] = std::max(d.keyPx[ci], kw);
            d.textPx[ci] = std::max(d.textPx[ci], std::ceil(render::measureText(rowF, row.text)));
        }
    }
    return d;
}

float ControlsWindow::contentPx(const Drawn& d, float k) const
{
    const float s = context() != nullptr ? std::max(context()->scale(), 0.01f) : 1.0f;
    const auto L = [&](float logical) { return std::round(logical * k * s); };
    return d.keyPx[0] + L(kTextGap) + d.textPx[0] + L(kColGap) + d.keyPx[1] + L(kTextGap) + d.textPx[1];
}

bool ControlsWindow::textsFit(std::string& error) const
{
    if (context() == nullptr || !isVisible())
        return true;
    const Drawn d = measureDrawn(m_k);
    const float need = contentPx(d, m_k);
    const float have = contentRect().w;
    if (need > have + 0.5f)
    {
        error = std::format("Tuşlar: content {:.0f} px in {:.0f} px (k {:.2f})", need, have, m_k);
        return false;
    }
    return true;
}

Vec2 ControlsWindow::fit(float maxW, float maxH)
{
    measure();
    const float contentW = m_cols[0].keyW + kTextGap + m_cols[0].textW + kColGap + m_cols[1].keyW + kTextGap + m_cols[1].textW;
    const float contentH = std::max(m_cols[0].h, m_cols[1].h) + kFooterH;
    const float frameW = 2.0f * kPadX;
    const float frameH = kTitleBar + kPadTop + kPadBottom;
    m_k = std::min({1.0f, (maxW - frameW) / std::max(contentW, 1.0f), (maxH - frameH) / std::max(contentH, 1.0f)});
    m_k = std::max(m_k, 0.6f);
    // fix1: the width from what is really drawn at k (rounded font sizes); shrink a little more while the
    // drawn content does not fit the room.
    float drawnW = contentW * m_k;
    if (context() != nullptr)
    {
        const float s = std::max(context()->scale(), 0.01f);
        for (int i = 0; i < 40; ++i)
        {
            drawnW = contentPx(measureDrawn(m_k), m_k) / s;
            if (frameW + drawnW <= maxW + 0.01f || m_k <= 0.6f)
                break;
            m_k = std::max(0.6f, m_k - 0.01f);
        }
    }
    const Vec2 size{std::ceil(frameW + drawnW + 1.0f), std::ceil(frameH + contentH * m_k)};
    if (layoutSpec().size.x != size.x || layoutSpec().size.y != size.y)
        setSize(size.x, size.y);
    return size;
}

void ControlsWindow::drawSelf(ui::DrawContext& ctx)
{
    // An opaque backing: it opens over the other windows, whose text must not show through.
    ctx.batch.fillRoundedRect(rect(), ctx.pxf(ctx.theme.cornerRadius), Color::hex(0x161B21));
    ui::Panel::drawSelf(ctx);
    measure();
    auto& b = ctx.batch;
    const ui::Theme& t = ctx.theme;
    const Rect area = contentRect();
    const float k = m_k;
    const auto L = [&](float logical) { return std::round(logical * k * ctx.scale); };
    const render::Font& capF = ctx.fonts.get(render::FontWeight::Bold, std::round(kCapFont * k * ctx.scale));
    const render::Font& rowF = ctx.fonts.get(render::FontWeight::Regular, std::round(kRowFont * k * ctx.scale));
    const render::Font& headF = ctx.fonts.get(render::FontWeight::Bold, std::round(kHeadFont * k * ctx.scale));
    const float line = std::max(1.0f, std::round(ctx.scale));

    // fix1: columns from the widths really drawn at k (no text runs into the gutter when shrunk).
    const Drawn drawn = measureDrawn(k);
    std::array<float, 2> colX{area.x, area.x + drawn.keyPx[0] + L(kTextGap) + drawn.textPx[0] + L(kColGap)};
    std::array<float, 2> pen{area.y, area.y};
    // The gutter between the columns: a faint vertical rule.
    {
        const float gx = std::round(colX[1] - L(kColGap * 0.5f)) + 0.5f;
        const float h = L(std::max(m_cols[0].h, m_cols[1].h));
        b.fillRectGradient({gx, area.y, line, h * 0.5f}, t.ornament.withAlpha(0.0f), t.ornament.withAlpha(0.35f));
        b.fillRectGradient({gx, area.y + h * 0.5f, line, h * 0.5f}, t.ornament.withAlpha(0.35f), t.ornament.withAlpha(0.0f));
    }
    for (const world::ControlGroup& g : world::controlsReference())
    {
        const std::size_t ci = static_cast<std::size_t>(std::clamp(g.column, 0, 1));
        float& y = pen[ci];
        if (y > area.y)
            y += L(kGroupGap);
        const float x = colX[ci];
        const float colW = drawn.keyPx[ci] + L(kTextGap) + drawn.textPx[ci];
        // Heading: gold, with a rule to the column's end.
        render::TextStyle hs;
        hs.color = t.textTitle;
        hs.shadow = true;
        const float hw = render::drawText(b, headF, g.title, {x, std::round(y + (L(kHeadH) - headF.lineHeight()) * 0.5f)}, hs);
        const float ry = std::round(y + L(kHeadH) * 0.5f) + 0.5f;
        b.drawLine({x + hw + L(8.0f), ry}, {x + colW, ry}, line, t.ornament.withAlpha(0.45f));
        b.fillDiamond({x + colW, ry}, 2.5f * ctx.scale, t.ornament.withAlpha(0.8f));
        y += L(kHeadH);
        for (const world::ControlRow& row : g.rows)
        {
            const float capH = L(kCapH);
            const float capY = std::round(y + (L(kRowH) - capH) * 0.5f);
            float cx = x;
            for (std::size_t i = 0; i < row.keys.size(); ++i)
            {
                const std::string_view key = row.keys[i];
                const float cw = std::max(capH, std::round(render::measureText(capF, key) + L(2.0f * kCapPad)));
                const Rect cap{cx, capY, cw, capH};
                // A key cap: raised plate, darker base edge, bronze rim.
                const float cr = std::round(4.0f * k * ctx.scale);
                b.fillRoundedRect({cap.x, cap.y + line, cap.w, cap.h}, cr, Color::hex(0x050608, 0.9f));
                b.fillRoundedRectGradient(cap, cr, Color::hex(0x4A4232), Color::hex(0x221E16));
                b.strokeRoundedRect(cap, cr, line, Color::hex(0x9C7D3C, 0.95f));
                b.fillRect({cap.x + cr, cap.y + line, std::max(0.0f, cap.w - 2.0f * cr), line}, Color::hex(0xFFFFFF, 0.14f));
                render::TextStyle ks;
                ks.color = Color::hex(0xF3E7C6);
                render::drawTextAligned(b, capF, key, cap, render::HAlign::Center, render::VAlign::Middle, ks);
                cx += cw + L(kCapGap);
            }
            render::TextStyle rs;
            rs.color = t.text;
            rs.shadow = true;
            (void)render::drawText(b, rowF, row.text,
                                   {x + drawn.keyPx[ci] + L(kTextGap), std::round(y + (L(kRowH) - rowF.lineHeight()) * 0.5f)}, rs);
            y += L(kRowH);
        }
    }
    // Footer: how to close it.
    const float fy = area.y + L(std::max(m_cols[0].h, m_cols[1].h));
    render::TextStyle fs;
    fs.color = t.textDim;
    fs.shadow = true;
    render::drawTextAligned(b, rowF, "Kapatmak için Esc ya da H", {area.x, fy, area.w, L(kFooterH)}, render::HAlign::Center,
                            render::VAlign::Bottom, fs);
}

} // namespace vo::client::scenes
