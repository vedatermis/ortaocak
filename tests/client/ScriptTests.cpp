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

TEST_CASE("Script parses the state-aware commands", "[client][script]")
{
    const std::string src = "waitfor charselect+idle 600\n"
                            "expect modal\n"
                            "clickon login.submit\n"
                            "clickon select.slot1 30\n"
                            "expecttext messagebox.text \"Kullanıcı adı veya şifre hatalı.\"\n"
                            "expecttext world.rtt Gecikme\n";
    std::vector<automation::ScriptCommand> cmds;
    std::string error;
    REQUIRE(automation::parseScript(src, cmds, error));
    REQUIRE(cmds.size() == 6);
    using automation::ScriptOp;
    REQUIRE(cmds[0].op == ScriptOp::WaitFor);
    REQUIRE(cmds[0].text == "charselect+idle");
    REQUIRE(cmds[0].a == 600);
    REQUIRE(cmds[1].op == ScriptOp::Expect);
    REQUIRE(cmds[1].text == "modal");
    REQUIRE(cmds[2].op == ScriptOp::ClickOn);
    REQUIRE(cmds[2].text == "login.submit");
    REQUIRE(cmds[2].a == automation::kDefaultClickOnTimeoutFrames);
    REQUIRE(cmds[3].a == 30);
    REQUIRE(cmds[4].op == ScriptOp::ExpectText);
    REQUIRE(cmds[4].text == "messagebox.text");
    REQUIRE(cmds[4].arg == "Kullanıcı adı veya şifre hatalı.");
    REQUIRE(cmds[5].arg == "Gecikme");

    const auto tokens = automation::splitStateTokens("login+idle++modal+");
    REQUIRE(tokens.size() == 3);
    REQUIRE(tokens[0] == "login");
    REQUIRE(tokens[2] == "modal");
    REQUIRE(automation::splitStateTokens("+").empty());

    REQUIRE_FALSE(automation::parseScript("waitfor login\n", cmds, error));       // no timeout
    REQUIRE_FALSE(automation::parseScript("waitfor login 0\n", cmds, error));     // timeout must be > 0
    REQUIRE_FALSE(automation::parseScript("waitfor + 10\n", cmds, error));        // no token
    REQUIRE_FALSE(automation::parseScript("expect\n", cmds, error));
    REQUIRE_FALSE(automation::parseScript("clickon\n", cmds, error));
    REQUIRE_FALSE(automation::parseScript("clickon a.b soon\n", cmds, error));
    REQUIRE_FALSE(automation::parseScript("expecttext a.b\n", cmds, error));
    REQUIRE(error.find("line 1") != std::string::npos);
}

TEST_CASE("Script parses the M6 world commands (hold, drag, scene)", "[client][script]")
{
    const std::string src = "hold w 60\n"
                            "hold shift+d 5\n"
                            "drag 10 20 300 40 15 right\n"
                            "drag 1 2 3 4 1\n"
                            "scene camera behind 20 8\n";
    std::vector<automation::ScriptCommand> cmds;
    std::string error;
    REQUIRE(automation::parseScript(src, cmds, error));
    REQUIRE(cmds.size() == 5);
    CHECK(cmds[0].op == automation::ScriptOp::Hold);
    CHECK(cmds[0].chord.key == platform::keys::letter('W'));
    CHECK(cmds[0].frames == 60);
    CHECK(cmds[1].chord.mods == platform::mods::kShift);
    CHECK(cmds[2].op == automation::ScriptOp::Drag);
    CHECK(cmds[2].a == 10);
    CHECK(cmds[2].b == 20);
    CHECK(cmds[2].c == 300);
    CHECK(cmds[2].d == 40);
    CHECK(cmds[2].frames == 15);
    CHECK(cmds[2].button == platform::MouseButton::Right);
    CHECK(cmds[3].button == platform::MouseButton::Left); // default
    CHECK(cmds[4].op == automation::ScriptOp::Scene);
    CHECK(cmds[4].text == "camera behind 20 8");

    for (const char* bad : {"hold w\n", "hold w 0\n", "hold nokey 5\n", "drag 1 2 3 4\n", "drag 1 2 3 4 0\n",
                            "drag 1 2 3 4 5 sideways\n", "scene\n"})
    {
        INFO(bad);
        CHECK_FALSE(automation::parseScript(bad, cmds, error));
    }
}

TEST_CASE("Script parses the widget-addressed pointer commands (M8)", "[client][script]")
{
    std::vector<automation::ScriptCommand> cmds;
    std::string error;
    REQUIRE(automation::parseScript("rclickon inv.bag.3\n"
                                    "dblclickon inv.equip.main 30\n"
                                    "hoveron inv.bag.0\n"
                                    "dragon inv.bag.0 inv.equip.main\n"
                                    "dragon inv.bag.1 inv.bag.2 20\n",
                                    cmds, error));
    REQUIRE(cmds.size() == 5);
    CHECK(cmds[0].op == automation::ScriptOp::ClickOn);
    CHECK(cmds[0].button == platform::MouseButton::Right);
    CHECK(cmds[0].text == "inv.bag.3");
    CHECK(cmds[1].op == automation::ScriptOp::DoubleClickOn);
    CHECK(cmds[1].a == 30);
    CHECK(cmds[2].op == automation::ScriptOp::HoverOn);
    CHECK(cmds[3].op == automation::ScriptOp::DragOn);
    CHECK(cmds[3].text == "inv.bag.0");
    CHECK(cmds[3].arg == "inv.equip.main");
    CHECK(cmds[3].frames == 12);
    CHECK(cmds[4].frames == 20);
    for (const char* bad : {"dragon inv.bag.0\n", "dragon a b 0\n", "hoveron\n", "dblclickon a soon\n"})
    {
        INFO(bad);
        CHECK_FALSE(automation::parseScript(bad, cmds, error));
    }
}
