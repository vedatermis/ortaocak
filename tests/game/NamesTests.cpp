#include "shared/game/Character.h"
#include "shared/game/Names.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace vg = vo::game;

TEST_CASE("Username rules", "[game][names]")
{
    CHECK(vg::isValidUsername("abcd"));
    CHECK(vg::isValidUsername("Vedat_2025"));
    CHECK(vg::isValidUsername(std::string(20, 'z')));
    CHECK(vg::isValidUsername("____"));
    CHECK_FALSE(vg::isValidUsername("abc"));                  // too short
    CHECK_FALSE(vg::isValidUsername(std::string(21, 'z')));   // too long
    CHECK_FALSE(vg::isValidUsername("ab cd"));
    CHECK_FALSE(vg::isValidUsername("ab-cd"));
    CHECK_FALSE(vg::isValidUsername("çağrı1"));               // non-ASCII letters are not allowed
    CHECK_FALSE(vg::isValidUsername(std::string("abc\0d", 5)));
    CHECK_FALSE(vg::isValidUsername(""));

    CHECK(vg::usernameKey("Vedat_ABC") == "vedat_abc");
    CHECK(vg::usernameKey("VEDAT") == vg::usernameKey("vedat")); // plain ASCII case-insensitivity
    CHECK(vg::usernameKey("IIII") == "iiii");                   // usernames are ASCII: no Turkish folding
}

TEST_CASE("Password rules (bytes, not characters)", "[game][names]")
{
    CHECK_FALSE(vg::isValidPassword("12345"));
    CHECK(vg::isValidPassword("123456"));
    CHECK(vg::isValidPassword(std::string(64, 'p')));
    CHECK_FALSE(vg::isValidPassword(std::string(65, 'p')));
    CHECK(vg::isValidPassword("şşş"));                         // 3 characters, 6 bytes
    CHECK_FALSE(vg::isValidPassword("şş"));                    // 4 bytes
    CHECK_FALSE(vg::isValidPassword(std::string(32, 'x') + std::string(16, '\0') + "\xC3")); // bad UTF-8
}

TEST_CASE("Character name rules", "[game][names]")
{
    CHECK(vg::isValidCharacterName("Ali"));
    CHECK(vg::isValidCharacterName("Çağrı"));
    CHECK(vg::isValidCharacterName("İsmail"));
    CHECK(vg::isValidCharacterName("ılgaz"));
    CHECK(vg::isValidCharacterName("Ömer1"));
    CHECK(vg::isValidCharacterName("ŞÜÖĞÇİ"));
    CHECK(vg::isValidCharacterName("a12"));
    CHECK(vg::isValidCharacterName("ŞŞŞŞŞŞŞŞŞŞŞŞŞŞŞŞ"));       // 16 characters = 32 bytes: still valid
    CHECK(vg::isValidCharacterName("abcdefghijklmnop"));

    CHECK_FALSE(vg::isValidCharacterName("Al"));                // too short
    CHECK_FALSE(vg::isValidCharacterName("ŞŞ"));                // 2 characters (4 bytes)
    CHECK_FALSE(vg::isValidCharacterName("abcdefghijklmnopq")); // 17
    CHECK_FALSE(vg::isValidCharacterName("ŞŞŞŞŞŞŞŞŞŞŞŞŞŞŞŞŞ"));
    CHECK_FALSE(vg::isValidCharacterName("1Ali"));              // must start with a letter
    CHECK_FALSE(vg::isValidCharacterName("Ali_"));
    CHECK_FALSE(vg::isValidCharacterName("Ali Veli"));
    CHECK_FALSE(vg::isValidCharacterName("Àli"));               // letter outside the allowed set
    CHECK_FALSE(vg::isValidCharacterName("Ali\xCC\x87"));       // combining dot above
    CHECK_FALSE(vg::isValidCharacterName("Alî"));
    CHECK_FALSE(vg::isValidCharacterName("Ali١"));              // Arabic-Indic digit
    CHECK_FALSE(vg::isValidCharacterName("\xC0\x80" "Ali"));    // invalid UTF-8
    CHECK_FALSE(vg::isValidCharacterName(""));
}

TEST_CASE("Turkish-aware case folding for character name uniqueness", "[game][names]")
{
    // I <-> ı and İ <-> i
    CHECK(vg::characterNameKey("IRMAK") == "ırmak");
    CHECK(vg::characterNameKey("Irmak") == vg::characterNameKey("ırmak"));
    CHECK(vg::characterNameKey("İnci") == "inci");
    CHECK(vg::characterNameKey("İNCİ") == vg::characterNameKey("inci"));
    // Dotted and dotless i are different letters: these names do not collide.
    CHECK(vg::characterNameKey("Irmak") != vg::characterNameKey("irmak"));
    CHECK(vg::characterNameKey("İnci") != vg::characterNameKey("ınci"));
    // Other Turkish capitals.
    CHECK(vg::characterNameKey("ÇAĞRI") == "çağrı");
    CHECK(vg::characterNameKey("ŞÜÖĞ") == "şüöğ");
    CHECK(vg::characterNameKey("Ömer1") == "ömer1");
    // Already folded input is unchanged (idempotent).
    CHECK(vg::characterNameKey(vg::characterNameKey("İSMAİL IŞIK")) == vg::characterNameKey("İSMAİL IŞIK"));
    CHECK(vg::characterNameKey("\xC0\x80").empty());

    CHECK(vg::turkishFold(U'I') == U'ı');
    CHECK(vg::turkishFold(U'İ') == U'i');
    CHECK(vg::turkishFold(U'i') == U'i');
    CHECK(vg::turkishFold(U'ı') == U'ı');
    CHECK(vg::turkishFold(U'Z') == U'z');
}

TEST_CASE("Race, class and appearance validity", "[game][character]")
{
    CHECK(vg::isValidRace(1));
    CHECK(vg::isValidRace(2));
    CHECK_FALSE(vg::isValidRace(0));
    CHECK_FALSE(vg::isValidRace(3));
    for (std::uint8_t c = 1; c <= 4; ++c)
    {
        CHECK(vg::isAllowedRaceClass(static_cast<std::uint8_t>(vg::Race::Aydor), c));
        CHECK(vg::isAllowedRaceClass(static_cast<std::uint8_t>(vg::Race::Kargan), c));
    }
    CHECK_FALSE(vg::isAllowedRaceClass(1, 0));
    CHECK_FALSE(vg::isAllowedRaceClass(1, 5));
    CHECK_FALSE(vg::isAllowedRaceClass(3, 1));
    CHECK(vg::isValidAppearance(0, 0));
    CHECK(vg::isValidAppearance(3, 3));
    CHECK_FALSE(vg::isValidAppearance(4, 0));
    CHECK_FALSE(vg::isValidAppearance(0, 4));
    CHECK(vg::className(vg::CharClass::Priest) == "Şifacı");
    CHECK(vg::raceName(vg::Race::Kargan) == "Kargan");
}
