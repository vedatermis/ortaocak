#include "client/ui/TextLayout.h"

#include "client/core/Utf8.h"

namespace vo::client::ui {
namespace {

bool isBreakSpace(char32_t cp) noexcept
{
    return cp == ' ' || cp == '\t';
}

} // namespace

float measureText(const GlyphMetrics& metrics, std::string_view text)
{
    float pen = 0.0f;
    char32_t prev = 0;
    for (std::size_t pos = 0; pos < text.size();)
    {
        const utf8::Decoded d = utf8::decode(text, pos);
        if (prev != 0)
            pen += metrics.kerning(prev, d.codePoint);
        pen += metrics.advance(d.codePoint);
        prev = d.codePoint;
        pos += d.length;
    }
    return pen;
}

void caretStops(const GlyphMetrics& metrics, std::string_view text, std::vector<float>& out)
{
    out.clear();
    out.push_back(0.0f);
    float pen = 0.0f;
    char32_t prev = 0;
    for (std::size_t pos = 0; pos < text.size();)
    {
        const utf8::Decoded d = utf8::decode(text, pos);
        if (prev != 0)
            pen += metrics.kerning(prev, d.codePoint);
        pen += metrics.advance(d.codePoint);
        out.push_back(pen);
        prev = d.codePoint;
        pos += d.length;
    }
}

void wrapText(const GlyphMetrics& metrics, std::string_view text, float maxWidth, std::vector<TextLine>& out)
{
    out.clear();
    const std::size_t n = text.size();

    std::size_t lineStart = 0;
    float width = 0.0f; // pen at `pos` relative to lineStart
    char32_t prev = 0;  // previous code point on this line (0 = none)
    bool haveBreak = false;
    std::size_t breakEnd = 0;    // end of the content before the last space run
    std::size_t breakResume = 0; // first byte after that space run
    float breakWidth = 0.0f;     // width of [lineStart, breakEnd)

    auto lineWidth = [&]() { return isBreakSpace(prev) && haveBreak ? breakWidth : width; };
    auto resetLine = [&](std::size_t start)
    {
        lineStart = start;
        width = 0.0f;
        prev = 0;
        haveBreak = false;
    };

    std::size_t pos = 0;
    while (pos < n)
    {
        const utf8::Decoded d = utf8::decode(text, pos);
        const char32_t cp = d.codePoint;

        if (cp == '\n')
        {
            out.push_back({lineStart, pos, lineWidth()});
            pos += d.length;
            resetLine(pos);
            continue;
        }

        if (isBreakSpace(cp))
        {
            if (!isBreakSpace(prev) && pos > lineStart)
            {
                breakEnd = pos;
                breakWidth = width;
                haveBreak = true;
            }
            width += (prev != 0 ? metrics.kerning(prev, cp) : 0.0f) + metrics.advance(cp);
            prev = cp;
            pos += d.length;
            if (haveBreak)
                breakResume = pos;
            continue;
        }

        float adv = (prev != 0 ? metrics.kerning(prev, cp) : 0.0f) + metrics.advance(cp);
        if (maxWidth > 0.0f && width + adv > maxWidth && pos > lineStart)
        {
            if (haveBreak && breakEnd > lineStart)
            {
                // Soft break at the last space run; the partial word moves to the next line.
                out.push_back({lineStart, breakEnd, breakWidth});
                const std::size_t wordStart = breakResume;
                resetLine(wordStart);
                const std::string_view partial = text.substr(wordStart, pos - wordStart);
                width = measureText(metrics, partial);
                prev = partial.empty() ? 0 : utf8::decode(text, utf8::prevBoundary(text, pos)).codePoint;
                adv = (prev != 0 ? metrics.kerning(prev, cp) : 0.0f) + metrics.advance(cp);
            }
            if (width + adv > maxWidth && pos > lineStart)
            {
                // The word alone is wider than the line: split it here.
                out.push_back({lineStart, pos, width});
                resetLine(pos);
                adv = metrics.advance(cp);
            }
        }
        width += adv;
        prev = cp;
        pos += d.length;
    }
    out.push_back({lineStart, n, lineWidth()});
}

} // namespace vo::client::ui
