// M13 "Üç Yurt" client logic (client/world/ZoneView.h, docs/PROTOCOL_ZONES.md §7): zone names and
// taglines from gamedata/zones.toml, the fixed Turkish texts, the veil of a gate prop, the gate views
// of a zone (marker, target, open / closed for a race, label, hint, prop and veil), the nearest gate,
// the label fade and the loading-screen lore lines.
#include "client/world/ZoneView.h"
#include "shared/assets/Terrain.h"
#include "shared/game/Character.h"
#include "shared/game/ZoneData.h"
#include "shared/game/Zones.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

namespace world = vo::client::world;
namespace vg = vo::game;
using Catch::Approx;

namespace {

constexpr std::uint8_t kAydor = static_cast<std::uint8_t>(vg::Race::Aydor);
constexpr std::uint8_t kKargan = static_cast<std::uint8_t>(vg::Race::Kargan);

const vg::ZoneCatalog& catalog()
{
    static const vg::ZoneCatalog c = [] {
        vg::ZoneCatalog out;
        const auto path = vg::locateZonesFile();
        REQUIRE(path.has_value());
        std::string error;
        REQUIRE(vg::loadZoneCatalog(*path, out, error));
        return out;
    }();
    return c;
}

// Zone 1's two gates with props next to their markers (one a little off), and a house far away.
vo::assets::TerrainData zone1Terrain()
{
    vo::assets::TerrainData t;
    t.spawns.push_back({"spawn_aydor", {250.0f, 20.0f, 270.0f}, 0.0f});
    t.spawns.push_back({"gate_aydor", {287.0f, 20.0f, 213.0f}, -0.6251f});
    t.spawns.push_back({"gate_kargan", {274.0f, 21.0f, 326.0f}, -1.5778f});
    vo::assets::ObjectPlacement house;
    house.mesh = "props/house_a.vmesh";
    house.position = {287.5f, 20.0f, 213.0f}; // a non-gate mesh right on the marker is never the gate
    t.objects.push_back(house);
    vo::assets::ObjectPlacement aydorGate;
    aydorGate.mesh = "props/gate_aydor.vmesh";
    aydorGate.position = {287.0f, 19.8f, 213.0f};
    t.objects.push_back(aydorGate);
    vo::assets::ObjectPlacement karganGate;
    karganGate.mesh = "props/gate_kargan.vmesh";
    karganGate.position = {275.5f, 21.0f, 326.5f};
    t.objects.push_back(karganGate);
    return t;
}

} // namespace

TEST_CASE("Zone names and taglines come from zones.toml, with fallbacks", "[client][zones]")
{
    const vg::ZoneCatalog& c = catalog();
    CHECK(world::zoneName(c, 1) == "Ortaocak");
    CHECK(world::zoneName(c, 2) == "Tanyeri");
    CHECK(world::zoneName(c, 3) == "Demirağız");
    CHECK(world::zoneTagline(c, 2) == "Aydor'un sınır yurdu");
    CHECK(world::zoneTagline(c, 3) == "Kargan'ın geçit yurdu");
    CHECK(world::zoneName(c, 77) == "Bölge 77");
    CHECK(world::zoneTagline(c, 77).empty());
    // An empty catalog (the file could not be read) still names the shipped zones.
    const vg::ZoneCatalog none;
    CHECK(world::zoneName(none, 3) == "Demirağız");
    CHECK(world::zoneTagline(none, 1) == "Eşik'in tarafsız köyü");
}

TEST_CASE("The fixed zone texts of PROTOCOL_ZONES.md §7", "[client][zones]")
{
    const vg::ZoneCatalog& c = catalog();
    CHECK(world::kGateCrossingText == "Geçitten geçiliyor…");
    CHECK(world::kZoneChangeFailedText == "Geçit şu an kullanılamıyor, tekrar dene.");
    CHECK(world::zoneLoadingText(c, 2) == "Tanyeri yükleniyor…");
    CHECK(world::zoneArrivalText(c, 1) == "Ortaocak bölgesine geçtin.");
    CHECK(world::gateLabelText(c, 2) == "Tanyeri Geçidi");
    CHECK(world::gatePassHint(c, 3) == "Geçide yürü: Demirağız");
    CHECK(world::gateClosedText(c, 2) == "Tanyeri yalnızca Aydorlulara açık.");
    CHECK(world::gateClosedText(c, 3) == "Demirağız yalnızca Karganlara açık.");
    CHECK(world::zoneRaceDative(*c.find(2)) == "Aydorlulara");
    CHECK(world::zoneRaceDative(*c.find(3)) == "Karganlara");
    CHECK(world::zoneRaceDative(*c.find(1)).empty());
}

