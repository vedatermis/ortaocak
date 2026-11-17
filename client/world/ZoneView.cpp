#include "client/world/ZoneView.h"

#include "shared/assets/Terrain.h"
#include "shared/game/Character.h"
#include "shared/game/Zones.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::world {

namespace {

constexpr float kGateLabelFullAlpha = 32.0f;
constexpr float kGatePropSearch = 4.0f; // a gate prop stands within this of its marker
constexpr float kDefaultGateHeight = 7.0f;
constexpr std::string_view kGatePropPrefix = "props/gate_";
constexpr std::string_view kMeshSuffix = ".vmesh";

} // namespace

std::string zoneName(const game::ZoneCatalog& catalog, std::uint16_t id)
{
    if (const game::ZoneDef* z = catalog.find(id))
        return z->name;
    return game::zoneDisplayName(id);
}

std::string zoneTagline(const game::ZoneCatalog& catalog, std::uint16_t id)
{
    if (const game::ZoneDef* z = catalog.find(id))
        return z->tagline;
    if (const game::ZoneInfo* z = game::findZone(id))
        return std::string(z->tagline);
    return {};
}

std::string zoneLoadingText(const game::ZoneCatalog& catalog, std::uint16_t id)
{
    return zoneName(catalog, id) + " yükleniyor…";
}

std::string zoneArrivalText(const game::ZoneCatalog& catalog, std::uint16_t id)
{
    return zoneName(catalog, id) + " bölgesine geçtin.";
}

std::string gateLabelText(const game::ZoneCatalog& catalog, std::uint16_t toZone)
{
    return zoneName(catalog, toZone) + " Geçidi";
}

std::string gatePassHint(const game::ZoneCatalog& catalog, std::uint16_t toZone)
{
    return "Geçide yürü: " + zoneName(catalog, toZone);
}

std::string_view zoneRaceDative(const game::ZoneDef& zone)
{
    const bool aydor = zone.allows(static_cast<std::uint8_t>(game::Race::Aydor));
    const bool kargan = zone.allows(static_cast<std::uint8_t>(game::Race::Kargan));
    if (aydor && !kargan)
        return "Aydorlulara";
    if (kargan && !aydor)
        return "Karganlara";
    return {};
}

std::string gateClosedText(const game::ZoneCatalog& catalog, std::uint16_t toZone)
{
    const game::ZoneDef* z = catalog.find(toZone);
    const std::string_view who = z != nullptr ? zoneRaceDative(*z) : std::string_view{};
    if (who.empty())
        return zoneName(catalog, toZone) + " sana kapalı."; // (no shipped zone: one race only, or both)
    return std::format("{} yalnızca {} açık.", zoneName(catalog, toZone), who);
}

std::string gateVeilMesh(std::string_view propMesh)
{
    if (!propMesh.starts_with(kGatePropPrefix) || !propMesh.ends_with(kMeshSuffix))
        return {};
    const std::string_view look =
        propMesh.substr(kGatePropPrefix.size(), propMesh.size() - kGatePropPrefix.size() - kMeshSuffix.size());
    if (look.empty() || look.find('/') != std::string_view::npos)
        return {};
    return std::format("effects/gate_veil_{}.vmesh", look);
}

const std::vector<std::string>& gateVeilMeshes()
{
    static const std::vector<std::string> kMeshes = {"effects/gate_veil_aydor.vmesh", "effects/gate_veil_kargan.vmesh",
                                                     "effects/gate_veil_esik.vmesh"};
    return kMeshes;
}

