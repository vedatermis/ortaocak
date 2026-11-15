#include "client/scenes/SceneWidgets.h"

#include "client/render/TextRenderer.h"
#include "client/scenes/SceneResources.h"
#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"
#include "shared/game/Character.h"
#include "shared/game/Zones.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace vo::client::scenes {

namespace {

constexpr std::uint8_t kAydor = static_cast<std::uint8_t>(game::Race::Aydor);

render::TextStyle shadowed(const Color& color, const ui::DrawContext& ctx)
{
    render::TextStyle s;
    s.color = color;
    s.shadow = true;
    s.shadowColor = ctx.theme.textShadow;
    s.shadowOffset = std::max(1.0f, std::round(ctx.scale));
    return s;
}

void fillCircle(render::SpriteBatch& b, float cx, float cy, float r, const Color& c)
{
    b.fillRoundedRect({cx - r, cy - r, 2.0f * r, 2.0f * r}, r, c);
}

// Tick mark centred at (cx, cy), `size` = its width (the UI font has no check mark glyph).
void drawCheck(render::SpriteBatch& b, float cx, float cy, float size, float width, const Color& c)
{
    const Vec2 a{cx - size * 0.45f, cy};
    const Vec2 m{cx - size * 0.12f, cy + size * 0.32f};
    const Vec2 e{cx + size * 0.48f, cy - size * 0.36f};
    b.drawLine(a, m, width, c, true);
    b.drawLine(m, e, width, c, true);
}

void drawCross(render::SpriteBatch& b, float cx, float cy, float size, float width, const Color& c)
{
    const float h = size * 0.36f;
    b.drawLine({cx - h, cy - h}, {cx + h, cy + h}, width, c, true);
    b.drawLine({cx - h, cy + h}, {cx + h, cy - h}, width, c, true);
}

} // namespace

// ---- Spinner ----

Spinner::Spinner()
{
    setMouseTransparent(true);
}

void Spinner::drawSelf(ui::DrawContext& ctx)
{
    const Rect r = rect();
    const float size = std::min(r.w, r.h);
    if (size <= 0.0f)
        return;
    constexpr int kDots = 8;
    const float ring = size * 0.36f;
    const float dot = std::max(1.0f, size * 0.09f);
    const int head = static_cast<int>(std::fmod(ctx.time * 10.0, static_cast<double>(kDots)));
    for (int i = 0; i < kDots; ++i)
    {
        const float a = static_cast<float>(i) / static_cast<float>(kDots) * 2.0f * std::numbers::pi_v<float>;
        const int age = (head - i + kDots) % kDots; // 0 = brightest
        const float alpha = 1.0f - static_cast<float>(age) / static_cast<float>(kDots) * 0.85f;
        fillCircle(ctx.batch, r.centerX() + std::sin(a) * ring, r.centerY() - std::cos(a) * ring, dot,
                   ctx.theme.accentBright.withAlpha(alpha));
    }
}

// ---- ChevronButton ----

ChevronButton::ChevronButton(Direction direction, std::function<void()> onClick)
    : ui::Button({}, std::move(onClick)), m_direction(direction)
{
}

void ChevronButton::drawSelf(ui::DrawContext& ctx)
{
    ui::Button::drawSelf(ctx);
    const Rect r = rect();
    const float s = std::min(r.w, r.h) * 0.22f;
    const float cx = std::round(r.centerX());
    const float cy = std::round(r.centerY());
    const float dir = m_direction == Direction::Left ? -1.0f : 1.0f;
    const Color c = !isEnabled() ? ctx.theme.textDisabled : (isHovered() ? ctx.theme.accentBright : ctx.theme.buttonText);
    const float w = std::max(1.5f, ctx.pxf(2.2f));
    ctx.batch.drawLine({cx - dir * s * 0.5f, cy - s}, {cx + dir * s * 0.5f, cy}, w, c, true);
    ctx.batch.drawLine({cx + dir * s * 0.5f, cy}, {cx - dir * s * 0.5f, cy + s}, w, c, true);
}

// ---- ToggleButton ----

ToggleButton::ToggleButton(std::string text, std::function<void()> onClick)
    : ui::Button(std::move(text), std::move(onClick))
{
}

void ToggleButton::drawSelf(ui::DrawContext& ctx)
{
    const Rect r = rect();
    const float radius = ctx.pxf(ctx.theme.cornerRadius);
    if (m_selected)
        ctx.batch.fillRoundedRect(r.inset(-ctx.px(2.0f)), radius + ctx.px(2.0f), ctx.theme.accent.withAlpha(0.35f));
    ui::Button::drawSelf(ctx);
    if (m_selected)
        ctx.batch.strokeRoundedRect(r, radius, std::max(1.0f, ctx.pxf(2.0f)), ctx.theme.accentBright);
}

