// gamedata/monsters.toml against the compiled zone 1 (docs/PROTOCOL_COMBAT.md §1.3): every spawn
// point and its whole wander circle lies on walkable walk-mask cells, outside the village safe
// zone, reachable from the village, and nothing aggressive is within reach of the paths the client
// flows walk (tools/dev/run-ui-flows.ps1: the plaza, the west house, world_tour's road down the
// plateau's north-east slope to 334/338).

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"
#include "shared/game/Monsters.h"
#include "shared/game/PathFind.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

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

const vo::assets::TerrainData& zone1()
{
    static const vo::assets::TerrainData t = [] {
        vo::assets::TerrainData d;
        std::string err;
        REQUIRE(vo::assets::loadTerrain(assetDirForMonsters() / "terrain" / "zone1" / "zone1.vterrain", d, err));
        return d;
    }();
    return t;
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

} // namespace

TEST_CASE("Monster spawns: every spawn circle lies on walkable cells of zone 1", "[content][monsters]")
{
    const auto& t = zone1();
    const auto& data = shipped();
    const float cell = t.worldSize / static_cast<float>(t.maskResolution);
    int checked = 0;
    for (const auto& s : data.spawns())
    {
        if (s.zone != 1)
            continue;
        INFO("spawn of monster " << s.monster << " at " << s.x << ", " << s.z << " radius " << s.radius);
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
        // Outside every safe zone, and reachable on foot from the village plaza.
        CHECK_FALSE(data.inSafeZone(1, s.x, s.z));
        CHECK(vg::findWalkPath(t, {256.0f, 256.0f}, {s.x, s.z}).has_value());
    }
    CHECK(checked > 100);
}

TEST_CASE("Monster spawns: the race spawns are in the safe zone; the flows' paths stay out of reach",
          "[content][monsters]")
{
    const auto& t = zone1();
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
        CHECK(data.inSafeZone(1, s->position.x, s->position.z));
        // A character spread 2 m around its spawn that walks 15 m away (the persist / world bot
        // scenarios) is never noticed by an idle monster.
        CHECK(std::hypot(s->position.x - 256.0f, s->position.z - 256.0f) + 17.0f < minReach);
    }
    // world_tour: plaza -> 334/338 and around it (the hill climb), with 10 m to spare beyond the
    // wander circle and the aggro radius.
    for (const auto& s : data.spawns())
    {
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
