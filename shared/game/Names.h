// Account and character name rules (docs/PROTOCOL.md §5.2, §5.3) and the canonical keys used to
// enforce case-insensitive uniqueness.
//
//   username:  4-20 characters [a-zA-Z0-9_], case-insensitive    -> usernameKey(): ASCII lower-case
//   password:  6-64 bytes (any valid UTF-8)
//   character: 3-16 characters; letters (ASCII + çğıöşüÇĞİÖŞÜ) and ASCII digits; starts with a letter;
//              unique with Turkish-aware folding                  -> characterNameKey()
//
// Turkish folding maps I -> ı and İ -> i (dotted and dotless i are different letters), plus the
// other upper-case letters to lower case. Consequently "Irmak" and "ırmak" collide, as do "İnci"
// and "inci", but "Irmak" and "irmak" are different names.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vo::game {

inline constexpr std::size_t kUsernameMinLength = 4;
inline constexpr std::size_t kUsernameMaxLength = 20;
inline constexpr std::size_t kPasswordMinBytes = 6;
inline constexpr std::size_t kPasswordMaxBytes = 64;
inline constexpr std::size_t kCharacterNameMinLength = 3; // characters (code points), not bytes
inline constexpr std::size_t kCharacterNameMaxLength = 16;

[[nodiscard]] bool isValidUsername(std::string_view username) noexcept;
// ASCII lower-case of a valid username (input is not validated).
[[nodiscard]] std::string usernameKey(std::string_view username);

[[nodiscard]] bool isValidPassword(std::string_view password) noexcept;

[[nodiscard]] bool isValidCharacterName(std::string_view utf8Name);
// Turkish-aware case fold to a canonical UTF-8 key. Code points outside the folding table are kept
// unchanged; invalid UTF-8 yields an empty string.
[[nodiscard]] std::string characterNameKey(std::string_view utf8Name);

// Letter/digit classification used by isValidCharacterName.
[[nodiscard]] bool isCharacterNameLetter(char32_t cp) noexcept;
[[nodiscard]] char32_t turkishFold(char32_t cp) noexcept;

} // namespace vo::game
