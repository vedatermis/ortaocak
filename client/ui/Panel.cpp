#include "client/ui/Panel.h"

#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"

namespace vo::client::ui {

Panel::Panel(Style style, std::string title) : m_style(style), m_title(std::move(title))
{
    if (style == Style::None)
        setMouseTransparent(true);
}

Panel& Panel::setTitle(std::string title)
{
    const bool hadTitle = !m_title.empty();
    m_title = std::move(title);
    if (hadTitle != !m_title.empty())
        markLayoutDirty();
    return *this;
}

Panel& Panel::setStyle(Style style)
{
    m_style = style;
    setMouseTransparent(style == Style::None);
    markLayoutDirty();
    return *this;
}

float Panel::titleBarHeight() const noexcept
{
    if (m_style != Style::Window || m_title.empty() || context() == nullptr)
        return 0.0f;
    return std::round(context()->theme().titleBarHeight * context()->scale());
}

Rect Panel::contentRect() const
{
    const Rect base = Widget::contentRect();
    const float bar = titleBarHeight();
    return base.inset(0.0f, bar, 0.0f, 0.0f);
}

void Panel::drawSelf(DrawContext& ctx)
{
    const Theme& t = ctx.theme;
    const Rect r = rect();
    auto& b = ctx.batch;
    const float radius = ctx.pxf(t.cornerRadius);
    const float border = std::max(1.0f, ctx.pxf(t.borderWidth));

    switch (m_style)
    {
    case Style::None:
        return;

    case Style::Plain:
        b.fillRoundedRect(r, radius, t.panelBottom);
        b.strokeRoundedRect(r, radius, border, t.insetBorder);
        return;

    case Style::Inset:
        b.fillRoundedRect(r, radius, t.insetFill);
        b.strokeRoundedRect(r, radius, border, t.insetBorder);
        // inner top shadow line
        b.fillRect({r.x + radius, r.y + border, r.w - 2.0f * radius, border}, Color::hex(0x000000, 0.35f));
        return;

    case Style::Window:
        break;
    }

    // Drop shadow (soft, two layers).
    const float s = ctx.pxf(4.0f);
    b.fillRoundedRect({r.x - s * 0.5f, r.y + s * 0.5f, r.w + s, r.h + s}, radius + s,
                      t.panelShadow.scaledAlpha(0.5f));
    b.fillRoundedRect({r.x, r.y + s * 0.5f, r.w, r.h + s * 0.5f}, radius + s * 0.5f, t.panelShadow);

    if (t.panelSkin.texture != nullptr)
    {
        b.drawNineSlice(*t.panelSkin.texture, r, t.panelSkin.srcBorder,
                        t.panelSkin.dstBorder.scaled(ctx.scale), t.panelSkin.tint);
    }
    else
    {
        b.fillRoundedRectGradient(r, radius, t.panelTop, t.panelBottom);
        // Double frame: bronze outer border and a faint inner bevel.
        b.strokeRoundedRect(r, radius, border, t.panelBorder);
        const float inner = ctx.px(3.0f);
        b.strokeRoundedRect(r.inset(inner), std::max(0.0f, radius - inner * 0.5f), border, t.panelHighlight);
    }

    const float bar = titleBarHeight();
    if (bar > 0.0f)
    {
        const float inset = ctx.px(3.0f) + border;
        const Rect barRect{r.x + inset, r.y + inset, r.w - 2.0f * inset, bar - inset};
        b.fillRoundedRectGradient(barRect, std::max(0.0f, radius - inset * 0.5f),
                                  t.titleBarTop.withAlpha(0.9f), t.titleBarBottom.withAlpha(0.9f));
        // Separator with a diamond in the middle and fading gold rule.
        const float y = std::round(r.y + bar) + 0.5f * border;
        const float cx = r.centerX();
        const float halfLen = r.w * 0.5f - inset - ctx.px(6.0f);
        b.drawLine({cx - halfLen, y}, {cx - ctx.px(8.0f), y}, border, t.ornament.withAlpha(0.75f));
        b.drawLine({cx + ctx.px(8.0f), y}, {cx + halfLen, y}, border, t.ornament.withAlpha(0.75f));
        b.fillDiamond({cx, y}, ctx.pxf(4.5f), t.ornament);
        b.fillDiamond({cx - halfLen, y}, ctx.pxf(2.5f), t.ornament);
        b.fillDiamond({cx + halfLen, y}, ctx.pxf(2.5f), t.ornament);

        const render::Font& font = ctx.font(TextRole::Title, render::FontWeight::Bold);
        render::TextStyle style;
        style.color = t.textTitle;
        style.shadow = true;
        style.shadowColor = t.textShadow;
        style.shadowOffset = std::max(1.0f, std::round(ctx.scale));
        render::drawTextAligned(b, font, m_title, {r.x, r.y + inset, r.w, bar - inset},
                                render::HAlign::Center, render::VAlign::Middle, style);
    }

    // Corner ornaments: short gold brackets.
    const float len = ctx.px(10.0f);
    const float o = ctx.px(6.0f);
    const float w = std::max(1.0f, ctx.pxf(1.5f));
    const Color c = t.ornament.withAlpha(0.85f);
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

} // namespace vo::client::ui
