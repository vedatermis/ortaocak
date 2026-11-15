#include "shared/net/Utf8.h"

#include <cstdint>

namespace vo::net {

namespace {

// Decodes one code point starting at text[i]; advances i. Returns false on malformed input.
bool decodeOne(std::string_view text, std::size_t& i, char32_t& cp) noexcept
{
    const auto byteAt = [&](std::size_t k) { return static_cast<std::uint8_t>(text[k]); };
    const std::uint8_t b0 = byteAt(i);
    if (b0 < 0x80)
    {
        cp = b0;
        i += 1;
        return true;
    }

    std::size_t len = 0;
    char32_t min = 0;
    if ((b0 & 0xE0) == 0xC0)
    {
        len = 2;
        cp = b0 & 0x1Fu;
        min = 0x80;
    }
    else if ((b0 & 0xF0) == 0xE0)
    {
        len = 3;
        cp = b0 & 0x0Fu;
        min = 0x800;
    }
    else if ((b0 & 0xF8) == 0xF0)
    {
        len = 4;
        cp = b0 & 0x07u;
        min = 0x10000;
    }
    else
    {
        return false;
    }
    if (text.size() - i < len)
        return false;
    for (std::size_t k = 1; k < len; ++k)
    {
        const std::uint8_t b = byteAt(i + k);
        if ((b & 0xC0) != 0x80)
            return false;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return false;
    i += len;
    return true;
}

} // namespace

bool isValidUtf8(std::string_view text) noexcept
{
    std::size_t i = 0;
    char32_t cp = 0;
    while (i < text.size())
    {
        if (!decodeOne(text, i, cp))
            return false;
    }
    return true;
}

bool decodeUtf8(std::string_view text, std::u32string& out)
{
    out.clear();
    std::size_t i = 0;
    char32_t cp = 0;
    while (i < text.size())
    {
        if (!decodeOne(text, i, cp))
            return false;
        out.push_back(cp);
    }
    return true;
}

void appendUtf8(char32_t cp, std::string& out)
{
    if (cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace vo::net
