#include "client/scenes/UiScale.h"
#include "client/scenes/Validation.h"

#include "shared/game/Names.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace vo::client::scenes;

namespace {
bool contains(const std::optional<std::string>& s, std::string_view part)
{
    return s && s->find(part) != std::string::npos;
}
} // namespace

TEST_CASE("Username validation gives specific Turkish messages", "[client][validation]")
{
    REQUIRE_FALSE(validateUsername("vedat").has_value());
    REQUIRE_FALSE(validateUsername("Ab_9").has_value());
    REQUIRE_FALSE(validateUsername(std::string(20, 'a')).has_value());

    REQUIRE(contains(validateUsername(""), "kullanıcı adınızı girin"));
    REQUIRE(contains(validateUsername("abc"), "en az 4"));
    REQUIRE(contains(validateUsername(std::string(21, 'a')), "en fazla 20"));
    REQUIRE(contains(validateUsername("ali veli"), "boşluk"));
    REQUIRE(contains(validateUsername("şükrü"), "Türkçe karakterler"));
    REQUIRE(contains(validateUsername("a-b-c-d"), "alt çizgi"));

    // Same verdict as the shared rule for a spread of inputs.
    const std::vector<std::string> samples = {"", "a", "abcd", "ABCD_1234", "abc!", "çağrı", std::string(20, 'x'),
                                              std::string(21, 'x'), "user name", "_____", "1234"};
    for (const auto& s : samples)
    {
        INFO(s);
        REQUIRE(validateUsername(s).has_value() == !vo::game::isValidUsername(s));
    }
}

TEST_CASE("Password validation gives specific Turkish messages", "[client][validation]")
{
    REQUIRE_FALSE(validatePassword("gizli123").has_value());
    REQUIRE_FALSE(validatePassword("şifre1").has_value()); // 6 characters, 8 bytes
    REQUIRE(contains(validatePassword(""), "şifrenizi girin"));
    REQUIRE(contains(validatePassword("12345"), "en az 6"));
    REQUIRE(contains(validatePassword(std::string(65, 'p')), "en fazla 64"));
    REQUIRE(contains(validatePassword("abc\xFF\xFE" "def"), "geçersiz"));

    const std::vector<std::string> samples = {"", "12345", "123456", std::string(64, 'p'), std::string(65, 'p'),
                                              "çççççç", "\xC3"};
    for (const auto& s : samples)
        REQUIRE(validatePassword(s).has_value() == !vo::game::isValidPassword(s));
}

TEST_CASE("Character name check reports each rule", "[client][validation]")
{
    auto ok = checkCharacterName("Gökalp");
    REQUIRE(ok.valid);
    REQUIRE(ok.lengthOk);
    REQUIRE(ok.charactersOk);
    REQUIRE(ok.startsWithLetter);
    REQUIRE(ok.length == 6);
    REQUIRE(ok.message.empty());

    auto empty = checkCharacterName("");
    REQUIRE(empty.empty);
    REQUIRE_FALSE(empty.valid);
    REQUIRE(empty.message.find("isim verin") != std::string::npos);

    auto digit = checkCharacterName("1abc");
    REQUIRE_FALSE(digit.valid);
    REQUIRE(digit.lengthOk);
    REQUIRE(digit.charactersOk);
    REQUIRE_FALSE(digit.startsWithLetter);
    REQUIRE(digit.message.find("harfle başlamalıdır") != std::string::npos);

    auto shortName = checkCharacterName("Al");
    REQUIRE_FALSE(shortName.lengthOk);
    REQUIRE(shortName.message.find("en az 3") != std::string::npos);

    auto longName = checkCharacterName("Abcdefghijklmnopq"); // 17
    REQUIRE_FALSE(longName.lengthOk);
    REQUIRE(longName.message.find("en fazla 16") != std::string::npos);

    auto symbol = checkCharacterName("Ali-Veli");
    REQUIRE_FALSE(symbol.charactersOk);
    REQUIRE(symbol.message.find("yalnızca harf ve rakam") != std::string::npos);

    auto space = checkCharacterName("Ali Veli");
    REQUIRE_FALSE(space.charactersOk);

    // 16 Turkish letters = 32 bytes: still valid (length counts characters).
    auto turkish = checkCharacterName("ÇĞİÖŞÜçğıöşüÇĞİÖ");
    REQUIRE(turkish.length == 16);
    REQUIRE(turkish.valid);

    auto broken = checkCharacterName("Ab\xC3");
    REQUIRE_FALSE(broken.valid);
    REQUIRE_FALSE(broken.charactersOk);

    const std::vector<std::string> samples = {"", "a", "ab", "abc", "Abc1", "1abc", "ab c", "Ömer", "ırmak",
                                              "İnci", "abcdefghijklmnop", "abcdefghijklmnopq", "a_b", "é", "Ab\xC3",
                                              "x9", "Kağan2"};
    for (const auto& s : samples)
    {
        INFO(s);
        const auto c = checkCharacterName(s);
        REQUIRE(c.valid == vo::game::isValidCharacterName(s));
        REQUIRE(c.message.empty() == c.valid);
        REQUIRE(c.valid == (c.lengthOk && c.charactersOk && c.startsWithLetter));
    }
}

TEST_CASE("UI scale fitting keeps the logical canvas large enough", "[client][layout]")
{
    REQUIRE(fitUiScale(1280.0f, 720.0f, 1.0f) == Catch::Approx(1.0f));
    REQUIRE(fitUiScale(1024.0f, 640.0f, 1.0f) == Catch::Approx(1.0f));
    REQUIRE(fitUiScale(1920.0f, 1080.0f, 1.5f) == Catch::Approx(1.5f));
    // 1024x640 at 150% would leave a 683x427 canvas: reduced until 960x600 fits.
    const float s = fitUiScale(1024.0f, 640.0f, 1.5f);
    REQUIRE(s < 1.5f);
    REQUIRE(1024.0f / s >= kMinLogicalWidth - 0.01f);
    REQUIRE(640.0f / s >= kMinLogicalHeight - 0.01f);
    REQUIRE(fitUiScale(100.0f, 100.0f, 1.0f) == Catch::Approx(kMinFitScale));
    REQUIRE(fitUiScale(0.0f, 0.0f, 1.25f) == Catch::Approx(1.25f));
}
