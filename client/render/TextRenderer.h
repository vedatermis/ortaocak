// Text drawing on top of SpriteBatch + Font: single lines, aligned lines in a box, wrapped
// paragraphs, optional drop shadow and outline. Baselines and glyph quads are snapped to whole
// pixels. Measuring and wrapping come from ui/TextLayout (font-independent, unit-tested).
#pragma once

#include "client/core/Geometry.h"
#include "client/render/Font.h"
#include "client/render/SpriteBatch.h"
#include "client/ui/TextLayout.h"

#include <span>
#include <string_view>

namespace vo::client::render {

enum class HAlign : std::uint8_t
{
    Left,
    Center,
    Right,
};

enum class VAlign : std::uint8_t
{
    Top,
    Middle,
    Bottom,
};

struct TextStyle
{
    Color color = colors::kWhite;
    bool shadow = false;
    Color shadowColor{0.0f, 0.0f, 0.0f, 0.65f};
    float shadowOffset = 1.0f; // pixels, down-right
    bool outline = false;
    Color outlineColor{0.0f, 0.0f, 0.0f, 0.9f};
};

// Width of a line in pixels (same pen arithmetic as drawing).
[[nodiscard]] inline float measureText(const Font& font, std::string_view text)
{
    return ui::measureText(font, text);
}

// Draws one line; `topLeft` is the top of the line box (baseline = top + ascent). Returns the width.
float drawText(SpriteBatch& batch, const Font& font, std::string_view text, Vec2 topLeft,
               const TextStyle& style);

// Single line aligned inside `box` (no wrapping, no clipping).
void drawTextAligned(SpriteBatch& batch, const Font& font, std::string_view text, const Rect& box, HAlign h,
                     VAlign v, const TextStyle& style);

// Pre-wrapped lines (from ui::wrapText) stacked at the font's line height and aligned in `box`.
void drawTextLines(SpriteBatch& batch, const Font& font, std::string_view text,
                   std::span<const ui::TextLine> lines, const Rect& box, HAlign h, VAlign v,
                   const TextStyle& style);

} // namespace vo::client::render