// ---- SelectableCard ----

SelectableCard::SelectableCard()
{
    setFocusable(true);
}

bool SelectableCard::onMouseDown(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    m_pressed = true;
    m_activateOnRelease = e.clickCount >= 2;
    if (m_onSelect)
        m_onSelect();
    return true;
}

bool SelectableCard::onMouseUp(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left || !m_pressed)
        return false;
    m_pressed = false;
    const bool inside = rect().contains(static_cast<float>(e.x), static_cast<float>(e.y));
    if (m_activateOnRelease && inside && m_onActivate)
        m_onActivate();
    m_activateOnRelease = false;
    return true;
}

bool SelectableCard::onKeyDown(const platform::InputEvent& e)
{
    if ((e.key == platform::keys::kEnter || e.key == platform::keys::kSpace) && !e.repeat && e.mods == 0)
    {
        if (e.key == platform::keys::kSpace)
        {
            if (m_onSelect)
                m_onSelect();
        }
        else if (m_onActivate)
        {
            m_onActivate();
        }
        return true;
    }
    return false;
}

void SelectableCard::drawFrame(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const float radius = ctx.pxf(t.cornerRadius + 1.0f);
    const bool hovered = isHovered() && isEnabled();
    if (isFocused())
    {
        const float ring = ctx.px(3.0f);
        b.strokeRoundedRect(r.inset(-ring), radius + ring, std::max(1.0f, ctx.pxf(t.focusRingWidth)),
                            t.focusRing.withAlpha(0.8f));
    }
    if (m_selected)
    {
        b.fillRoundedRect(r.inset(-ctx.px(2.0f)), radius + ctx.px(2.0f), t.accent.withAlpha(0.22f));
        b.fillRoundedRectGradient(r, radius, Color::hex(0x41371F, 0.97f), Color::hex(0x231E13, 0.97f));
    }
    else if (hovered)
    {
        b.fillRoundedRectGradient(r, radius, Color::hex(0x2F3843, 0.96f), Color::hex(0x1C2128, 0.96f));
    }
    else
    {
        b.fillRoundedRectGradient(r, radius, Color::hex(0x252C35, 0.94f), Color::hex(0x161A20, 0.94f));
    }
    const Color border = m_selected ? t.accentBright : (hovered ? t.buttonBorderHover : t.insetBorder);
    b.strokeRoundedRect(r, radius, std::max(1.0f, ctx.pxf(m_selected ? 2.0f : 1.0f)), border);
}

void SelectableCard::drawSelf(ui::DrawContext& ctx)
{
    drawFrame(ctx);
}

// ---- CharacterSlotCard ----

CharacterSlotCard::CharacterSlotCard() = default;

void CharacterSlotCard::setCharacter(const vo::net::CharSummary* character)
{
    if (character != nullptr)
        m_character = *character;
    else
        m_character.reset();
}

std::string CharacterSlotCard::displayText() const
{
    if (!m_character)
        return "Boş Slot — Karakter Yarat";
    return std::format("{} · {} · Seviye {} · {}", m_character->name,
                       describeCharacter(m_character->race, m_character->charClass, 0), m_character->level,
                       game::zoneDisplayName(m_character->zoneId));
}