TEST_CASE("A gate prop's veil mesh", "[client][zones]")
{
    CHECK(world::gateVeilMesh("props/gate_aydor.vmesh") == "effects/gate_veil_aydor.vmesh");
    CHECK(world::gateVeilMesh("props/gate_kargan.vmesh") == "effects/gate_veil_kargan.vmesh");
    CHECK(world::gateVeilMesh("props/gate_esik.vmesh") == "effects/gate_veil_esik.vmesh");
    CHECK(world::gateVeilMesh("props/house_a.vmesh").empty());
    CHECK(world::gateVeilMesh("props/gate_.vmesh").empty());
    CHECK(world::gateVeilMesh("props/gate_aydor.png").empty());
    CHECK(world::gateVeilMesh("props/gate_x/y.vmesh").empty());
    REQUIRE(world::gateVeilMeshes().size() == 3);
    for (const std::string& m : world::gateVeilMeshes())
        CHECK(m.starts_with("effects/gate_veil_"));
}

TEST_CASE("Gate views of Ortaocak for each race", "[client][zones]")
{
    const vg::ZoneCatalog& c = catalog();
    const vo::assets::TerrainData t = zone1Terrain();
    const auto height = [](int placement) { return placement == 1 ? 7.9f : 7.8f; };

    const std::vector<world::GateView> aydor = world::buildGateViews(t, c, 1, kAydor, height);
    REQUIRE(aydor.size() == 2);
    const world::GateView& toTanyeri = aydor[0];
    CHECK(toTanyeri.marker == "gate_aydor");
    CHECK(toTanyeri.toZone == 2);
    CHECK(toTanyeri.open);
    CHECK(toTanyeri.label == "Tanyeri Geçidi");
    CHECK(toTanyeri.hint == "Geçide yürü: Tanyeri");
    CHECK(toTanyeri.placement == 1); // the gate mesh, not the house standing closer
    CHECK(toTanyeri.veilMesh == "effects/gate_veil_aydor.vmesh");
    CHECK(toTanyeri.top == Approx(19.8f + 7.9f));
    CHECK(toTanyeri.yaw == Approx(-0.6251f));
    CHECK(toTanyeri.radius == Approx(2.5f));
    const world::GateView& toDemiragiz = aydor[1];
    CHECK(toDemiragiz.toZone == 3);
    CHECK_FALSE(toDemiragiz.open);
    CHECK(toDemiragiz.label == "Demirağız Geçidi");
    CHECK(toDemiragiz.hint == "Demirağız yalnızca Karganlara açık.");
    CHECK(toDemiragiz.placement == 2);
    CHECK(toDemiragiz.veilMesh == "effects/gate_veil_kargan.vmesh");

    const std::vector<world::GateView> kargan = world::buildGateViews(t, c, 1, kKargan);
    REQUIRE(kargan.size() == 2);
    CHECK_FALSE(kargan[0].open);
    CHECK(kargan[0].hint == "Tanyeri yalnızca Aydorlulara açık.");
    CHECK(kargan[1].open);
    CHECK(kargan[1].hint == "Geçide yürü: Demirağız");
    // Without prop heights the label sits 7 m over the marker.
    CHECK(kargan[1].top == Approx(21.0f + 7.0f));

    // A gate prop farther than 4 m from its marker is not taken: no veil, still a label.
    vo::assets::TerrainData far = t;
    far.objects[2].position = {280.0f, 21.0f, 326.0f};
    const auto noProp = world::buildGateViews(far, c, 1, kKargan);
    REQUIRE(noProp.size() == 2);
    CHECK(noProp[1].placement == -1);
    CHECK(noProp[1].veilMesh.empty());
    // A marker missing from the terrain: that gate is skipped.
    vo::assets::TerrainData missing = t;
    missing.spawns.pop_back();
    CHECK(world::buildGateViews(missing, c, 1, kAydor).size() == 1);
    // Unknown zone: none.
    CHECK(world::buildGateViews(t, c, 9, kAydor).empty());
}

TEST_CASE("Nearest gate, label fade and lore lines", "[client][zones]")
{
    const std::vector<world::GateView> gates = world::buildGateViews(zone1Terrain(), catalog(), 1, kAydor);
    REQUIRE(gates.size() == 2);
    const world::GateView* g = world::nearestGate(gates, 284.0f, 217.0f, world::kGateHintRange);
    REQUIRE(g != nullptr);
    CHECK(g->marker == "gate_aydor");
    CHECK(world::nearestGate(gates, 250.0f, 270.0f, world::kGateHintRange) == nullptr);
    CHECK(world::nearestGate(gates, 274.0f, 334.0f, world::kGateHintRange) != nullptr);  // exactly 8 m
    CHECK(world::nearestGate(gates, 274.0f, 334.1f, world::kGateHintRange) == nullptr);

    CHECK(world::gateLabelAlpha(0.0f) == Approx(1.0f));
    CHECK(world::gateLabelAlpha(32.0f) == Approx(1.0f));
    CHECK(world::gateLabelAlpha(36.0f) == Approx(0.5f));
    CHECK(world::gateLabelAlpha(40.0f) == Approx(0.0f));
    CHECK(world::gateLabelAlpha(55.0f) == Approx(0.0f));

    for (const int zone : {1, 2, 3, 99})
        CHECK_FALSE(world::zoneLoreLines(static_cast<std::uint16_t>(zone)).empty());
    CHECK(world::zoneLoreLines(2)[0] != world::zoneLoreLines(3)[0]);
    CHECK(world::zoneLoreLines(99)[0] == world::zoneLoreLines(1)[0]);
}
