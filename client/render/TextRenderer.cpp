#include "client/render/TextRenderer.h"

#include "client/core/Utf8.h"

#include <cmath>

namespace vo::client::render {
namespace {

float drawRun(SpriteBatch& batch, const Font& font, std::string_view text, float x, float baseline,
              const Color& color)
{
    ID3D11ShaderResourceView* srv = font.atlas().srv();
    const float startX = std::round(x);
    float pen = 0.0f;
    char32_t prev = 0;
    for (std::size_t pos = 0; pos < text.size();)
    {
        const utf8::Decoded d = utf8::decode(text, pos);
        pos += d.length;
        if (prev != 0)
            pen += font.kerning(prev, d.codePoint);
        prev = d.codePoint;
        const Glyph& g = font.glyph(d.codePoint);
        if (g.visible)
        {
            const float gx = startX + std::round(pen) + g.x0;
            const float gy = baseline + g.y0;
            batch.drawQuad(srv, Rect{gx, gy, g.x1 - g.x0, g.y1 - g.y0}, {g.u0, g.v0}, {g.u1, g.v1}, color,
                           true);
        }
        pen += g.advance;
    }
    return pen;
}

} // namespace

float drawText(SpriteBatch& batch, const Font& font, std::string_view text, Vec2 topLeft,
               const TextStyle& style)
{
    const float baseline = std::round(topLeft.y) + font.ascent();
    if (style.outline)
    {
        constexpr float kOffsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                          {1, 0},   {-1, 1}, {0, 1},  {1, 1}};
        for (const auto& o : kOffsets)
            drawRun(batch, font, text, topLeft.x + o[0], baseline + o[1], style.outlineColor);
    }
    if (style.shadow)
    {
        const float off = std::round(style.shadowOffset);
        drawRun(batch, font, text, topLeft.x + off, baseline + off, style.shadowColor);
    }
    return drawRun(batch, font, text, topLeft.x, baseline, style.color);
}

void drawTextAligned(SpriteBatch& batch, const Font& font, std::string_view text, const Rect& box, HAlign h,
                     VAlign v, const TextStyle& style)
{
    float x = box.x;
    if (h != HAlign::Left)
    {
        const float w = measureText(font, text);
        x = h == HAlign::Center ? box.x + (box.w - w) * 0.5f : box.right() - w;
    }
    float y = box.y;
    if (v == VAlign::Middle)
        y = box.y + (box.h - font.lineHeight()) * 0.5f;
    else if (v == VAlign::Bottom)
        y = box.bottom() - font.lineHeight();
    drawText(batch, font, text, {std::round(x), std::round(y)}, style);
}

void drawTextLines(SpriteBatch& batch, const Font& font, std::string_view text,
                   std::span<const ui::TextLine> lines, const Rect& box, HAlign h, VAlign v,
                   const TextStyle& style)
{
    const float lineH = font.lineHeight();
    const float total = lineH * static_cast<float>(lines.size());
    float y = box.y;
    if (v == VAlign::Middle)
        y = box.y + (box.h - total) * 0.5f;
    else if (v == VAlign::Bottom)
        y = box.bottom() - total;
    y = std::round(y);
    for (const ui::TextLine& line : lines)
    {
        const std::string_view s = text.substr(line.begin, line.end - line.begin);
        float x = box.x;
        if (h == HAlign::Center)
            x = box.x + (box.w - line.width) * 0.5f;
        else if (h == HAlign::Right)
            x = box.right() - line.width;
        drawText(batch, font, s, {std::round(x), y}, style);
        y += lineH;
    }
}

} // namespace vo::client::render
