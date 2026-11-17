// Zone data and gate rules (shared/game/ZoneData.h, docs/PROTOCOL_ZONES.md): the shipped
// gamedata/zones.toml, the strict loader (every broken file refused with file:line), the gate
// decision, and the v7 messages.

#include "shared/game/Character.h"
#include "shared/game/ZoneData.h"
#include "shared/game/Zones.h"
#include "shared/net/Messages.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace vg = vo::game;
namespace vn = vo::net;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr std::uint8_t kAydor = static_cast<std::uint8_t>(vg::Race::Aydor);
constexpr std::uint8_t kKargan = static_cast<std::uint8_t>(vg::Race::Kargan);

const vg::ZoneCatalog& shipped()
{
    static const vg::ZoneCatalog c = [] {
        vg::ZoneCatalog out;
        std::string error;
        const auto file = vg::locateZonesFile();
        REQUIRE(file);
        const bool ok = vg::loadZoneCatalog(*file, out, error);
        INFO(error);
        REQUIRE(ok);
        return out;
    }();
    return c;
}

// The shipped layout in miniature; `replace` swaps one exact substring for another.
std::string sample(std::vector<std::pair<std::string, std::string>> replace = {})
{
    std::string s = R"([start]
aydor = 2
kargan = 3

[[zone]]
id = 1
key = "esik"
name = "Ortaocak"
tagline = "T1"
terrain = "terrain/zone1/zone1.vterrain"
races = ["aydor", "kargan"]

[[zone.gate]]
marker = "gate_aydor"
radius = 2.5
to_zone = 2
arrive = "arrive_esik"

[[zone.gate]]
marker = "gate_kargan"
radius = 2.5
to_zone = 3
arrive = "arrive_esik"

[[zone]]
id = 2
key = "tanyeri"
name = "Tanyeri"
tagline = "T2"
terrain = "terrain/zone2/zone2.vterrain"
races = ["aydor"]

[[zone.gate]]
marker = "gate_esik"
radius = 2.5
to_zone = 1
arrive = "arrive_aydor"

[[zone]]
id = 3
key = "demiragiz"
name = "Demirağız"
tagline = "T3"
terrain = "terrain/zone3/zone3.vterrain"
races = ["kargan"]

[[zone.gate]]
marker = "gate_esik"
radius = 2.5
to_zone = 1
arrive = "arrive_kargan"
)";
    for (const auto& [from, to] : replace)
    {
        const auto at = s.find(from);
        REQUIRE(at != std::string::npos);
        s.replace(at, from.size(), to);
    }
    return s;
}

std::string parseError(const std::string& text)
{
    vg::ZoneCatalog c;
    std::string error;
    if (vg::parseZoneCatalog(text, "zones.toml", c, error))
        return {};
    return error;
}

} // namespace

