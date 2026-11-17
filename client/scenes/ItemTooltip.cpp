#include "client/scenes/ItemTooltip.h"

#include "client/render/TextRenderer.h"
#include "client/ui/DrawContext.h"
#include "client/ui/TextLayout.h"

#include <algorithm>

namespace vo::client::scenes {

namespace {

constexpr float kTooltipWidth = 270.0f;
const Color kRed = Color::hex(0xE5736A);
const Color kGood = Color::hex(0x9FD27A);
const Color kValue = Color::hex(0xE9C46A);

} // namespace

std::vector<ShownTipLine> shownTipLines(const std::vector<world::TipLine>& lines, const ui::Theme& t)
{
    std::vector<ShownTipLine> out;
    out.reserve(lines.size());
    for (const world::TipLine& l : lines)
    {
        ShownTipLine s;
        s.text = l.text;
        s.wrap = l.wrap;
        switch (l.tone)
        {
        case world::TipTone::Title:
            s.color = Color::hex(l.rgb != 0 ? l.rgb : 0xF2CA64);
            s.title = true;
            break;
        case world::TipTone::Normal: s.color = t.text; break;
        case world::TipTone::Dim: s.color = t.textDim; break;
        case world::TipTone::Good: s.color = kGood; break;
        case world::TipTone::Bad:
            s.color = kRed;
            s.unmet = true;
            break;
        case world::TipTone::Value: s.color = kValue; break;
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::string tooltipPlainText(const std::vector<ShownTipLine>& lines)
{
    std::string out;
    for (const ShownTipLine& l : lines)
    {
        if (!out.empty())
            out += '\n';
        out += l.text;
        if (l.unmet)
            out += " (karşılanmıyor)";
    }
    return out;
}

void drawTooltipBox(ui::DrawContext& ctx, const std::vector<ShownTipLine>& lines, const Rect& anchor, float viewportW,
                    float viewportH, bool above)
{
    if (lines.empty())
        return;
    auto& b = ctx.batch;
    const ui::Theme& t = ctx.theme;
    const float pad = ctx.px(10.0f);
    const float gap = ctx.px(2.0f);
    const float maxW = ctx.px(kTooltipWidth);
    const render::Font& titleFont = ctx.font(ui::TextRole::Large, render::FontWeight::Bold);
    const render::Font& bodyFont = ctx.font(ui::TextRole::Small);

    struct Laid
    {
        const ShownTipLine* line;
        const render::Font* font;
        std::vector<ui::TextLine> wrapped;
        float h;
    };
    std::vector<Laid> laid;
    float w = 0.0f;
    float h = 0.0f;
    for (const ShownTipLine& l : lines)
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

    // Right of the anchor, else left of it; inside the screen vertically.
    const float margin = ctx.px(4.0f);
    float x = anchor.right() + ctx.px(8.0f);
    if (x + boxW > viewportW - margin)
        x = anchor.x - ctx.px(8.0f) - boxW;
    float y = anchor.y;
    if (above)
    {
        x = std::round(anchor.centerX() - boxW * 0.5f);
        y = anchor.y - ctx.px(8.0f) - boxH;
    }
    x = std::clamp(x, margin, std::max(margin, viewportW - margin - boxW));
    y = std::clamp(y, margin, std::max(margin, viewportH - margin - boxH));
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
