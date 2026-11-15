#include "client/net/NetText.h"

#include "shared/net/ClientSession.h"
#include "shared/net/CloseReason.h"
#include "shared/net/Opcodes.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace vo::client::net;
namespace vn = vo::net;

namespace {
// Player-facing text must be real Turkish sentences, never an English diagnostic or empty.
void requireTurkishText(const std::string& text)
{
    REQUIRE(text.size() > 10);
    REQUIRE(text.back() == '.');
}
} // namespace

TEST_CASE("Every ConnectResult failure has its own Turkish message", "[client][net][text]")
{
    std::set<std::string> texts;
    int known = 0;
    for (int i = 0; i <= 255; ++i)
    {
        const auto r = static_cast<vn::ConnectResult>(i);
        if (vn::connectResultName(r) == "unknown")
            continue;
        ++known;
        const UserMessage m = connectFailureMessage(r);
        if (r == vn::ConnectResult::Ok)
        {
            REQUIRE(m.text.empty());
            continue;
        }
        INFO("ConnectResult " << i);
        REQUIRE_FALSE(m.title.empty());
        requireTurkishText(m.text);
        REQUIRE(texts.insert(m.text).second); // distinct per result
    }
    REQUIRE(known == 11); // Ok .. Cancelled: update the mapping when ConnectResult grows

    REQUIRE(connectFailureMessage(vn::ConnectResult::VerificationFailed).text.find("doğrulanamadı") !=
            std::string::npos);
    REQUIRE(connectFailureMessage(vn::ConnectResult::ServerFull).text.find("dolu") != std::string::npos);
    REQUIRE(connectFailureMessage(vn::ConnectResult::Maintenance).text.find("bakım") != std::string::npos);
    REQUIRE(connectFailureMessage(vn::ConnectResult::ConnectFailed).text.find("ulaşılamadı") != std::string::npos);
    REQUIRE(connectFailureMessage(vn::ConnectResult::Timeout).text.find("zaman aşımı") != std::string::npos);
    requireTurkishText(missingServerKeyMessage().text);
}

TEST_CASE("Every LoginResult failure has its own Turkish message", "[client][net][text]")
{
    std::set<std::string> texts;
    for (int i = 0; i <= vn::kLoginResultMax; ++i)
    {
        const auto r = static_cast<vn::LoginResult>(i);
        const UserMessage m = loginFailureMessage(r);
        if (r == vn::LoginResult::Ok)
        {
            REQUIRE(m.text.empty());
            continue;
        }
        INFO("LoginResult " << i);
        REQUIRE_FALSE(m.title.empty());
        requireTurkishText(m.text);
        REQUIRE(texts.insert(m.text).second);
        REQUIRE(loginFailureMessage(static_cast<std::uint8_t>(i)).text == m.text);
    }
    REQUIRE(loginFailureMessage(vn::LoginResult::InvalidCredentials).text == "Kullanıcı adı veya şifre hatalı.");
    // Out-of-range raw values still get a sentence.
    requireTurkishText(loginFailureMessage(static_cast<std::uint8_t>(vn::kLoginResultMax + 1)).text);
}

TEST_CASE("Character, enter-world and leave-world results map to Turkish messages", "[client][net][text]")
{
    std::set<std::string> create;
    for (int i = 0; i <= vn::kCharCreateResultMax; ++i)
    {
        const auto r = static_cast<vn::CharCreateResult>(i);
        const std::string text = charCreateResultMessage(r);
        if (r == vn::CharCreateResult::Ok)
        {
            REQUIRE(text.empty());
            continue;
        }
        INFO("CharCreateResult " << i);
        requireTurkishText(text);
        REQUIRE(create.insert(text).second);
    }
    REQUIRE(charCreateResultMessage(vn::CharCreateResult::NameTaken).find("zaten kullanılıyor") != std::string::npos);
    REQUIRE(charCreateResultMessage(vn::CharCreateResult::NoFreeSlot).find("yuva") != std::string::npos);

    for (int i = 0; i <= vn::kCharDeleteResultMax; ++i)
    {
        const auto r = static_cast<vn::CharDeleteResult>(i);
        INFO("CharDeleteResult " << i);
        if (r == vn::CharDeleteResult::Ok)
            REQUIRE(charDeleteResultMessage(r).empty());
        else
            requireTurkishText(charDeleteResultMessage(r));
    }
    for (int i = 0; i <= vn::kEnterWorldResultMax; ++i)
    {
        const auto r = static_cast<vn::EnterWorldResult>(i);
        INFO("EnterWorldResult " << i);
        if (r == vn::EnterWorldResult::Ok)
            REQUIRE(enterWorldResultMessage(r).empty());
        else
            requireTurkishText(enterWorldResultMessage(r));
    }
    REQUIRE(leaveWorldResultMessage(vn::kLeaveWorldOk).empty());
    requireTurkishText(leaveWorldResultMessage(7));
}

TEST_CASE("Disconnect reasons map to Turkish messages", "[client][net][text]")
{
    std::set<std::string> texts;
    // Every ERROR code.
    for (int code = 0; code <= 0xFFFF; ++code)
    {
        const auto ec = static_cast<vn::ErrorCode>(code);
        if (vn::errorCodeName(ec) == "Unknown")
            continue;
        DisconnectInfo info;
        info.serverError = static_cast<std::uint16_t>(code);
        const UserMessage m = disconnectMessage(info);
        INFO("ErrorCode " << code);
        REQUIRE_FALSE(m.title.empty());
        requireTurkishText(m.text);
        texts.insert(m.text);
    }
    REQUIRE(texts.size() >= 6); // ProtocolViolation and BadState share one text

    DisconnectInfo kicked;
    kicked.serverError = static_cast<std::uint16_t>(vn::ErrorCode::KickedDuplicateLogin);
    REQUIRE(disconnectMessage(kicked).text.find("başka bir yerden") != std::string::npos);

    DisconnectInfo unknown;
    unknown.serverError = 999;
    REQUIRE(disconnectMessage(unknown).text.find("999") != std::string::npos);

    // Every close reason without an ERROR.
    for (int i = 0; i <= static_cast<int>(vn::CloseReason::OutboundOverflow); ++i)
    {
        DisconnectInfo info;
        info.closeReason = static_cast<vn::CloseReason>(i);
        INFO("CloseReason " << i);
        requireTurkishText(disconnectMessage(info).text);
    }
    DisconnectInfo lost;
    lost.closeReason = vn::CloseReason::NetworkError;
    REQUIRE(disconnectMessage(lost).text == "Sunucuyla bağlantı koptu.");

    DisconnectInfo timeout;
    timeout.cause = DisconnectCause::RequestTimeout;
    requireTurkishText(disconnectMessage(timeout).text);
    timeout.cause = DisconnectCause::PingTimeout;
    requireTurkishText(disconnectMessage(timeout).text);
}