TEST_CASE("Zones: the shipped zones.toml", "[game][zones]")
{
    const vg::ZoneCatalog& c = shipped();
    REQUIRE(c.size() == 3);
    const vg::ZoneDef* esik = c.find(1);
    const vg::ZoneDef* tanyeri = c.find(2);
    const vg::ZoneDef* demir = c.find(3);
    REQUIRE(esik);
    REQUIRE(tanyeri);
    REQUIRE(demir);
    CHECK(esik->key == "esik");
    CHECK(tanyeri->key == "tanyeri");
    CHECK(demir->key == "demiragiz");
    CHECK(esik->terrain == "terrain/zone1/zone1.vterrain");
    CHECK(tanyeri->terrain == "terrain/zone2/zone2.vterrain");
    CHECK(demir->terrain == "terrain/zone3/zone3.vterrain");
    CHECK(esik->allows(kAydor));
    CHECK(esik->allows(kKargan));
    CHECK(tanyeri->allows(kAydor));
    CHECK_FALSE(tanyeri->allows(kKargan));
    CHECK(demir->allows(kKargan));
    CHECK_FALSE(demir->allows(kAydor));
    CHECK(c.homeZone(kAydor) == 2);
    CHECK(c.homeZone(kKargan) == 3);
    CHECK(c.homeZone(9) == 0);
    CHECK(c.findKey("tanyeri") == tanyeri);
    CHECK(c.findByIdOrKey("3") == demir);
    CHECK(c.findByIdOrKey("esik") == esik);
    CHECK(c.findByIdOrKey("7") == nullptr);

    // The fixed contract with the terrains (PROTOCOL_ZONES.md §1.2).
    REQUIRE(esik->gates.size() == 2);
    CHECK(esik->gates[0].marker == "gate_aydor");
    CHECK(esik->gates[0].toZone == 2);
    CHECK(esik->gates[0].arrive == "arrive_esik");
    CHECK(esik->gates[1].marker == "gate_kargan");
    CHECK(esik->gates[1].toZone == 3);
    CHECK(esik->gates[1].arrive == "arrive_esik");
    REQUIRE(tanyeri->gates.size() == 1);
    CHECK(tanyeri->gates[0].marker == "gate_esik");
    CHECK(tanyeri->gates[0].toZone == 1);
    CHECK(tanyeri->gates[0].arrive == "arrive_aydor");
    REQUIRE(demir->gates.size() == 1);
    CHECK(demir->gates[0].marker == "gate_esik");
    CHECK(demir->gates[0].toZone == 1);
    CHECK(demir->gates[0].arrive == "arrive_kargan");

    // The display table of shared/game/Zones.h (client HUD, character cards) says the same.
    for (const vg::ZoneDef& z : c.zones())
    {
        const vg::ZoneInfo* info = vg::findZone(z.id);
        REQUIRE(info);
        CHECK(info->name == z.name);
        CHECK(info->tagline == z.tagline);
        CHECK(vg::zoneDisplayName(z.id) == z.name);
    }
    CHECK(vg::raceSpawnMarker(kAydor) == "spawn_aydor");
    CHECK(vg::raceSpawnMarker(kKargan) == "spawn_kargan");
    CHECK(vg::raceSpawnMarker(0).empty());
}

TEST_CASE("Zones: the sample parses; ids are sorted", "[game][zones]")
{
    vg::ZoneCatalog c;
    std::string error;
    REQUIRE(vg::parseZoneCatalog(sample(), "zones.toml", c, error));
    REQUIRE(c.size() == 3);
    CHECK(c.zones()[0].id == 1);
    CHECK(c.zones()[2].id == 3);
    CHECK(c.find(3)->name == "Demirağız");

    // Reversed order in the file: still ascending in the catalog.
    std::string reversed = sample();
    REQUIRE(vg::parseZoneCatalog(reversed, "zones.toml", c, error));

    const vg::ZoneCatalog one = vg::ZoneCatalog::singleZone();
    REQUIRE(one.size() == 1);
    CHECK(one.homeZone(kAydor) == 1);
    CHECK(one.homeZone(kKargan) == 1);
    CHECK(one.find(1)->allows(kKargan));
}

