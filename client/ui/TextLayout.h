// Font-independent text layout: measuring, caret stops and word wrapping over an abstract glyph
// metrics provider (the real one is render::Font; tests use a fake). Measurements are in pixels.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace vo::client::ui {

class GlyphMetrics
{
public:
    virtual ~GlyphMetrics() = default;
    // Horizontal advance of a code point (fallback glyph advance if the font lacks it).
    [[nodiscard]] virtual float advance(char32_t cp) const = 0;
    // Kerning adjustment to add between `left` and `right`.
    [[nodiscard]] virtual float kerning(char32_t left, char32_t right) const = 0;
    [[nodiscard]] virtual float lineHeight() const = 0;
};

// Width of a single line (newlines are measured like any other code point; callers split first).
[[nodiscard]] float measureText(const GlyphMetrics& metrics, std::string_view text);

// Pen position (x, before rounding) at every code point boundary of `text`: out[0] = 0,
// out.back() = full width; out has countCodePoints(text) + 1 entries. `out` is cleared first and
// reuses its capacity.
void caretStops(const GlyphMetrics& metrics, std::string_view text, std::vector<float>& out);

struct TextLine
{
    std::size_t begin = 0; // byte range into the source text (excludes the break character)
    std::size_t end = 0;
    float width = 0.0f; // trailing spaces excluded
};

// Splits `text` into lines no wider than `maxWidth` (<= 0: only hard breaks). Breaks at '\n' and
// after spaces; a word longer than a line is split at code point boundaries. Spaces at a soft
// break are dropped (not part of either line). `out` is cleared first and reuses its capacity.
void wrapText(const GlyphMetrics& metrics, std::string_view text, float maxWidth, std::vector<TextLine>& out);

} // namespace vo::client::ui
