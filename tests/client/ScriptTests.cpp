#include "client/automation/Script.h"

#include "client/platform/InputEvent.h"
#include "client/platform/KeyNames.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace vo::client;

TEST_CASE("Key names and chords", "[client][script]")
{
    REQUIRE(platform::keyFromName("VK_RETURN") == platform::keys::kEnter);
    REQUIRE(platform::keyFromName("enter") == platform::keys::kEnter);
    REQUIRE(platform::keyFromName("Tab") == platform::keys::kTab);
    REQUIRE(platform::keyFromName("a") == platform::keys::kA);
    REQUIRE(platform::keyFromName("7") == 0x37);
    REQUIRE(platform::keyFromName("F12") == 0x7B);
    REQUIRE_FALSE(platform::keyFromName("nope").has_value());

    const auto chord = platform::parseKeyChord("ctrl+shift+Left");
    REQUIRE(chord.has_value());
    REQUIRE(chord->key == platform::keys::kLeft);
    REQUIRE(chord->mods == (platform::mods::kCtrl | platform::mods::kShift));
    REQUIRE_FALSE(platform::parseKeyChord("hyper+a").has_value());
}

TEST_CASE("Script parses every command", "[client][script]")
{
    const std::string src = "\xEF\xBB\xBF# comment\n"
                            "wait 3\n"
                            "move 10 20\n"
                            "click 30 40\n"
                            "click 1 2 right\n"
                            "dblclick 5 6\n"
                            "mousedown 1 1\n"
                            "mouseup 2 2\n"
                            "wheel -1.5\n"
                            "key shift+tab\n"
                            "keydown ctrl\n"
                            "keyup ctrl\n"
                            "text \"Ağaç dalı \"\n"
                            "text şifre123\n"
                            "screenshot shots/01 login.png\n"
                            "resize 1024 640\n"
                            "uiscale 1.5\n"
                            "log hello\n"
                            "\n"
                            "quit\n";
    std::vector<automation::ScriptCommand> cmds;
    std::string error;
    REQUIRE(automation::parseScript(src, cmds, error));
    REQUIRE(cmds.size() == 18);
    using automation::ScriptOp;
    REQUIRE(cmds[0].op == ScriptOp::Wait);
    REQUIRE(cmds[0].a == 3);
    REQUIRE(cmds[0].line == 2);
    REQUIRE(cmds[2].op == ScriptOp::Click);
    REQUIRE(cmds[2].a == 30);
    REQUIRE(cmds[3].button == platform::MouseButton::Right);
    REQUIRE(cmds[7].value == -1.5f);
    REQUIRE(cmds[8].chord.key == platform::keys::kTab);
    REQUIRE(cmds[8].chord.mods == platform::mods::kShift);
    REQUIRE(cmds[11].text == "Ağaç dalı ");
    REQUIRE(cmds[12].text == "şifre123");
    REQUIRE(cmds[13].text == "shots/01 login.png");
    REQUIRE(cmds[14].op == ScriptOp::Resize);
    REQUIRE(cmds[14].b == 640);
    REQUIRE(cmds[15].value == 1.5f);
    REQUIRE(cmds[17].op == ScriptOp::Quit);
}

TEST_CASE("Script errors name the line", "[client][script]")
{
    std::vector<automation::ScriptCommand> cmds;
    std::string error;
    REQUIRE_FALSE(automation::parseScript("wait 1\nfly 1 2\n", cmds, error));
    REQUIRE(error.find("line 2") != std::string::npos);
    REQUIRE_FALSE(automation::parseScript("click 1\n", cmds, error));
    REQUIRE_FALSE(automation::parseScript("key notakey\n", cmds, error));
    REQUIRE_FALSE(automation::parseScript("wait -1\n", cmds, error));
    REQUIRE_FALSE(automation::parseScript("resize 0 10\n", cmds, error));
    REQUIRE_FALSE(automation::parseScript("quit now\n", cmds, error));
}