void CharacterSlotCard::drawSelf(ui::DrawContext& ctx)
{
    drawFrame(ctx);
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const float pad = ctx.px(12.0f);
    const float badge = std::min(r.h - 2.0f * pad, ctx.px(52.0f));
    const float bx = r.x + pad + badge * 0.5f;
    const float by = r.centerY();
    const Rect text{r.x + pad * 2.0f + badge, r.y + pad * 0.5f, r.w - pad * 3.0f - badge, r.h - pad};

    if (!m_character)
    {
        // Dashed-feel empty badge with a plus.
        b.strokeRoundedRect({bx - badge * 0.5f, by - badge * 0.5f, badge, badge}, badge * 0.5f,
                            std::max(1.0f, ctx.pxf(1.5f)), t.textDim.withAlpha(0.6f));
        const float arm = badge * 0.22f;
        const float w = std::max(1.5f, ctx.pxf(2.0f));
        b.drawLine({bx - arm, by}, {bx + arm, by}, w, t.textDim, true);
        b.drawLine({bx, by - arm}, {bx, by + arm}, w, t.textDim, true);
        render::drawTextAligned(b, ctx.font(ui::TextRole::Large), "Boş Slot — Karakter Yarat", text,
                                render::HAlign::Left, render::VAlign::Middle,
                                shadowed(isSelected() ? t.text : t.textDim, ctx));
        return;
    }

    const vo::net::CharSummary& c = *m_character;
    const bool aydor = c.race == kAydor;
    fillCircle(b, bx, by, badge * 0.5f + ctx.px(1.5f), t.ornament.withAlpha(0.85f));
    fillCircle(b, bx, by, badge * 0.5f, aydor ? Color::hex(0x2E4A7A) : Color::hex(0x7A3A1E));
    const char* initial = aydor ? "A" : (c.race == static_cast<std::uint8_t>(game::Race::Kargan) ? "K" : "?");
    render::drawTextAligned(b, ctx.font(22.0f, render::FontWeight::Bold), initial,
                            {bx - badge * 0.5f, by - badge * 0.5f, badge, badge}, render::HAlign::Center,
                            render::VAlign::Middle, shadowed(t.accentBright, ctx));

    const render::Font& nameFont = ctx.font(ui::TextRole::Large, render::FontWeight::Bold);
    const render::Font& infoFont = ctx.font(ui::TextRole::Normal);
    const float gap = ctx.px(3.0f);
    const float blockH = nameFont.lineHeight() + gap + infoFont.lineHeight();
    const float y0 = std::round(text.centerY() - blockH * 0.5f);
    const std::string level = std::format("Seviye {}", c.level);
    const float levelW = render::measureText(infoFont, level);
    render::drawTextAligned(b, nameFont, c.name, {text.x, y0, text.w - levelW - pad, nameFont.lineHeight()},
                            render::HAlign::Left, render::VAlign::Top,
                            shadowed(isSelected() ? t.textTitle : t.text, ctx));
    render::drawTextAligned(b, infoFont, level, {text.x, y0, text.w, nameFont.lineHeight()}, render::HAlign::Right,
                            render::VAlign::Middle, shadowed(t.textDim, ctx));
    const std::string info = c.zoneId != 0 ? describeCharacter(c.race, c.charClass, 0) + " · " + game::zoneDisplayName(c.zoneId)
                                           : describeCharacter(c.race, c.charClass, 0);
    render::drawTextAligned(b, infoFont, info, {text.x, y0 + nameFont.lineHeight() + gap, text.w, infoFont.lineHeight()},
                            render::HAlign::Left, render::VAlign::Top, shadowed(t.textDim, ctx));
}

// ---- RaceCard ----

RaceCard::RaceCard(const SceneResources& resources, std::uint8_t race, std::string tagline, std::string lore)
    : m_resources(resources), m_race(race), m_tagline(std::move(tagline)), m_lore(std::move(lore))
{
}

void RaceCard::drawSelf(ui::DrawContext& ctx)
{
    drawFrame(ctx);
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    const float pad = ctx.px(14.0f);
    const Rect inner = r.inset(pad);
    if (inner.empty())
        return;

    const render::Font& nameFont = ctx.font(30.0f, render::FontWeight::Bold);
    const render::Font& tagFont = ctx.font(ui::TextRole::Small);
    const render::Font& loreFont = ctx.font(ui::TextRole::Normal);

    // Lore lines at the current width (cached).
    if (m_linesWidth != inner.w || m_linesFontPx != loreFont.pixelSize())
    {
        ui::wrapText(loreFont, m_lore, inner.w, m_lines);
        m_linesWidth = inner.w;
        m_linesFontPx = loreFont.pixelSize();
    }
    // At least kReservedLoreLines so cards with shorter texts keep the same art size.
    constexpr std::size_t kReservedLoreLines = 5;
    const float loreH = loreFont.lineHeight() * static_cast<float>(std::max(m_lines.size(), kReservedLoreLines));
    const float headerH = nameFont.lineHeight() + tagFont.lineHeight() + ctx.px(14.0f);
    const float artH = std::max(ctx.px(60.0f), inner.h - headerH - loreH - ctx.px(16.0f));
    const Rect art{inner.x, inner.y, inner.w, std::round(artH)};

    CharacterLook look;
    look.race = m_race;
    look.charClass = 0;
    drawCharacterArt(b, t, m_resources.racePortrait(m_race), look, art, ctx.scale);
    b.strokeRoundedRect(art, ctx.pxf(3.0f), std::max(1.0f, ctx.pxf(1.0f)), t.panelBorder);

    float y = art.bottom() + ctx.px(8.0f);
    const std::string_view name = game::raceName(static_cast<game::Race>(m_race));
    render::drawTextAligned(b, nameFont, name, {inner.x, y, inner.w, nameFont.lineHeight()}, render::HAlign::Center,
                            render::VAlign::Top, shadowed(isSelected() ? t.textTitle : t.text, ctx));
    y += nameFont.lineHeight();
    render::drawTextAligned(b, tagFont, m_tagline, {inner.x, y, inner.w, tagFont.lineHeight()},
                            render::HAlign::Center, render::VAlign::Top, shadowed(t.accent, ctx));
    y += tagFont.lineHeight() + ctx.px(3.0f);
    const float ruleY = std::round(y) + 0.5f;
    const float half = inner.w * 0.3f;
    b.drawLine({inner.centerX() - half, ruleY}, {inner.centerX() + half, ruleY}, std::max(1.0f, ctx.scale),
               t.ornament.withAlpha(0.5f));
    b.fillDiamond({inner.centerX(), ruleY}, ctx.pxf(3.5f), t.ornament);
    y += ctx.px(8.0f);
    render::drawTextLines(b, loreFont, m_lore, m_lines, {inner.x, y, inner.w, inner.bottom() - y},
                          render::HAlign::Center, render::VAlign::Top, shadowed(t.text, ctx));

    if (isSelected())
    {
        // Gold check badge in the top-right corner of the art.
        const float d = ctx.px(30.0f);
        const float cx = art.right() - d * 0.5f - ctx.px(8.0f);
        const float cy = art.y + d * 0.5f + ctx.px(8.0f);
        fillCircle(b, cx, cy, d * 0.5f, t.accentBright);
        fillCircle(b, cx, cy, d * 0.5f - ctx.px(2.0f), Color::hex(0x2B2412));
        drawCheck(b, cx, cy, d * 0.5f, std::max(1.5f, ctx.pxf(2.5f)), t.accentBright);
    }
}

