#include "client/scenes/Validation.h"

#include "shared/game/Names.h"
#include "shared/net/Utf8.h"

#include <format>

namespace vo::client::scenes {

namespace {

constexpr bool isAsciiDigit(char32_t c) noexcept
{
    return c >= U'0' && c <= U'9';
}

constexpr bool isUsernameChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

} // namespace

std::optional<std::string> validateUsername(std::string_view username)
{
    if (username.empty())
        return std::string("Lütfen kullanıcı adınızı girin.");
    for (const char c : username)
    {
        if (c == ' ')
            return std::string("Kullanıcı adı boşluk içeremez.");
        if (!isUsernameChar(c))
            return std::string(
                "Kullanıcı adı yalnızca İngilizce harf (a–z), rakam ve alt çizgi (_) içerebilir; Türkçe "
                "karakterler kullanılamaz.");
    }
    if (username.size() < game::kUsernameMinLength)
        return std::format("Kullanıcı adı en az {} karakter olmalıdır.", game::kUsernameMinLength);
    if (username.size() > game::kUsernameMaxLength)
        return std::format("Kullanıcı adı en fazla {} karakter olabilir.", game::kUsernameMaxLength);
    if (!game::isValidUsername(username)) // keep in lock-step with the shared rule
        return std::string("Kullanıcı adı geçersiz.");
    return std::nullopt;
}

std::optional<std::string> validatePassword(std::string_view password)
{
    if (password.empty())
        return std::string("Lütfen şifrenizi girin.");
    if (!vo::net::isValidUtf8(password))
        return std::string("Şifre geçersiz karakterler içeriyor.");
    if (password.size() < game::kPasswordMinBytes)
        return std::format("Şifre en az {} karakter olmalıdır.", game::kPasswordMinBytes);
    if (password.size() > game::kPasswordMaxBytes)
        return std::format("Şifre çok uzun (en fazla {} bayt; Türkçe harfler 2 bayt sayılır).",
                           game::kPasswordMaxBytes);
    if (!game::isValidPassword(password))
        return std::string("Şifre geçersiz.");
    return std::nullopt;
}

CharacterNameCheck checkCharacterName(std::string_view name)
{
    CharacterNameCheck c;
    c.empty = name.empty();
    std::u32string cps;
    const bool decoded = vo::net::decodeUtf8(name, cps);
    c.length = decoded ? cps.size() : name.size();
    c.lengthOk = c.length >= game::kCharacterNameMinLength && c.length <= game::kCharacterNameMaxLength;
    c.startsWithLetter = decoded && !cps.empty() && game::isCharacterNameLetter(cps.front());
    c.charactersOk = decoded && !cps.empty();
    if (decoded)
    {
        for (const char32_t cp : cps)
        {
            if (!game::isCharacterNameLetter(cp) && !isAsciiDigit(cp))
            {
                c.charactersOk = false;
                break;
            }
        }
    }
    c.valid = game::isValidCharacterName(name);

    if (c.valid)
        return c;
    if (c.empty)
        c.message = "Lütfen karakterinize bir isim verin.";
    else if (!c.charactersOk)
        c.message = "İsim yalnızca harf ve rakam içerebilir (boşluk ve işaretler kullanılamaz).";
    else if (!c.startsWithLetter)
        c.message = "İsim bir harfle başlamalıdır.";
    else if (c.length < game::kCharacterNameMinLength)
        c.message = std::format("İsim en az {} karakter olmalıdır.", game::kCharacterNameMinLength);
    else if (c.length > game::kCharacterNameMaxLength)
        c.message = std::format("İsim en fazla {} karakter olabilir.", game::kCharacterNameMaxLength);
    else
        c.message = "Bu isim kurallara uymuyor.";
    return c;
}

} // namespace vo::client::scenes