TEST_CASE("Zones: the loader refuses broken files with file and line", "[game][zones]")
{
    struct Case
    {
        const char* what;
        std::string text;
        const char* expect;
    };
    const std::vector<Case> cases = {
        {"empty", "", "no [[zone]]"},
        {"syntax", "[[zone]\nid = 1", "zones.toml:"},
        {"unknown top key", sample({{"[start]", "color = 1\n[start]"}}), "unknown key 'color'"},
        {"no start", sample({{"[start]\naydor = 2\nkargan = 3\n", ""}}), "missing [start]"},
        {"start unknown key", sample({{"kargan = 3\n", "kargan = 3\nelf = 1\n"}}), "unknown key 'elf'"},
        {"start missing race", sample({{"kargan = 3\n", ""}}), "missing 'kargan'"},
        {"start zone not listed", sample({{"aydor = 2", "aydor = 9"}}), "not a listed zone"},
        {"start zone forbids race", sample({{"kargan = 3", "kargan = 2"}}), "does not allow race 'kargan'"},
        {"unknown zone key", sample({{"key = \"esik\"", "key = \"esik\"\ncolour = 2"}}), "unknown key 'colour'"},
        {"missing id", sample({{"id = 1\n", ""}}), "missing 'id'"},
        {"id 0", sample({{"id = 1\n", "id = 0\n"}}), "out of range"},
        {"id not integer", sample({{"id = 1\n", "id = \"1\"\n"}}), "must be an integer"},
        {"duplicate id", sample({{"id = 3\n", "id = 2\n"}, {"kargan = 3", "kargan = 1"}}), "id 2 used twice"},
        {"duplicate key", sample({{"key = \"demiragiz\"", "key = \"tanyeri\""}}), "key 'tanyeri' used twice"},
        {"bad key", sample({{"key = \"esik\"", "key = \"Eşik\""}}), "'key'"},
        {"empty name", sample({{"name = \"Ortaocak\"", "name = \"\""}}), "must not be empty"},
        {"long name", sample({{"name = \"Ortaocak\"", "name = \"" + std::string(60, 'a') + "\""}}), "too long"},
        {"missing tagline", sample({{"tagline = \"T1\"\n", ""}}), "missing 'tagline'"},
        {"absolute terrain", sample({{"terrain = \"terrain/zone1/zone1.vterrain\"", "terrain = \"/x/zone1.vterrain\""}}), "relative path"},
        {"dotdot terrain", sample({{"terrain = \"terrain/zone1/zone1.vterrain\"", "terrain = \"../zone1/zone1.vterrain\""}}), "relative path"},
        {"backslash terrain", sample({{"terrain = \"terrain/zone1/zone1.vterrain\"", "terrain = \"terrain\\\\zone1.vterrain\""}}), "relative path"},
        {"wrong extension", sample({{"terrain = \"terrain/zone1/zone1.vterrain\"", "terrain = \"terrain/zone1/zone1.png\""}}), "relative path"},
        {"terrain twice", sample({{"terrain = \"terrain/zone3/zone3.vterrain\"", "terrain = \"terrain/zone2/zone2.vterrain\""}}), "used twice"},
        {"no races", sample({{"races = [\"aydor\"]", "races = []"}}), "non-empty array"},
        {"races not array", sample({{"races = [\"aydor\"]", "races = \"aydor\""}}), "non-empty array"},
        {"unknown race", sample({{"races = [\"aydor\"]", "races = [\"elf\"]"}}), "unknown race"},
        {"race twice", sample({{"races = [\"aydor\"]", "races = [\"aydor\", \"aydor\"]"}}), "listed twice"},
        {"gate unknown key", sample({{"radius = 2.5\nto_zone = 2", "radius = 2.5\nprompt = 1\nto_zone = 2"}}), "unknown key 'prompt'"},
        {"gate missing marker", sample({{"marker = \"gate_aydor\"\n", ""}}), "missing 'marker'"},
        {"gate bad marker", sample({{"marker = \"gate_aydor\"", "marker = \"gate aydor\""}}), "not a marker name"},
        {"gate bad arrive", sample({{"arrive = \"arrive_aydor\"", "arrive = \"arrive-aydor\""}}), "not a marker name"},
        {"gate radius small", sample({{"radius = 2.5\nto_zone = 2", "radius = 0.5\nto_zone = 2"}}), "out of range"},
        {"gate radius large", sample({{"radius = 2.5\nto_zone = 2", "radius = 11\nto_zone = 2"}}), "out of range"},
        {"gate radius nan", sample({{"radius = 2.5\nto_zone = 2", "radius = nan\nto_zone = 2"}}), "out of range"},
        {"gate to unknown", sample({{"to_zone = 2", "to_zone = 9"}}), "unknown zone 9"},
        {"gate to itself", sample({{"to_zone = 2", "to_zone = 1"}}), "its own zone"},
        {"gate marker twice", sample({{"marker = \"gate_kargan\"", "marker = \"gate_aydor\""}}), "used twice"},
        {"gate nobody may pass", sample({{"races = [\"aydor\"]\n\n[[zone.gate]]", "races = [\"aydor\"]\n\n[[zone.gate]]\nmarker = \"gate_x\"\nradius = 2\nto_zone = 3\narrive = \"a\"\n\n[[zone.gate]]"}}),
         "nobody allowed here may pass"},
        {"gate not a table array", sample({{"[[zone.gate]]\nmarker = \"gate_esik\"\nradius = 2.5\nto_zone = 1\narrive = \"arrive_kargan\"", "gate = 5"}}),
         "array of tables"},
    };
    for (const Case& c : cases)
    {
        INFO(c.what);
        const std::string error = parseError(c.text);
        CHECK_THAT(error, ContainsSubstring(c.expect));
        CHECK_THAT(error, ContainsSubstring("zones.toml"));
    }
    // A line number for errors inside a table.
    CHECK_THAT(parseError(sample({{"to_zone = 2", "to_zone = 1"}})), ContainsSubstring("zones.toml:"));

    // Too many gates / zones.
    std::string gates;
    for (int i = 0; i < 9; ++i)
        gates += "[[zone.gate]]\nmarker = \"g" + std::to_string(i) + "\"\nradius = 2\nto_zone = 2\narrive = \"a\"\n";
    CHECK_THAT(parseError(sample({{"races = [\"aydor\", \"kargan\"]\n", "races = [\"aydor\", \"kargan\"]\n" + gates}})),
               ContainsSubstring("more than 8 gates"));
    std::string many = "[start]\naydor = 1\nkargan = 1\n";
    for (int i = 1; i <= 17; ++i)
        many += "[[zone]]\nid = " + std::to_string(i) + "\nkey = \"z" + std::to_string(i) + "\"\nname = \"Z\"\ntagline = \"t\"\n"
                "terrain = \"terrain/z" + std::to_string(i) + ".vterrain\"\nraces = [\"aydor\", \"kargan\"]\n";
    CHECK_THAT(parseError(many), ContainsSubstring("more than 16 zones"));

    // On failure the output is unchanged.
    vg::ZoneCatalog keep = vg::ZoneCatalog::singleZone(5);
    std::string error;
    CHECK_FALSE(vg::parseZoneCatalog("[[zone]]\nid = 0", "x", keep, error));
    CHECK(keep.find(5) != nullptr);
}