// ---- NameHints ----

NameHints::NameHints()
{
    setMouseTransparent(true);
}

void NameHints::setCheck(const CharacterNameCheck& check)
{
    m_check = check;
}

void NameHints::drawSelf(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    const Rect r = rect();
    const render::Font& font = ctx.font(ui::TextRole::Small);
    struct Item
    {
        std::string text;
        bool ok;
    };
    const Item items[3] = {
        {std::format("3–16 karakter ({})", m_check.length), m_check.lengthOk},
        {"Harf ve rakam", m_check.charactersOk},
        {"Harfle başlar", m_check.startsWithLetter},
    };
    const Color okColor = Color::hex(0x8FD18B);
    const Color badColor = Color::hex(0xE58A72);
    const float colW = r.w / 3.0f;
    const float icon = ctx.px(11.0f);
    const float lineW = std::max(1.5f, ctx.pxf(2.0f));
    for (int i = 0; i < 3; ++i)
    {
        const Item& it = items[i];
        const Color c = m_check.empty ? t.textDim : (it.ok ? okColor : badColor);
        const float x = std::round(r.x + colW * static_cast<float>(i));
        const float cy = r.centerY();
        if (m_check.empty)
            fillCircle(ctx.batch, x + icon * 0.5f, cy, ctx.pxf(2.5f), c);
        else if (it.ok)
            drawCheck(ctx.batch, x + icon * 0.5f, cy, icon, lineW, c);
        else
            drawCross(ctx.batch, x + icon * 0.5f, cy, icon, lineW, c);
        const float textX = x + icon + ctx.px(5.0f);
        render::drawTextAligned(ctx.batch, font, it.text, {textX, r.y, std::round(colW) - (textX - x), r.h},
                                render::HAlign::Left, render::VAlign::Middle, shadowed(c, ctx));
    }
}

// ---- CenteredRow ----

CenteredRow::CenteredRow(float maxWidth, float maxHeight, float spacing)
    : m_maxWidth(maxWidth), m_maxHeight(maxHeight), m_spacing(spacing)
{
    setMouseTransparent(true);
}

void CenteredRow::layoutChildren(float scale)
{
    recordLayoutScale(scale);
    const Rect content = contentRect();
    int visible = 0;
    for (const auto& c : children())
        visible += c->isVisible() ? 1 : 0;
    if (visible == 0)
        return;
    const float gap = std::round(m_spacing * scale);
    const float n = static_cast<float>(visible);
    const float w = std::floor(std::min(m_maxWidth * scale, (content.w - gap * (n - 1.0f)) / n));
    const float h = std::floor(std::min(m_maxHeight * scale, content.h));
    const float total = w * n + gap * (n - 1.0f);
    float x = std::round(content.x + (content.w - total) * 0.5f);
    const float y = std::round(content.y + (content.h - h) * 0.5f);
    for (const auto& c : children())
    {
        if (!c->isVisible())
            continue;
        placeChild(*c, {x, y, std::max(0.0f, w), std::max(0.0f, h)}, scale);
        x += w + gap;
    }
}

} // namespace vo::client::scenes
