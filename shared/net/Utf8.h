// Strict UTF-8 validation/decoding (RFC 3629: no overlong forms, no surrogates, max U+10FFFF).
// Used by PacketReader for str8/str16 and by shared/game name validation.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vo::net {

[[nodiscard]] bool isValidUtf8(std::string_view text) noexcept;

// Decodes `text` into code points. Returns false (and leaves `out` unspecified) on invalid input.
[[nodiscard]] bool decodeUtf8(std::string_view text, std::u32string& out);

// Appends the UTF-8 encoding of `cp` (must be a valid scalar value) to `out`.
void appendUtf8(char32_t cp, std::string& out);

} // namespace vo::net