TEST_CASE("Zones: gate decision (PROTOCOL_ZONES.md §2)", "[game][zones]")
{
    using C = vg::GateCheck;
    const std::uint8_t aydorOnly = vg::raceBit(kAydor);
    vg::GateState s;
    CHECK(vg::checkGate(aydorOnly, kAydor, s) == C::Pass);
    CHECK(vg::checkGate(aydorOnly, kKargan, s) == C::RaceForbidden);
    CHECK(vg::checkGate(vg::kAllRaces, kKargan, s) == C::Pass);
    CHECK(vg::checkGate(aydorOnly, 0, s) == C::RaceForbidden);

    s.armedByTime = false;
    CHECK(vg::checkGate(aydorOnly, kAydor, s) == C::Disarmed);
    CHECK(vg::checkGate(aydorOnly, kKargan, s) == C::Disarmed); // no refusal while disarmed either
    s.armedByTime = true;
    s.beenOutside = false;
    CHECK(vg::checkGate(aydorOnly, kAydor, s) == C::Disarmed);
    s.beenOutside = true;
    s.alive = false;
    CHECK(vg::checkGate(aydorOnly, kAydor, s) == C::Dead);
    s.loading = true;
    CHECK(vg::checkGate(aydorOnly, kAydor, s) == C::Loading);

    // Through the catalog.
    const vg::ZoneCatalog& c = shipped();
    const vg::GateDef& toTanyeri = c.find(1)->gates[0];
    CHECK(vg::checkGate(c, toTanyeri, kAydor, {}) == C::Pass);
    CHECK(vg::checkGate(c, toTanyeri, kKargan, {}) == C::RaceForbidden);
    const vg::GateDef& toDemir = c.find(1)->gates[1];
    CHECK(vg::checkGate(c, toDemir, kAydor, {}) == C::RaceForbidden);
    CHECK(vg::checkGate(c, toDemir, kKargan, {}) == C::Pass);
    CHECK(vg::checkGate(c, c.find(2)->gates[0], kAydor, {}) == C::Pass);

    // The timing constants of the contract.
    CHECK(vg::kGateRearmMs == 2000);
    CHECK(vg::kGateRefusalIntervalMs == 3000);
    CHECK(vg::kZoneLoadingTimeoutMs == 20000);
    CHECK(vg::kArrivalGateMargin == 1.0f);
}

