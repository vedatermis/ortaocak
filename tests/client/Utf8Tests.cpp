#include "client/core/Utf8.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace utf8 = vo::client::utf8;

namespace {

std::vector<char32_t> decodeAll(std::string_view s)
{
    std::vector<char32_t> out;
    for (std::size_t pos = 0; pos < s.size();)
    {
        const auto d = utf8::decode(s, pos);
        out.push_back(d.codePoint);
        pos += d.length;
    }
    return out;
}

} // namespace

TEST_CASE("UTF-8 decodes Turkish letters", "[client][utf8]")
{
    const std::string s = "çğıİöşüÇĞÖŞÜ";
    const std::vector<char32_t> expected{0xE7, 0x11F, 0x131, 0x130, 0xF6,  0x15F,
                                         0xFC, 0xC7,  0x11E, 0xD6,  0x15E, 0xDC};
    REQUIRE(decodeAll(s) == expected);
    REQUIRE(utf8::countCodePoints(s) == 12);
    REQUIRE(utf8::isValid(s));
}

TEST_CASE("UTF-8 decodes 1-4 byte sequences", "[client][utf8]")
{
    REQUIRE(utf8::decode("A", 0).codePoint == U'A');
    REQUIRE(utf8::decode("\xC3\xA7", 0).codePoint == 0xE7);
    REQUIRE(utf8::decode("\xE2\x82\xAC", 0).codePoint == 0x20AC); // euro
    const auto d = utf8::decode("\xF0\x9F\x90\x89", 0);           // U+1F409
    REQUIRE(d.valid);
    REQUIRE(d.codePoint == 0x1F409);
    REQUIRE(d.length == 4);
}

TEST_CASE("UTF-8 invalid input yields one replacement per bad byte", "[client][utf8]")
{
    SECTION("stray continuation and invalid lead bytes")
    {
        REQUIRE(decodeAll("\x80") == std::vector<char32_t>{utf8::kReplacement});
        REQUIRE(decodeAll("\xFF"
                          "a") == std::vector<char32_t>{utf8::kReplacement, U'a'});
        REQUIRE_FALSE(utf8::isValid("\xC0\xAF")); // C0 is never valid
    }
    SECTION("truncated sequence")
    {
        REQUIRE(decodeAll("\xE2\x82") == std::vector<char32_t>{utf8::kReplacement, utf8::kReplacement});
        REQUIRE(decodeAll("\xC3") == std::vector<char32_t>{utf8::kReplacement});
    }
    SECTION("overlong, surrogate and out-of-range forms")
    {
        REQUIRE_FALSE(utf8::decode("\xE0\x80\xAF", 0).valid);     // overlong '/'
        REQUIRE_FALSE(utf8::decode("\xED\xA0\x80", 0).valid);     // U+D800
        REQUIRE_FALSE(utf8::decode("\xF4\x90\x80\x80", 0).valid); // > U+10FFFF
        REQUIRE(utf8::decode("\xED\xA0\x80", 0).length == 1);
    }
    SECTION("sanitize replaces invalid bytes")
    {
        REQUIRE(utf8::sanitize("a\xFF"
                               "b") == "a\xEF\xBF\xBD"
                                       "b");
        REQUIRE(utf8::sanitize("çok") == "çok");
    }
}

TEST_CASE("UTF-8 encode round-trips and rejects invalid scalars", "[client][utf8]")
{
    for (const char32_t cp :
         {char32_t{0x41}, char32_t{0xE7}, char32_t{0x130}, char32_t{0x20BA}, char32_t{0x1F409}})
    {
        std::string s;
        utf8::append(s, cp);
        const auto d = utf8::decode(s, 0);
        REQUIRE(d.valid);
        REQUIRE(d.codePoint == cp);
        REQUIRE(d.length == s.size());
    }
    std::string bad;
    utf8::append(bad, 0xD800);
    REQUIRE(bad == "\xEF\xBF\xBD");
    bad.clear();
    utf8::append(bad, 0x110000);
    REQUIRE(bad == "\xEF\xBF\xBD");
}

TEST_CASE("UTF-8 boundary stepping", "[client][utf8]")
{
    const std::string s = "aşİ\xF0\x9F\x90\x89z"; // 1 + 2 + 2 + 4 + 1 bytes
    REQUIRE(s.size() == 10);
    std::vector<std::size_t> forward;
    for (std::size_t p = 0; p < s.size(); p = utf8::nextBoundary(s, p))
        forward.push_back(p);
    REQUIRE(forward == std::vector<std::size_t>{0, 1, 3, 5, 9});

    std::vector<std::size_t> backward;
    for (std::size_t p = s.size(); p > 0;)
    {
        p = utf8::prevBoundary(s, p);
        backward.push_back(p);
    }
    REQUIRE(backward == std::vector<std::size_t>{9, 5, 3, 1, 0});

    REQUIRE(utf8::isBoundary(s, 3));
    REQUIRE_FALSE(utf8::isBoundary(s, 2));
    REQUIRE(utf8::floorBoundary(s, 2) == 1);
    REQUIRE(utf8::floorBoundary(s, 7) == 5);
    REQUIRE(utf8::offsetOfIndex(s, 3) == 5);
    REQUIRE(utf8::offsetOfIndex(s, 99) == s.size());
    REQUIRE(utf8::nextBoundary(s, s.size()) == s.size());
    REQUIRE(utf8::prevBoundary(s, 0) == 0);
}

TEST_CASE("UTF-8 prevBoundary over invalid bytes steps one byte", "[client][utf8]")
{
    const std::string s = "a\x80\x80"
                          "b";
    REQUIRE(utf8::prevBoundary(s, 4) == 3);
    REQUIRE(utf8::prevBoundary(s, 3) == 2);
    REQUIRE(utf8::prevBoundary(s, 2) == 1);
    // Consistency: stepping back always lands where stepping forward would.
    for (std::size_t p = 1; p <= s.size(); ++p)
        REQUIRE(utf8::nextBoundary(s, utf8::prevBoundary(s, p)) == p);
}

TEST_CASE("UTF-16 surrogate helpers", "[client][utf8]")
{
    REQUIRE(utf8::isHighSurrogate(0xD83D));
    REQUIRE(utf8::isLowSurrogate(0xDC09));
    REQUIRE(utf8::combineSurrogates(0xD83D, 0xDC09) == 0x1F409);
}