std::vector<GateView> buildGateViews(const assets::TerrainData& terrain, const game::ZoneCatalog& catalog,
                                     std::uint16_t zoneId, std::uint8_t race,
                                     const std::function<float(int placement)>& propHeight)
{
    std::vector<GateView> out;
    const game::ZoneDef* zone = catalog.find(zoneId);
    if (zone == nullptr)
        return out;
    for (const game::GateDef& g : zone->gates)
    {
        const assets::SpawnPoint* marker = terrain.findSpawn(g.marker);
        if (marker == nullptr)
            continue;
        GateView v;
        v.marker = g.marker;
        v.position = marker->position;
        v.yaw = marker->yaw;
        v.radius = g.radius;
        v.toZone = g.toZone;
        const game::ZoneDef* target = catalog.find(g.toZone);
        v.open = target != nullptr && target->allows(race);
        v.label = gateLabelText(catalog, g.toZone);
        v.hint = v.open ? gatePassHint(catalog, g.toZone) : gateClosedText(catalog, g.toZone);
        v.top = marker->position.y + kDefaultGateHeight;
        float best = kGatePropSearch;
        for (std::size_t i = 0; i < terrain.objects.size(); ++i)
        {
            const assets::ObjectPlacement& o = terrain.objects[i];
            if (!o.mesh.starts_with(kGatePropPrefix))
                continue;
            const float d = std::hypot(o.position.x - marker->position.x, o.position.z - marker->position.z);
            if (d <= best)
            {
                best = d;
                v.placement = static_cast<int>(i);
            }
        }
        if (v.placement >= 0)
        {
            const assets::ObjectPlacement& o = terrain.objects[static_cast<std::size_t>(v.placement)];
            v.veilMesh = gateVeilMesh(o.mesh);
            const float h = propHeight ? propHeight(v.placement) : 0.0f;
            if (h > 0.0f)
                v.top = o.position.y + h;
        }
        out.push_back(std::move(v));
    }
    return out;
}

const GateView* nearestGate(const std::vector<GateView>& gates, float x, float z, float range)
{
    const GateView* best = nullptr;
    float bestD = range;
    for (const GateView& g : gates)
    {
        const float d = std::hypot(g.position.x - x, g.position.z - z);
        if (d <= bestD)
        {
            bestD = d;
            best = &g;
        }
    }
    return best;
}

float gateLabelAlpha(float d)
{
    if (d <= kGateLabelFullAlpha)
        return 1.0f;
    if (d >= kGateLabelRange)
        return 0.0f;
    return 1.0f - (d - kGateLabelFullAlpha) / (kGateLabelRange - kGateLabelFullAlpha);
}

const std::vector<std::string_view>& zoneLoreLines(std::uint16_t zoneId)
{
    // docs/LORE.md; Ortaocak's lines are the M7 loading-screen lines.
    static const std::vector<std::string_view> kEsik = {
        "Ortaocak'ta eski bir gelenek gereği kan dökülmez.",
        "Ant Taşı hâlâ meydanda duruyor, ortasından çatlamış halde.",
        "Köz kırıkları bulundukları yeri ısıtır, taşıyana güç verir.",
        "Aydorlu Közaran yeminini mühürler; Karganlı adını ocak taşına kazır.",
        "Közyürek parçalandığı gece Uzun Kış bitti. Savaş bitmedi.",
        "Kırıkların düştüğü yerlerde kül kıpırdıyor.",
    };
    static const std::vector<std::string_view> kTanyeri = {
        "Tanyeri Burcu'nun sancağında soluk bir güneş dalgalanır.",
        "Ovada hasat üç yıl üst üste yandı; yanık tarlalarda hâlâ kül kıpırdıyor.",
        "Aydorlu bir Közaran yeminini yazar, mühürler ve tanık gösterir.",
        "Burcun ötesindeki yol Eşik'e, Eşik'ten her cepheye gider.",
    };
    static const std::vector<std::string_view> kDemiragiz = {
        "Demirağız'ın kapıları kayaya oyulmuştur; ocakları hiç sönmez.",
        "Karganlı bir Közaran adını boyunun ocak taşına kazır.",
        "Geçidin buzunda Közyürek'in eski ısısını arayan donuk gölgeler dolaşır.",
        "Kargan sözü yazılmaz, taşa kazınır.",
    };
    switch (zoneId)
    {
    case 2: return kTanyeri;
    case 3: return kDemiragiz;
    default: return kEsik;
    }
}

} // namespace vo::client::world