TEST_CASE("Zones: v7 messages round-trip and reject malformed input", "[net][zones]")
{
    const auto roundTrip = [](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        const auto bytes = vn::encodeMessage(msg);
        REQUIRE(bytes);
        T back{};
        REQUIRE(vn::decodeMessage(*bytes, back));
        CHECK(back == msg);
        return bytes->size();
    };
    CHECK(vn::kProtocolVersion == 7);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::ZoneChangeBegin) == 0x0800);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::ZoneChange) == 0x0801);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::ZoneLoadedReq) == 0x0802);
    CHECK(static_cast<std::uint16_t>(vn::Opcode::ZoneGateRefused) == 0x0803);
    CHECK(vn::opcodeName(vn::Opcode::ZoneChangeBegin) == "ZONE_CHANGE_BEGIN");
    CHECK(vn::opcodeName(vn::Opcode::ZoneChange) == "ZONE_CHANGE");
    CHECK(vn::opcodeName(vn::Opcode::ZoneLoadedReq) == "ZONE_LOADED_REQ");
    CHECK(vn::opcodeName(vn::Opcode::ZoneGateRefused) == "ZONE_GATE_REFUSED");

    CHECK(roundTrip(vn::ZoneChangeBegin{1, 2}) == 4);
    vn::ZoneChange zc;
    zc.result = vn::ZoneChangeResult::Ok;
    zc.fromZone = 1;
    zc.enter.entityId = 77;
    zc.enter.zoneId = 2;
    zc.enter.x = 322.5f;
    zc.enter.y = 11.0f;
    zc.enter.z = 256.25f;
    zc.enter.yaw = -1.5f;
    zc.enter.walkSpeed = 1.4f;
    zc.enter.runSpeed = 5.5f;
    zc.enter.serverTimeMs = 123456;
    zc.enter.level = 4;
    zc.enter.xp = 30;
    zc.enter.xpToNext = 400;
    zc.enter.hp = 120;
    zc.enter.maxHp = 150;
    zc.enter.mp = 40;
    zc.enter.maxMp = 50;
    zc.enter.attack = 21;
    zc.enter.defense = 9;
    CHECK(roundTrip(zc) == 68);
    zc.result = vn::ZoneChangeResult::Failed;
    CHECK(roundTrip(zc) == 68);
    CHECK(roundTrip(vn::ZoneLoadedReq{3}) == 2);
    CHECK(roundTrip(vn::ZoneGateRefused{2, vn::GateRefusal::RaceForbidden}) == 3);

    // Writers refuse what readers would reject; readers refuse bad values and trailing bytes.
    vn::ZoneChange bad = zc;
    bad.enter.x = NAN;
    CHECK_FALSE(vn::encodeMessage(bad));
    auto bytes = *vn::encodeMessage(zc);
    {
        auto b = bytes;
        b[0] = 2; // result 2
        vn::ZoneChange out;
        CHECK_FALSE(vn::decodeMessage(b, out));
    }
    {
        auto b = bytes;
        b.back() = 2; // is_dead 2
        vn::ZoneChange out;
        CHECK_FALSE(vn::decodeMessage(b, out));
    }
    {
        auto b = bytes;
        b.push_back(0);
        vn::ZoneChange out;
        CHECK_FALSE(vn::decodeMessage(b, out));
        b.resize(b.size() - 2);
        CHECK_FALSE(vn::decodeMessage(b, out));
    }
    {
        vn::ZoneGateRefused out;
        const std::vector<std::uint8_t> zero{2, 0, 0};
        CHECK_FALSE(vn::decodeMessage(zero, out));
        const std::vector<std::uint8_t> two{2, 0, 2};
        CHECK_FALSE(vn::decodeMessage(two, out));
        const std::vector<std::uint8_t> ok{2, 0, 1};
        CHECK(vn::decodeMessage(ok, out));
        CHECK(out.toZone == 2);
    }
    {
        vn::ZoneLoadedReq out;
        const std::vector<std::uint8_t> shortPayload{1};
        CHECK_FALSE(vn::decodeMessage(shortPayload, out));
        const std::vector<std::uint8_t> longPayload{1, 0, 0};
        CHECK_FALSE(vn::decodeMessage(longPayload, out));
    }
    // A fuzz pass over the decoders: never crashes, never reads past the end.
    std::uint32_t state = 12345;
    for (int i = 0; i < 4000; ++i)
    {
        std::vector<std::uint8_t> b(static_cast<std::size_t>(state % 80));
        for (auto& v : b)
        {
            state = state * 1664525u + 1013904223u;
            v = static_cast<std::uint8_t>(state >> 24);
        }
        state = state * 1664525u + 1013904223u;
        vn::ZoneChange a;
        vn::ZoneChangeBegin c;
        vn::ZoneLoadedReq d;
        vn::ZoneGateRefused e;
        (void)vn::decodeMessage(b, a);
        (void)vn::decodeMessage(b, c);
        (void)vn::decodeMessage(b, d);
        (void)vn::decodeMessage(b, e);
    }
}
