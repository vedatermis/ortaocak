// gamedata/monsters.toml against the compiled zones (docs/PROTOCOL_COMBAT.md §1.3,
// docs/WORLD_ZONES_ART.md): in every zone each spawn point and its whole wander circle lies on
// walkable walk-mask cells, outside the safe zones, reachable from the town; the race spawns and
// gate arrival points are in a safe zone and out of every monster's reach; in zone 1 nothing
// aggressive is within reach of the paths the client flows walk (tools/dev/run-ui-flows.ps1: the
// plaza, the west house, world_tour's road down the plateau's north-east slope to 334/338).

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"
#include "shared/game/Monsters.h"
#include "shared/game/PathFind.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace vg = vo::game;

namespace {

fs::path assetDirForMonsters()
{
    char* value = nullptr;
    std::size_t size = 0;
    fs::path result = "assets";
    if (_dupenv_s(&value, &size, "VO_TEST_ASSET_DIR") == 0 && value != nullptr)
    {
        result = vo::core::pathFromUtf8(value);
        std::free(value);
    }
    return result;
}

const vo::assets::TerrainData& zoneTerrain(std::uint16_t zone)
{
    static std::map<std::uint16_t, vo::assets::TerrainData> cache;
    auto it = cache.find(zone);
    if (it == cache.end())
    {
        vo::assets::TerrainData d;
        std::string err;
        const std::string name = "zone" + std::to_string(zone);
        INFO(err);
        REQUIRE(vo::assets::loadTerrain(assetDirForMonsters() / "terrain" / name / (name + ".vterrain"), d, err));
        it = cache.emplace(zone, std::move(d)).first;
    }
    return it->second;
}

const vg::MonsterData& shipped()
{
    static const vg::MonsterData data = [] {
        vg::MonsterData d;
        std::string err;
        const auto path = vg::locateMonstersFile();
        REQUIRE(path);
        INFO(err);
        REQUIRE(vg::loadMonsterData(*path, d, err));
        return d;
    }();
    return data;
}

// Distance from (px, pz) to the segment a-b.
float segmentDistance(float px, float pz, float ax, float az, float bx, float bz)
{
    const float dx = bx - ax, dz = bz - az;
    const float len2 = dx * dx + dz * dz;
    float t = len2 > 0.0f ? ((px - ax) * dx + (pz - az) * dz) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(px - (ax + t * dx), pz - (az + t * dz));
}

// Per zone: the town plaza (paths start there) and the markers players stand on.
struct ZoneSpec
{
    std::uint16_t id;
    float plazaX, plazaZ;
    std::vector<const char*> safeMarkers; // race spawns and gate arrival points
};
const ZoneSpec kZones[] = {
    {1, 256.0f, 256.0f, {"spawn_aydor", "spawn_kargan", "arrive_aydor", "arrive_kargan"}},
    {2, 250.0f, 296.0f, {"spawn_aydor", "arrive_esik"}},
    {3, 256.0f, 344.0f, {"spawn_kargan", "arrive_esik"}},
};

} // namespace

TEST_CASE("Monster spawns: every spawn circle lies on walkable cells of its zone", "[content][monsters]")
{
    const auto& data = shipped();
    for (const ZoneSpec& z : kZones)
    {
        const auto& t = zoneTerrain(z.id);
        const float cell = t.worldSize / static_cast<float>(t.maskResolution);
        int checked = 0, spawns = 0;
        for (const auto& s : data.spawns())
        {
            if (s.zone != z.id)
                continue;
            ++spawns;
            INFO("zone " << z.id << " spawn of monster " << s.monster << " at " << s.x << ", " << s.z << " radius "
                         << s.radius);
            REQUIRE(t.isWalkable(s.x, s.z));
            // Every mask cell touching the circle (plus a small margin) must be walkable.
            const int i0 = static_cast<int>(std::floor((s.x - s.radius) / cell)) - 1;
            const int i1 = static_cast<int>(std::floor((s.x + s.radius) / cell)) + 1;
            const int j0 = static_cast<int>(std::floor((s.z - s.radius) / cell)) - 1;
            const int j1 = static_cast<int>(std::floor((s.z + s.radius) / cell)) + 1;
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i)
                {
                    const float cx = std::clamp(s.x, i * cell, (i + 1) * cell);
                    const float cz = std::clamp(s.z, j * cell, (j + 1) * cell);
                    if (std::hypot(cx - s.x, cz - s.z) > s.radius + 0.5f)
                        continue;
                    INFO("cell " << i << ", " << j);
                    CHECK(t.isWalkable((i + 0.5f) * cell, (j + 0.5f) * cell));
                    ++checked;
                }
            // Outside every safe zone, and reachable on foot from the town plaza.
            CHECK_FALSE(data.inSafeZone(z.id, s.x, s.z));
            CHECK(vg::findWalkPath(t, {z.plazaX, z.plazaZ}, {s.x, s.z}).has_value());
        }
        INFO("zone " << z.id);
        CHECK(spawns >= 15);
        CHECK(checked > 100);
    }
}

