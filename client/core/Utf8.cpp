#include "client/core/Utf8.h"

namespace vo::client::utf8 {
namespace {

constexpr bool isContinuation(unsigned char c) noexcept
{
    return (c & 0xC0) == 0x80;
}

} // namespace

Decoded decode(std::string_view text, std::size_t pos) noexcept
{
    if (pos >= text.size())
        return {kReplacement, 0, false};

    const auto b0 = static_cast<unsigned char>(text[pos]);
    if (b0 < 0x80)
        return {b0, 1, true};

    std::size_t len = 0;
    char32_t cp = 0;
    char32_t minValue = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF)
    {
        len = 2;
        cp = b0 & 0x1F;
        minValue = 0x80;
    }
    else if (b0 >= 0xE0 && b0 <= 0xEF)
    {
        len = 3;
        cp = b0 & 0x0F;
        minValue = 0x800;
    }
    else if (b0 >= 0xF0 && b0 <= 0xF4)
    {
        len = 4;
        cp = b0 & 0x07;
        minValue = 0x10000;
    }
    else
    {
        return {kReplacement, 1, false}; // stray continuation byte, C0/C1 or F5..FF
    }

    if (pos + len > text.size())
        return {kReplacement, 1, false};
    for (std::size_t i = 1; i < len; ++i)
    {
        const auto b = static_cast<unsigned char>(text[pos + i]);
        if (!isContinuation(b))
            return {kReplacement, 1, false};
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < minValue || !isScalarValue(cp))
        return {kReplacement, 1, false}; // overlong, surrogate or > U+10FFFF
    return {cp, len, true};
}

std::size_t encode(char32_t cp, char (&out)[4]) noexcept
{
    if (!isScalarValue(cp))
        cp = kReplacement;
    if (cp < 0x80)
    {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

void append(std::string& out, char32_t cp)
{
    char buf[4];
    const std::size_t n = encode(cp, buf);
    out.append(buf, n);
}

std::size_t nextBoundary(std::string_view text, std::size_t pos) noexcept
{
    if (pos >= text.size())
        return text.size();
    return pos + decode(text, pos).length;
}

std::size_t prevBoundary(std::string_view text, std::size_t pos) noexcept
{
    if (pos == 0)
        return 0;
    if (pos > text.size())
        pos = text.size();
    // Walk back over at most 3 continuation bytes to a candidate lead byte, then check that decoding
    // from there lands exactly on pos. Otherwise the previous "code point" is a single invalid byte.
    std::size_t start = pos - 1;
    while (start > 0 && pos - start < 4 && isContinuation(static_cast<unsigned char>(text[start])))
        --start;
    const Decoded d = decode(text, start);
    if (d.valid && start + d.length == pos)
        return start;
    return pos - 1;
}

namespace {

// Start of the valid multi-byte sequence that strictly contains `pos`, or npos.
std::size_t coveringStart(std::string_view text, std::size_t pos) noexcept
{
    for (std::size_t back = 1; back <= 3 && back <= pos; ++back)
    {
        const std::size_t s = pos - back;
        const Decoded d = decode(text, s);
        if (d.valid && s + d.length > pos)
            return s;
        if (!isContinuation(static_cast<unsigned char>(text[s])))
            break; // a lead/ASCII byte that does not cover pos: nothing further back can
    }
    return std::string_view::npos;
}

} // namespace

bool isBoundary(std::string_view text, std::size_t pos) noexcept
{
    if (pos == 0 || pos == text.size())
        return true;
    if (pos > text.size())
        return false;
    return coveringStart(text, pos) == std::string_view::npos;
}

std::size_t floorBoundary(std::string_view text, std::size_t pos) noexcept
{
    if (pos >= text.size())
        return text.size();
    const std::size_t start = coveringStart(text, pos);
    return start == std::string_view::npos ? pos : start;
}

std::size_t countCodePoints(std::string_view text) noexcept
{
    std::size_t count = 0;
    for (std::size_t pos = 0; pos < text.size(); pos += decode(text, pos).length)
        ++count;
    return count;
}

std::size_t offsetOfIndex(std::string_view text, std::size_t index) noexcept
{
    std::size_t pos = 0;
    for (std::size_t i = 0; i < index && pos < text.size(); ++i)
        pos += decode(text, pos).length;
    return pos;
}

bool isValid(std::string_view text) noexcept
{
    for (std::size_t pos = 0; pos < text.size();)
    {
        const Decoded d = decode(text, pos);
        if (!d.valid)
            return false;
        pos += d.length;
    }
    return true;
}

std::string sanitize(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t pos = 0; pos < text.size();)
    {
        const Decoded d = decode(text, pos);
        if (d.valid)
            out.append(text.substr(pos, d.length));
        else
            append(out, kReplacement);
        pos += d.length;
    }
    return out;
}

} // namespace vo::client::utf8
