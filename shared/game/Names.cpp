#include "shared/game/Names.h"

#include "shared/net/Utf8.h"

namespace vo::game {

namespace {

constexpr bool isAsciiLetter(char32_t c) noexcept
{
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

constexpr bool isAsciiDigit(char32_t c) noexcept
{
    return c >= U'0' && c <= U'9';
}

} // namespace

bool isValidUsername(std::string_view username) noexcept
{
    if (username.size() < kUsernameMinLength || username.size() > kUsernameMaxLength)
        return false;
    for (const char ch : username)
    {
        const auto c = static_cast<char32_t>(static_cast<unsigned char>(ch));
        if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != U'_')
            return false;
    }
    return true;
}

std::string usernameKey(std::string_view username)
{
    std::string key(username);
    for (char& c : key)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

bool isValidPassword(std::string_view password) noexcept
{
    return password.size() >= kPasswordMinBytes && password.size() <= kPasswordMaxBytes &&
           net::isValidUtf8(password);
}

bool isCharacterNameLetter(char32_t cp) noexcept
{
    if (isAsciiLetter(cp))
        return true;
    switch (cp)
    {
    case U'ç': case U'ğ': case U'ı': case U'ö': case U'ş': case U'ü':
    case U'Ç': case U'Ğ': case U'İ': case U'Ö': case U'Ş': case U'Ü':
        return true;
    default:
        return false;
    }
}

char32_t turkishFold(char32_t cp) noexcept
{
    if (cp == U'I')
        return U'ı';
    if (cp >= U'A' && cp <= U'Z')
        return static_cast<char32_t>(cp - U'A' + U'a');
    switch (cp)
    {
    case U'İ': return U'i';
    case U'Ç': return U'ç';
    case U'Ğ': return U'ğ';
    case U'Ö': return U'ö';
    case U'Ş': return U'ş';
    case U'Ü': return U'ü';
    default: return cp;
    }
}

bool isValidCharacterName(std::string_view utf8Name)
{
    std::u32string cps;
    if (!net::decodeUtf8(utf8Name, cps))
        return false;
    if (cps.size() < kCharacterNameMinLength || cps.size() > kCharacterNameMaxLength)
        return false;
    if (!isCharacterNameLetter(cps.front()))
        return false;
    for (const char32_t cp : cps)
    {
        if (!isCharacterNameLetter(cp) && !isAsciiDigit(cp))
            return false;
    }
    return true;
}

std::string characterNameKey(std::string_view utf8Name)
{
    std::u32string cps;
    if (!net::decodeUtf8(utf8Name, cps))
        return {};
    std::string key;
    key.reserve(utf8Name.size());
    for (const char32_t cp : cps)
        net::appendUtf8(turkishFold(cp), key);
    return key;
}

} // namespace vo::game
