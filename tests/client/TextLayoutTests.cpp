#include "client/ui/TextLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace vo::client::ui;

namespace {

// Every code point is 10 px wide, spaces 5 px; "AV" kerns by -2.
class FakeMetrics final : public GlyphMetrics
{
public:
    [[nodiscard]] float advance(char32_t cp) const override { return cp == U' ' ? 5.0f : 10.0f; }
    [[nodiscard]] float kerning(char32_t a, char32_t b) const override
    {
        return (a == U'A' && b == U'V') ? -2.0f : 0.0f;
    }
    [[nodiscard]] float lineHeight() const override { return 20.0f; }
};

std::vector<std::string> lineTexts(std::string_view text, const std::vector<TextLine>& lines)
{
    std::vector<std::string> out;
    for (const TextLine& l : lines)
        out.emplace_back(text.substr(l.begin, l.end - l.begin));
    return out;
}

} // namespace

TEST_CASE("measureText sums advances and kerning per code point", "[client][text]")
{
    FakeMetrics m;
    REQUIRE(measureText(m, "") == 0.0f);
    REQUIRE(measureText(m, "ab") == 20.0f);
    REQUIRE(measureText(m, "a b") == 25.0f);
    REQUIRE(measureText(m, "AV") == 18.0f);
    REQUIRE(measureText(m, "çğ") == 20.0f); // 2 code points, 4 bytes
}

TEST_CASE("caretStops has one entry per boundary", "[client][text]")
{
    FakeMetrics m;
    std::vector<float> stops;
    caretStops(m, "şAV", stops);
    REQUIRE(stops == std::vector<float>{0.0f, 10.0f, 20.0f, 28.0f});
    caretStops(m, "", stops);
    REQUIRE(stops == std::vector<float>{0.0f});
}

TEST_CASE("wrapText breaks at spaces and drops them", "[client][text]")
{
    FakeMetrics m;
    const std::string text = "çok güzel bir gün"; // words of 3, 5, 3, 3 code points
    std::vector<TextLine> lines;
    wrapText(m, text, 90.0f, lines);
    // "çok güzel" = 30 + 5 + 50 = 85 fits; " bir" would exceed 90.
    REQUIRE(lineTexts(text, lines) == std::vector<std::string>{"çok güzel", "bir gün"});
    REQUIRE(lines[0].width == 85.0f);
    REQUIRE(lines[1].width == 65.0f);
    for (const TextLine& l : lines)
        REQUIRE(l.width <= 90.0f);

    wrapText(m, text, 60.0f, lines);
    REQUIRE(lineTexts(text, lines) == std::vector<std::string>{"çok", "güzel", "bir", "gün"});
}

TEST_CASE("wrapText without width only splits hard breaks", "[client][text]")
{
    FakeMetrics m;
    const std::string text = "bir\niki üç\n\ndört";
    std::vector<TextLine> lines;
    wrapText(m, text, 0.0f, lines);
    REQUIRE(lineTexts(text, lines) == std::vector<std::string>{"bir", "iki üç", "", "dört"});
}

TEST_CASE("wrapText splits a word longer than the line", "[client][text]")
{
    FakeMetrics m;
    const std::string text = "ŞŞŞŞŞŞŞ ab"; // 7-letter word, 30 px per line -> 3 per line
    std::vector<TextLine> lines;
    wrapText(m, text, 30.0f, lines);
    REQUIRE(lineTexts(text, lines) == std::vector<std::string>{"ŞŞŞ", "ŞŞŞ", "Ş", "ab"});
    for (const TextLine& l : lines)
        REQUIRE(l.width <= 30.0f);
}

TEST_CASE("wrapText trailing spaces do not count toward width", "[client][text]")
{
    FakeMetrics m;
    std::vector<TextLine> lines;
    wrapText(m, "abc   ", 100.0f, lines);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].width == 30.0f);

    wrapText(m, "", 100.0f, lines);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].begin == 0);
    REQUIRE(lines[0].end == 0);
}

TEST_CASE("wrapText reuses the output vector", "[client][text]")
{
    FakeMetrics m;
    std::vector<TextLine> lines;
    wrapText(m, "a b c d e f g h", 20.0f, lines);
    const std::size_t count = lines.size();
    const auto* data = lines.data();
    wrapText(m, "a b c d e f g h", 20.0f, lines);
    REQUIRE(lines.size() == count);
    REQUIRE(lines.data() == data); // no reallocation in the steady state
}