TEST_CASE("Monster spawns: race spawns and gate arrivals are safe and out of every monster's reach",
          "[content][monsters]")
{
    const auto& data = shipped();
    for (const ZoneSpec& z : kZones)
    {
        const auto& t = zoneTerrain(z.id);
        for (const char* name : z.safeMarkers)
        {
            INFO("zone " << z.id << " " << name);
            const auto* m = t.findSpawn(name);
            REQUIRE(m);
            CHECK(data.inSafeZone(z.id, m->position.x, m->position.z));
            // No wander circle reaches the marker's safe ground, and an idle monster at the edge of
            // its circle cannot notice a player standing 8 m from the marker (arrivals spread and
            // look round first). An elite chases up to its leash: its wander circle, aggro radius and
            // leash together stay 10 m clear of the marker (fix1: the Kor Bekçisi met new arrivals).
            for (const auto& s : data.spawns())
            {
                if (s.zone != z.id)
                    continue;
                const auto* tmpl = data.find(s.monster);
                REQUIRE(tmpl);
                INFO("monster " << s.monster << " at " << s.x << ", " << s.z);
                const float d = std::hypot(s.x - m->position.x, s.z - m->position.z);
                CHECK(d - s.radius - tmpl->aggroRadius > 8.0f);
                if (tmpl->elite)
                    CHECK(d - s.radius - tmpl->aggroRadius - tmpl->leashRadius > 10.0f);
            }
        }
    }
}

TEST_CASE("Monster spawns: in zone 1 the flows' paths stay out of reach", "[content][monsters]")
{
    const auto& t = zoneTerrain(1);
    const auto& data = shipped();
    // Nearest point (from the plaza) any monster can notice a player from while idle.
    float minReach = 1e9f;
    for (const auto& s : data.spawns())
        if (s.zone == 1)
            minReach = std::min(minReach, std::hypot(s.x - 256.0f, s.z - 256.0f) - s.radius - data.find(s.monster)->aggroRadius);
    for (const char* name : {"spawn_aydor", "spawn_kargan"})
    {
        const auto* s = t.findSpawn(name);
        REQUIRE(s);
        // A character spread 2 m around its spawn that walks 15 m away (the persist / world bot
        // scenarios) is never noticed by an idle monster.
        CHECK(std::hypot(s->position.x - 256.0f, s->position.z - 256.0f) + 17.0f < minReach);
    }
    // world_tour: plaza -> 334/338 and around it (the hill climb), with 10 m to spare beyond the
    // wander circle and the aggro radius.
    for (const auto& s : data.spawns())
    {
        if (s.zone != 1)
            continue;
        const auto* m = data.find(s.monster);
        REQUIRE(m);
        const float reach = s.radius + m->aggroRadius + 10.0f;
        INFO("monster " << s.monster << " at " << s.x << ", " << s.z);
        CHECK(segmentDistance(s.x, s.z, 256.0f, 256.0f, 334.0f, 338.0f) > reach);
        CHECK(std::hypot(s.x - 334.0f, s.z - 338.0f) > reach + 20.0f);
        // The west house of the flows (inside the safe zone): out of aggro reach with 5 m to spare.
        CHECK(std::hypot(s.x - 217.5f, s.z - 258.1f) > s.radius + m->aggroRadius + 5.0f);
    }
}

TEST_CASE("Monster spawns: the homelands' levels rise with the distance from the town", "[content][monsters]")
{
    const auto& data = shipped();
    for (const ZoneSpec& z : kZones)
    {
        if (z.id == 1)
            continue;
        // M15: levels 1-10 in the homelands; each band's nearest member lies beyond the previous band's
        // nearest (the slots read outwards from the town).
        float nearestHigh = 1e9f;
        std::map<int, float> nearestOfBand; // 0: 1-2, 1: 3-4, 2: 5-7, 3: 8-10
        for (const auto& s : data.spawns())
        {
            if (s.zone != z.id)
                continue;
            const auto* m = data.find(s.monster);
            REQUIRE(m);
            INFO("zone " << z.id << " monster " << s.monster);
            CHECK(m->level >= 1);
            CHECK(m->level <= 10);
            const float d = std::hypot(s.x - z.plazaX, s.z - z.plazaZ);
            if (m->level >= 3)
                nearestHigh = std::min(nearestHigh, d);
            else
                CHECK((d > 50.0f && d < 125.0f)); // level 1-2 slots just outside the town
            if (!m->elite)
            {
                const int band = m->level <= 2 ? 0 : m->level <= 4 ? 1 : m->level <= 7 ? 2 : 3;
                auto [it, fresh] = nearestOfBand.emplace(band, d);
                if (!fresh)
                    it->second = std::min(it->second, d);
            }
        }
        INFO("zone " << z.id);
        CHECK(nearestHigh > 100.0f); // levels 3-10 only well away from the town
        REQUIRE(nearestOfBand.size() == 4);
        CHECK(nearestOfBand[1] > nearestOfBand[0]);
        CHECK(nearestOfBand[2] > nearestOfBand[0]);
        CHECK(nearestOfBand[3] > nearestOfBand[1]);

    }
}
