// UTF-8 helpers: strict decoding with a replacement code point, encoding, and code point boundary
// stepping. All client strings are UTF-8 std::string; byte offsets handed around (caret, selection)
// always sit on code point boundaries as defined by decode().
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vo::client::utf8 {

inline constexpr char32_t kReplacement = 0xFFFD;
inline constexpr char32_t kMaxCodePoint = 0x10FFFF;

struct Decoded
{
    char32_t codePoint = kReplacement;
    std::size_t length = 0; // bytes consumed (>= 1 unless pos is at/after the end)
    bool valid = false;
};

// Decodes the code point starting at byte `pos`. Invalid or truncated sequences, overlong forms,
// surrogates and values above U+10FFFF yield {U+FFFD, 1, false}: exactly one byte is skipped.
[[nodiscard]] Decoded decode(std::string_view text, std::size_t pos) noexcept;

// True for code points that can be encoded (not a surrogate, <= U+10FFFF).
[[nodiscard]] constexpr bool isScalarValue(char32_t cp) noexcept
{
    return cp <= kMaxCodePoint && (cp < 0xD800 || cp > 0xDFFF);
}

// Encodes `cp` into `out` (invalid values become U+FFFD). Returns the number of bytes (1..4).
std::size_t encode(char32_t cp, char (&out)[4]) noexcept;
void append(std::string& out, char32_t cp);

// Offset of the next / previous code point boundary. nextBoundary(pos >= size) returns size;
// prevBoundary(0) returns 0.
[[nodiscard]] std::size_t nextBoundary(std::string_view text, std::size_t pos) noexcept;
[[nodiscard]] std::size_t prevBoundary(std::string_view text, std::size_t pos) noexcept;

// True if `pos` is 0, size(), or the start of a decode() step when walking from the beginning.
[[nodiscard]] bool isBoundary(std::string_view text, std::size_t pos) noexcept;
// Largest boundary <= pos.
[[nodiscard]] std::size_t floorBoundary(std::string_view text, std::size_t pos) noexcept;

[[nodiscard]] std::size_t countCodePoints(std::string_view text) noexcept;
// Byte offset of the code point with the given index (size() if index >= count).
[[nodiscard]] std::size_t offsetOfIndex(std::string_view text, std::size_t index) noexcept;

[[nodiscard]] bool isValid(std::string_view text) noexcept;
// Copy with every invalid byte replaced by U+FFFD.
[[nodiscard]] std::string sanitize(std::string_view text);

// UTF-16 helpers used for WM_CHAR handling.
[[nodiscard]] constexpr bool isHighSurrogate(char32_t u) noexcept
{
    return u >= 0xD800 && u <= 0xDBFF;
}
[[nodiscard]] constexpr bool isLowSurrogate(char32_t u) noexcept
{
    return u >= 0xDC00 && u <= 0xDFFF;
}
[[nodiscard]] constexpr char32_t combineSurrogates(char32_t hi, char32_t lo) noexcept
{
    return 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
}

} // namespace vo::client::utf8
