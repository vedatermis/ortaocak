// Environment content checks on the real assets: zone 1 "Ortaocak" (terrain, layers, props,
// placements, walk mask, spawns, the Ant Tasi) and the M13 "Üç Yurt" zones 2 (Aydor march) and 3
// (Kargan pass) with the gates between them, built from assets_src by `assetc build` (ctest fixture
// `assets.build`; VO_TEST_ASSET_DIR / VO_TEST_ASSET_SRC as for ContentTests.cpp).
// tools/blender/gen_props.py, gen_zone_props.py, gen_terrain.py and gen_zones.py produce the
// sources; the rules checked here are the ones documented in docs/ASSET_PIPELINE.md §4-5 and
// docs/WORLD_ZONES_ART.md.

#include "tools/assetc/TerrainCompiler.h"

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace vo::assets;
namespace fs = std::filesystem;
namespace ac = vo::tools::assetc;

namespace {

fs::path envPath(const char* name, const char* fallback)
{
    char* value = nullptr;
    std::size_t size = 0;
    fs::path result = fallback;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr)
    {
        result = vo::core::pathFromUtf8(value);
        std::free(value);
    }
    return result;
}

fs::path assetDir() { return envPath("VO_TEST_ASSET_DIR", "assets"); }
fs::path sourceDir() { return envPath("VO_TEST_ASSET_SRC", "assets_src"); }

std::string zoneName(int zone) { return "zone" + std::to_string(zone); }

const TerrainData& zoneTerrain(int zone)
{
    static std::map<int, TerrainData> cache;
    auto it = cache.find(zone);
    if (it == cache.end())
    {
        TerrainData d;
        std::string err;
        INFO(err);
        REQUIRE(loadTerrain(assetDir() / "terrain" / zoneName(zone) / (zoneName(zone) + ".vterrain"), d, err));
        it = cache.emplace(zone, std::move(d)).first;
    }
    return it->second;
}

const TerrainData& zone1() { return zoneTerrain(1); }

const MeshData& prop(const std::string& path)
{
    static std::map<std::string, MeshData> cache;
    auto it = cache.find(path);
    if (it == cache.end())
    {
        MeshData m;
        std::string err;
        INFO(path << ": " << err);
        REQUIRE(loadMesh(assetDir() / path, m, err));
        it = cache.emplace(path, std::move(m)).first;
    }
    return it->second;
}

// Real-world size of every prop (metres, engine axes: width x, height y, depth z), its texture size,
// triangle budget (lod0 or the whole mesh), whether it has a far LOD and an emissive map, and the
// largest share of emissive texels (lanterns and windows: a few per cent; coal beds more).
struct PropSpec
{
    const char* name;
    float minW, maxW, minH, maxH, minD, maxD;
    std::uint32_t tex;
    std::uint32_t maxTris;
    bool lod;
    bool emissive;
    double maxLit = 0.06;
};
// Zone 1's props (gen_props.py, generated meshes).
const PropSpec kProps[] = {
    {"house_a", 5.5f, 6.5f, 7.6f, 8.4f, 7.2f, 8.2f, 2048, 14000, true, true},
    {"cottage", 6.5f, 7.3f, 5.3f, 6.2f, 5.6f, 6.4f, 2048, 14000, true, false},
    {"well", 1.9f, 2.4f, 2.7f, 3.1f, 2.3f, 2.8f, 1024, 7000, false, false},
    {"market_stall", 3.2f, 3.7f, 2.9f, 3.4f, 2.2f, 2.7f, 1024, 9000, false, true},
    {"ant_tasi", 2.3f, 2.9f, 4.7f, 5.1f, 1.7f, 2.2f, 2048, 12000, false, true},
    {"watchtower", 3.8f, 4.6f, 8.5f, 9.2f, 4.6f, 5.4f, 1024, 9000, true, true},
    {"fence", 3.9f, 4.1f, 1.1f, 1.35f, 0.2f, 0.4f, 512, 2400, false, false},
    {"lamp_post", 0.4f, 0.8f, 3.0f, 3.4f, 1.4f, 1.9f, 512, 3000, false, true},
    {"goods", 1.7f, 1.95f, 1.1f, 1.5f, 1.7f, 2.1f, 1024, 5000, false, false},
    {"signpost", 0.7f, 1.1f, 2.4f, 2.8f, 1.4f, 1.8f, 512, 3000, false, false},
    {"boulder", 1.9f, 2.3f, 1.3f, 1.8f, 1.8f, 2.3f, 1024, 3000, false, false},
    {"tree_oak", 6.0f, 8.0f, 8.6f, 9.4f, 6.0f, 8.0f, 2048, 12000, true, false},
    {"tree_pine", 5.5f, 7.5f, 11.5f, 12.5f, 5.5f, 7.5f, 2048, 10000, true, false},
};
// M13 props (gen_zone_props.py, procedural): the three gate looks and the homelands' settlements.
const PropSpec kM13Props[] = {
    {"gate_aydor", 5.6f, 6.2f, 7.4f, 8.2f, 1.6f, 2.0f, 1024, 3000, false, true},
    {"gate_kargan", 5.6f, 6.2f, 7.4f, 8.2f, 1.6f, 2.0f, 1024, 3000, false, true},
    {"gate_esik", 5.6f, 7.8f, 6.8f, 7.4f, 1.6f, 2.8f, 1024, 3000, false, true},
    {"aydor_wall", 5.9f, 6.2f, 4.9f, 5.4f, 1.8f, 2.2f, 1024, 2000, false, false},
    {"aydor_tower", 6.6f, 7.2f, 16.0f, 17.0f, 6.6f, 8.0f, 1024, 4000, false, true},
    {"aydor_house", 6.8f, 7.4f, 5.3f, 6.0f, 5.6f, 6.4f, 1024, 3000, false, true},
    {"aydor_banner", 1.1f, 1.4f, 4.6f, 5.1f, 0.5f, 0.8f, 512, 1000, false, false},
    {"kargan_door", 6.4f, 7.6f, 5.8f, 6.6f, 3.6f, 4.6f, 1024, 3000, false, true},
    {"kargan_forge", 4.2f, 4.8f, 4.3f, 4.9f, 1.6f, 2.0f, 1024, 2000, false, true, 0.12},
    {"kargan_brazier", 0.9f, 1.2f, 1.2f, 1.6f, 0.9f, 1.2f, 512, 1000, false, true, 0.30},
    {"kargan_palisade", 3.9f, 4.1f, 3.1f, 3.6f, 0.3f, 0.5f, 512, 1000, false, false},
    {"kargan_hut", 6.6f, 7.2f, 5.1f, 5.7f, 6.6f, 7.4f, 1024, 3000, false, true},
};

std::string meshPath(const char* name) { return std::string("props/") + name + ".vmesh"; }

bool isGate(const std::string& mesh) { return mesh.rfind("props/gate_", 0) == 0; }

// Footprint of a placed mesh: its bounds' x/z rectangle through the placement transform (a
// conservative superset of the real footprint the walk mask was built from).
std::array<Vec3, 4> footprint(const ObjectPlacement& o, const MeshData& m)
{
    const Mat4 w = o.worldMatrix();
    std::array<Vec3, 4> out{};
    const float xs[4] = {m.boundsMin.x, m.boundsMax.x, m.boundsMax.x, m.boundsMin.x};
    const float zs[4] = {m.boundsMin.z, m.boundsMin.z, m.boundsMax.z, m.boundsMax.z};
    for (int i = 0; i < 4; ++i)
        out[static_cast<std::size_t>(i)] = transformPoint(w, {xs[i], 0.0f, zs[i]});
    return out;
}

// Distance in the x/z plane from (x, z) to a convex quad (0 inside).
float distanceToQuad(float x, float z, const std::array<Vec3, 4>& q)
{
    bool inside = true;
    float best = 1e9f;
    float sign = 0.0f;
    for (std::size_t i = 0; i < 4; ++i)
    {
        const Vec3& p0 = q[i];
        const Vec3& p1 = q[(i + 1) % 4];
        const float ex = p1.x - p0.x, ez = p1.z - p0.z;
        const float cr = ex * (z - p0.z) - ez * (x - p0.x);
        if (sign == 0.0f)
            sign = cr >= 0.0f ? 1.0f : -1.0f;
        else if (cr * sign < 0.0f)
            inside = false;
        const float t = std::clamp(((x - p0.x) * ex + (z - p0.z) * ez) / (ex * ex + ez * ez), 0.0f, 1.0f);
        best = std::min(best, std::hypot(x - (p0.x + t * ex), z - (p0.z + t * ez)));
    }
    return inside ? 0.0f : best;
}

bool isTree(const std::string& mesh) { return mesh.find("tree_") != std::string::npos; }

// Props that stand at a road's verge and lean over it (or stand on the road's end: the homeland
// gates).
bool vergeProp(const std::string& mesh)
{
    return mesh == "props/lamp_post.vmesh" || mesh == "props/signpost.vmesh" || mesh == "props/kargan_brazier.vmesh" ||
           mesh == "props/aydor_banner.vmesh";
}

// Splat weight (0..1) of layer l at (x, z).
float splatWeight(const TerrainData& t, float x, float z, int l)
{
    const float cell = t.worldSize / static_cast<float>(t.splatResolution);
    const int i = std::clamp(static_cast<int>(x / cell), 0, static_cast<int>(t.splatResolution) - 1);
    const int j = std::clamp(static_cast<int>(z / cell), 0, static_cast<int>(t.splatResolution) - 1);
    return t.splat[(static_cast<std::size_t>(j) * t.splatResolution + static_cast<std::size_t>(i)) * 4 +
                   static_cast<std::size_t>(l)] /
           255.0f;
}

// Steepest slope (rise / run) around a mask cell, from the heights at its corners and neighbours.
float cellSlope(const TerrainData& t, int i, int j)
{
    float s = 0.0f;
    for (int dj = 0; dj <= 1; ++dj)
        for (int di = 0; di <= 1; ++di)
        {
            const int x = i + di, z = j + dj;
            const float gx = (t.sampleHeight(x + 1, z) - t.sampleHeight(x - 1, z)) * 0.5f;
            const float gz = (t.sampleHeight(x, z + 1) - t.sampleHeight(x, z - 1)) * 0.5f;
            s = std::max(s, std::hypot(gx, gz));
        }
    return s;
}

const ObjectPlacement& theStone()
{
    const ObjectPlacement* st = nullptr;
    for (const ObjectPlacement& o : zone1().objects)
        if (o.mesh == "props/ant_tasi.vmesh")
            st = &o;
    REQUIRE(st != nullptr);
    return *st;
}

constexpr float kPlazaX = 256.0f, kPlazaZ = 256.0f;
constexpr float kCharRadius = 0.35f; // footprints are grown by this in the walk mask (gen_terrain.py)

// The named markers of every zone (docs/WORLD_ZONES_ART.md): race spawns, gates with the gate prop
// standing on them and arrival points, the town plaza (centre of the town's safe ground).
struct GateSpec
{
    const char* gate;
    const char* arrive;
    const char* mesh;
};
struct ZoneSpec
{
    int id;
    float plazaX, plazaZ;
    std::vector<const char*> spawns;
    std::vector<GateSpec> gates;
};
const ZoneSpec kZones[] = {
    {1, 256.0f, 256.0f, {"spawn_aydor", "spawn_kargan"},
     {{"gate_aydor", "arrive_aydor", "props/gate_aydor.vmesh"}, {"gate_kargan", "arrive_kargan", "props/gate_kargan.vmesh"}}},
    {2, 250.0f, 296.0f, {"spawn_aydor"}, {{"gate_esik", "arrive_esik", "props/gate_esik.vmesh"}}},
    {3, 256.0f, 344.0f, {"spawn_kargan"}, {{"gate_esik", "arrive_esik", "props/gate_esik.vmesh"}}},
};

std::set<std::string> knownMeshes()
{
    std::set<std::string> known;
    for (const PropSpec& p : kProps)
        known.insert(meshPath(p.name));
    for (const PropSpec& p : kM13Props)
        known.insert(meshPath(p.name));
    return known;
}

} // namespace

TEST_CASE("zone 1 terrain: size, spawns, layers and placements", "[content][env]")
{
    const TerrainData& t = zone1();
    CHECK(t.worldSize == 512.0f);
    CHECK((t.resolution == 513 || t.resolution == 1025));
    const SpawnPoint* a = t.findSpawn("spawn_aydor");
    const SpawnPoint* k = t.findSpawn("spawn_kargan");
    REQUIRE(a != nullptr);
    REQUIRE(k != nullptr);
    const float apart = std::hypot(a->position.x - k->position.x, a->position.z - k->position.z);
    CHECK(apart >= 40.0f);
    CHECK(apart <= 60.0f);
    // Every placement references an existing static mesh of the prop sets; zone 1 uses every
    // zone 1 prop (and, since M13, the Aydor and Kargan gates).
    std::set<std::string> meshes;
    const std::set<std::string> known = knownMeshes();
    CHECK(t.objects.size() >= 300);
    for (const ObjectPlacement& o : t.objects)
    {
        INFO(o.mesh << " at " << o.position.x << ", " << o.position.z);
        meshes.insert(o.mesh);
        CHECK(known.contains(o.mesh));
        // Variation stays subtle: stretch within +-10 % (fence sections fitted to their run: 15 %).
        const float lim = o.mesh == "props/fence.vmesh" ? 0.15f : 0.10f;
        CHECK(std::abs(o.stretch.x - 1.0f) <= lim);
        CHECK(std::abs(o.stretch.y - 1.0f) <= 0.10f);
        CHECK(std::abs(o.stretch.z - 1.0f) <= 0.10f);
    }
    for (const PropSpec& p : kProps)
        CHECK(meshes.contains(meshPath(p.name)));
    CHECK(meshes.size() == std::size(kProps) + 2);
}

TEST_CASE("zones 1-3: size, outer margin, four PBR layers, placements of known props", "[content][env]")
{
    std::map<int, std::set<std::string>> layerSets;
    std::set<std::string> used;
    const std::set<std::string> known = knownMeshes();
    for (const ZoneSpec& z : kZones)
    {
        INFO("zone " << z.id);
        const TerrainData& t = zoneTerrain(z.id);
        CHECK(t.worldSize == 512.0f);
        CHECK(t.resolution == 513);
        REQUIRE(t.maskResolution == 512);
        // The outer margin is blocked: nobody walks off the map.
        for (float s = 0.5f; s < 512.0f; s += 16.0f)
        {
            CHECK_FALSE(t.isWalkable(s, 1.0f));
            CHECK_FALSE(t.isWalkable(1.0f, s));
            CHECK_FALSE(t.isWalkable(s, 510.5f));
            CHECK_FALSE(t.isWalkable(510.5f, s));
        }
        // Four PBR layers: base colour + normal + ORM, each compiled to .vtex (BC1 / BC5 / BC1, 1024^2,
        // full mips), tiles of 2-8 m.
        REQUIRE(t.layers.size() == 4);
        for (const TerrainLayer& l : t.layers)
        {
            INFO(l.texture);
            layerSets[z.id].insert(l.texture);
            CHECK(l.tileSize >= 2.0f);
            CHECK(l.tileSize <= 8.0f);
            const std::pair<const std::string*, TextureFormat> maps[] = {
                {&l.texture, TextureFormat::Bc1}, {&l.normalTexture, TextureFormat::Bc5}, {&l.ormTexture, TextureFormat::Bc1}};
            for (const auto& [path, format] : maps)
            {
                INFO(*path);
                REQUIRE_FALSE(path->empty());
                CHECK(fs::exists(assetDir() / *path));
                TextureData tex;
                std::string err;
                REQUIRE(loadTexture(assetDir() / compiledTexturePath(*path), tex, err));
                CHECK(tex.format == format);
                CHECK(tex.width() == 1024);
                CHECK(tex.mips.size() == 11);
            }
        }
        CHECK(t.objects.size() >= 200);
        for (const ObjectPlacement& o : t.objects)
        {
            INFO(o.mesh << " at " << o.position.x << ", " << o.position.z);
            used.insert(o.mesh);
            CHECK(known.contains(o.mesh));
            CHECK(t.contains(o.position.x, o.position.z));
            if (o.blocks())
                CHECK_FALSE(t.isWalkable(o.position.x, o.position.z));
            CHECK_FALSE(prop(o.mesh).isSkinned());
        }
    }
    // The three zones look different at a glance: no ground layer is shared between them.
    for (int a = 1; a <= 3; ++a)
        for (int b = a + 1; b <= 3; ++b)
            for (const std::string& l : layerSets[a])
            {
                INFO("zones " << a << " and " << b << ": " << l);
                CHECK_FALSE(layerSets[b].contains(l));
            }
    for (const PropSpec& p : kM13Props) // every M13 prop stands somewhere
    {
        INFO(p.name);
        CHECK(used.contains(meshPath(p.name)));
    }
}

TEST_CASE("props: real-world sizes, grounded bases, budgets, textures, LODs and emissive maps", "[content][env]")
{
    std::vector<PropSpec> specs(std::begin(kProps), std::end(kProps));
    specs.insert(specs.end(), std::begin(kM13Props), std::end(kM13Props));
    for (const PropSpec& p : specs)
    {
        INFO(p.name);
        const MeshData& m = prop(meshPath(p.name));
        CHECK_FALSE(m.isSkinned());
        REQUIRE(m.hasTangents());
        const Vec3 size = m.boundsMax - m.boundsMin;
        CHECK(size.x >= p.minW);
        CHECK(size.x <= p.maxW);
        CHECK(size.y >= p.minH);
        CHECK(size.y <= p.maxH);
        CHECK(size.z >= p.minD);
        CHECK(size.z <= p.maxD);
        // The base sits on the ground: slightly sunk (never floating), origin below the footprint.
        CHECK(m.boundsMin.y <= -0.02f);
        CHECK(m.boundsMin.y >= -0.30f);
        CHECK(std::abs(0.5f * (m.boundsMin.x + m.boundsMax.x)) < 0.5f * size.x);
        CHECK(std::abs(0.5f * (m.boundsMin.z + m.boundsMax.z)) < 0.5f * size.z);
        // A base: some vertices within 5 cm of the lowest point, spread over at least a fifth of the
        // footprint's shorter side (not a single spike).
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (std::uint32_t v = 0; v < m.vertexCount; ++v)
        {
            const Vec3 q = m.position(v);
            if (q.y > m.boundsMin.y + 0.05f)
                continue;
            lo[0] = std::min(lo[0], q.x);
            hi[0] = std::max(hi[0], q.x);
            lo[1] = std::min(lo[1], q.z);
            hi[1] = std::max(hi[1], q.z);
        }
        CHECK(std::max(hi[0] - lo[0], hi[1] - lo[1]) >= 0.2f * std::min(size.x, size.z));
        // One textured PBR material (per LOD level); textures at the expected sizes.
        std::uint32_t lod0 = 0, lod1 = 0, all = 0;
        for (const Submesh& s : m.submeshes)
        {
            all += s.indexCount / 3;
            lod0 += s.name == "lod0" ? s.indexCount / 3 : 0;
            lod1 += s.name == "lod1" ? s.indexCount / 3 : 0;
            CHECK(s.texture == std::string("props/") + p.name + ".png");
            CHECK(s.normalTexture == std::string("props/") + p.name + "_n.png");
            CHECK(s.ormTexture == std::string("props/") + p.name + "_orm.png");
            CHECK(s.occlusionStrength > 0.0f);
            CHECK(s.emissiveTexture.empty() != p.emissive);
            if (p.emissive)
            {
                // Lanterns, windows, embers: a few times the lit surface at most (subtle in daylight).
                CHECK(s.emissiveFactor[0] >= 1.0f);
                CHECK(s.emissiveFactor[0] <= 6.0f);
            }
            const std::pair<const std::string*, TextureUsage> maps[] = {{&s.texture, TextureUsage::Color},
                                                                        {&s.normalTexture, TextureUsage::Normal},
                                                                        {&s.ormTexture, TextureUsage::Data}};
            for (const auto& [path, usage] : maps)
            {
                INFO(*path);
                TextureData tex;
                std::string err;
                REQUIRE(loadTexture(assetDir() / compiledTexturePath(*path), tex, err));
                CHECK(tex.usage == usage);
                CHECK(tex.width() == p.tex);
                CHECK(tex.height() == p.tex);
            }
            if (!s.emissiveTexture.empty())
            {
                TextureData tex;
                std::string err;
                REQUIRE(loadTexture(assetDir() / compiledTexturePath(s.emissiveTexture), tex, err));
                CHECK(tex.width() <= p.tex);
                // Mostly dark: emission is a mask over a few parts of the atlas.
                std::vector<std::uint8_t> rgba;
                REQUIRE(decodeTextureMip(tex, 0, rgba, err));
                std::size_t lit = 0;
                for (std::size_t i = 0; i < rgba.size(); i += 4)
                    lit += (rgba[i] > 40 || rgba[i + 1] > 40) ? 1 : 0;
                const double frac = static_cast<double>(lit) / static_cast<double>(rgba.size() / 4);
                CHECK(frac > 0.0005);
                CHECK(frac < p.maxLit);
            }
        }
        if (p.lod)
        {
            CHECK(lod0 > 0);
            CHECK(lod1 > 0);
            CHECK(lod1 * 3 < lod0); // the far LOD is much cheaper
            CHECK(lod0 <= p.maxTris);
            CHECK(lod0 + lod1 == all);
        }
        else
        {
            CHECK(lod1 == 0);
            CHECK(all <= p.maxTris);
        }
    }
}

TEST_CASE("gate veils: a double-sided glowing sheet per gate look", "[content][env]")
{
    for (const char* look : {"aydor", "kargan", "esik"})
    {
        INFO(look);
        const std::string base = std::string("effects/gate_veil_") + look;
        const MeshData& m = prop(base + ".vmesh");
        CHECK_FALSE(m.isSkinned());
        REQUIRE(m.submeshes.size() == 1);
        CHECK(m.submeshes[0].emissiveTexture == base + "_e.png");
        // Fills the opening of the gate (3.4 m wide, ~5 m to the crown), origin on the ground.
        const Vec3 size = m.boundsMax - m.boundsMin;
        CHECK(size.x > 3.0f);
        CHECK(size.x < 3.5f);
        CHECK(size.y > 4.5f);
        CHECK(size.y < 5.3f);
        CHECK(size.z < 0.05f);
        CHECK(std::abs(m.boundsMin.y) < 0.05f);
    }
}

TEST_CASE("zones 1-3 placements sit on the ground, off the roads and clear of the markers", "[content][env]")
{
    for (const ZoneSpec& z : kZones)
    {
        const TerrainData& t = zoneTerrain(z.id);
        std::vector<const SpawnPoint*> markers;
        for (const SpawnPoint& s : t.spawns)
            markers.push_back(&s);
        for (const ObjectPlacement& o : t.objects)
        {
            INFO("zone " << z.id << ": " << o.mesh << " at " << o.position.x << ", " << o.position.z);
            const MeshData& m = prop(o.mesh);
            const auto quad = footprint(o, m);
            // Grounded: the mesh base is at or below the ground under its origin, and the placement
            // origin is not above the ground anywhere inside its footprint (sampled at 70 % of the
            // bounds rectangle, inside the solid base), so the base never floats on a slope.
            const float base = o.position.y + m.boundsMin.y * o.scale * o.stretch.y;
            CHECK(base <= t.heightAt(o.position.x, o.position.z) + 0.01f);
            if (!isTree(o.mesh) && !vergeProp(o.mesh))
                for (const Vec3& c : quad)
                {
                    const float x = o.position.x + 0.7f * (c.x - o.position.x);
                    const float zz = o.position.z + 0.7f * (c.z - o.position.z);
                    CHECK(o.position.y <= t.heightAt(x, zz) + 0.05f);
                }
            // Off the roads: no footprint over road ground (road weight > 1/2 outside the town plaza),
            // except the props that stand at the verge and the gates (the homeland gates stand where
            // their road ends).
            if (!vergeProp(o.mesh) && !isGate(o.mesh) &&
                std::hypot(o.position.x - z.plazaX, o.position.z - z.plazaZ) > 24.0f)
                CHECK(splatWeight(t, o.position.x, o.position.z, 1) < 0.5f);
            // Markers: no blocking footprint within 2 m (a gate marker stands in its own gate).
            if (o.blocks())
                for (const SpawnPoint* s : markers)
                {
                    INFO(s->name);
                    CHECK(distanceToQuad(s->position.x, s->position.z, quad) >= 2.0f);
                }
        }
    }
    // Zone 1's village: houses of both kinds, a market, lamps, fences, goods; groves of both trees.
    std::map<std::string, int> count;
    for (const ObjectPlacement& o : zone1().objects)
        ++count[o.mesh];
    CHECK(count["props/house_a.vmesh"] >= 5);
    CHECK(count["props/cottage.vmesh"] >= 4);
    CHECK(count["props/market_stall.vmesh"] >= 2);
    CHECK(count["props/lamp_post.vmesh"] >= 10);
    CHECK(count["props/fence.vmesh"] >= 15);
    CHECK(count["props/goods.vmesh"] >= 6);
    CHECK(count["props/signpost.vmesh"] >= 1);
    CHECK(count["props/tree_oak.vmesh"] >= 150);
    CHECK(count["props/tree_pine.vmesh"] >= 100);
    CHECK(count["props/boulder.vmesh"] >= 50);
    // House variants: some stretched and tinted, none identical in yaw to every other.
    int varied = 0;
    for (const ObjectPlacement& o : zone1().objects)
        if ((o.mesh == "props/house_a.vmesh" || o.mesh == "props/cottage.vmesh") &&
            o.tint != std::array<std::uint8_t, 4>{255, 255, 255, 255})
            ++varied;
    CHECK(varied >= 3);
    // The homelands' towns: Aydor white stone (keep tower, walls, houses, banners), Kargan rock and
    // iron (doors in the cliff, huts, forges, braziers, palisade).
    std::map<std::string, int> c2, c3;
    for (const ObjectPlacement& o : zoneTerrain(2).objects)
        ++c2[o.mesh];
    for (const ObjectPlacement& o : zoneTerrain(3).objects)
        ++c3[o.mesh];
    CHECK(c2["props/aydor_tower.vmesh"] == 1);
    CHECK(c2["props/aydor_house.vmesh"] >= 10);
    CHECK(c2["props/aydor_wall.vmesh"] >= 30);
    CHECK(c2["props/aydor_banner.vmesh"] >= 4);
    CHECK(c2["props/fence.vmesh"] >= 30); // the fields
    CHECK(c3["props/kargan_door.vmesh"] >= 3);
    CHECK(c3["props/kargan_hut.vmesh"] >= 8);
    CHECK(c3["props/kargan_forge.vmesh"] >= 2);
    CHECK(c3["props/kargan_brazier.vmesh"] >= 6);
    CHECK(c3["props/kargan_palisade.vmesh"] >= 20);
}

TEST_CASE("zones 1-3 walk mask: blocked cells are under a prop footprint, on steep ground or in the margin",
          "[content][env]")
{
    for (const ZoneSpec& z : kZones)
    {
        INFO("zone " << z.id);
        const TerrainData& t = zoneTerrain(z.id);
        REQUIRE(t.maskResolution == 512);
        // Footprint quads of the blocking placements and of the gates (their pillars block, the
        // opening does not), bucketed in 16 m cells.
        std::vector<std::array<Vec3, 4>> quads;
        std::map<std::pair<int, int>, std::vector<std::size_t>> grid;
        for (const ObjectPlacement& o : t.objects)
        {
            if (!o.blocks() && !isGate(o.mesh))
                continue;
            quads.push_back(footprint(o, prop(o.mesh)));
            float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
            for (const Vec3& c : quads.back())
            {
                x0 = std::min(x0, c.x);
                z0 = std::min(z0, c.z);
                x1 = std::max(x1, c.x);
                z1 = std::max(z1, c.z);
            }
            for (int gj = static_cast<int>(z0 - 2.0f) / 16; gj <= static_cast<int>(z1 + 2.0f) / 16; ++gj)
                for (int gi = static_cast<int>(x0 - 2.0f) / 16; gi <= static_cast<int>(x1 + 2.0f) / 16; ++gi)
                    grid[{gi, gj}].push_back(quads.size() - 1);
        }
        const float maxSlope = std::tan(36.0f * 3.14159265f / 180.0f);
        std::size_t blocked = 0, checked = 0, unexplained = 0;
        // A deterministic sample: every 7th cell of every 3rd row (~12 k cells).
        for (int j = 0; j < 512; j += 3)
            for (int i = (j * 5) % 7; i < 512; i += 7)
            {
                const float x = static_cast<float>(i) + 0.5f, zz = static_cast<float>(j) + 0.5f;
                if (t.isWalkable(x, zz))
                    continue;
                ++blocked;
                if (x < 7.0f || zz < 7.0f || x > 505.0f || zz > 505.0f)
                    continue; // outer margin
                if (cellSlope(t, i, j) > maxSlope * 0.97f)
                    continue; // steep
                ++checked;
                bool covered = false;
                const auto it = grid.find({i / 16, j / 16});
                if (it != grid.end())
                    for (std::size_t q : it->second)
                        covered = covered || distanceToQuad(x, zz, quads[q]) <= kCharRadius + 0.75f;
                if (!covered)
                {
                    ++unexplained;
                    INFO("blocked cell " << i << ", " << j << " slope " << cellSlope(t, i, j));
                    CHECK(covered);
                }
            }
        CHECK(blocked > 1000);
        CHECK(checked > 50); // the sample does reach prop footprints (zone 3: few on flat ground)
        CHECK(unexplained == 0);
        // The other way round: open ground well away from props on gentle slopes is walkable (no
        // blocked air).
        std::size_t open = 0, openBlocked = 0;
        for (int j = 20; j < 492; j += 9)
            for (int i = 20; i < 492; i += 9)
            {
                const float x = static_cast<float>(i) + 0.5f, zz = static_cast<float>(j) + 0.5f;
                if (cellSlope(t, i, j) > maxSlope * 0.8f)
                    continue;
                bool nearProp = false;
                const auto it = grid.find({i / 16, j / 16});
                if (it != grid.end())
                    for (std::size_t q : it->second)
                        nearProp = nearProp || distanceToQuad(x, zz, quads[q]) <= 2.5f;
                if (nearProp)
                    continue;
                ++open;
                openBlocked += t.isWalkable(x, zz) ? 0 : 1;
            }
        CHECK(open > 500);
        CHECK(openBlocked == 0);
    }
}

TEST_CASE("zones 1-3 markers: race spawns, gates and arrival points on walkable ground", "[content][env]")
{
    for (const ZoneSpec& z : kZones)
    {
        const TerrainData& t = zoneTerrain(z.id);
        std::vector<const char*> roomy(z.spawns.begin(), z.spawns.end());
        for (const GateSpec& g : z.gates)
            roomy.push_back(g.arrive);
        // Race spawns and arrival points: inside, walkable, at ground height, the 3 m around them
        // walkable; reachable on foot from the town plaza.
        for (const char* name : roomy)
        {
            INFO("zone " << z.id << " " << name);
            const SpawnPoint* s = t.findSpawn(name);
            REQUIRE(s != nullptr);
            CHECK(t.contains(s->position.x, s->position.z));
            CHECK(t.isWalkable(s->position.x, s->position.z));
            CHECK(std::abs(s->position.y - t.heightAt(s->position.x, s->position.z)) < 1e-3f);
            for (float dx = -3.0f; dx <= 3.0f; dx += 1.0f)
                for (float dz = -3.0f; dz <= 3.0f; dz += 1.0f)
                    CHECK(t.isWalkable(s->position.x + dx, s->position.z + dz));
        }
        for (const char* name : z.spawns) // new characters start in the town
        {
            const SpawnPoint* s = t.findSpawn(name);
            REQUIRE(s != nullptr);
            CHECK(std::hypot(s->position.x - z.plazaX, s->position.z - z.plazaZ) < 30.0f);
        }
        for (const GateSpec& g : z.gates)
        {
            INFO("zone " << z.id << " " << g.gate);
            const SpawnPoint* gate = t.findSpawn(g.gate);
            const SpawnPoint* arrive = t.findSpawn(g.arrive);
            REQUIRE(gate != nullptr);
            REQUIRE(arrive != nullptr);
            // The gate prop stands on the marker, facing the way the marker's yaw says.
            const ObjectPlacement* prop = nullptr;
            for (const ObjectPlacement& o : t.objects)
                if (o.mesh == g.mesh && std::hypot(o.position.x - gate->position.x, o.position.z - gate->position.z) < 0.05f)
                    prop = &o;
            REQUIRE(prop != nullptr);
            CHECK_FALSE(prop->blocks());
            CHECK(std::abs(std::remainder(prop->yaw - gate->yaw, 6.2831853f)) < 1e-3f);
            // The arrival point 6-10 m in front of the gate, facing away from it.
            const float dx = arrive->position.x - gate->position.x, dz = arrive->position.z - gate->position.z;
            const float d = std::hypot(dx, dz);
            CHECK(d >= 6.0f);
            CHECK(d <= 10.0f);
            CHECK(std::abs(std::remainder(std::atan2(dx, dz) - arrive->yaw, 6.2831853f)) < 0.05f);
            CHECK(std::abs(std::remainder(std::atan2(dx, dz) - gate->yaw, 6.2831853f)) < 0.05f);
            // The opening is walkable (half a metre either side of the centre line, 2 m through).
            const float fx = std::sin(gate->yaw), fz = std::cos(gate->yaw);
            for (float u : {-0.5f, 0.0f, 0.5f})
                for (float v : {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f})
                    CHECK(t.isWalkable(gate->position.x + fz * u + fx * v, gate->position.z - fx * u + fz * v));
            // A straight walkable path from the gate through the arrival point on to the road (the
            // road layer of the splat: a road or the village plaza), within 45 m.
            bool road = false;
            for (float s = 0.0f; s <= 45.0f && !road; s += 0.25f)
            {
                const float x = gate->position.x + fx * s, zz = gate->position.z + fz * s;
                INFO("path at " << x << ", " << zz);
                REQUIRE(t.isWalkable(x, zz));
                road = s >= d && splatWeight(t, x, zz, 1) > 0.5f;
            }
            CHECK(road);
            if (z.id == 1) // zone 1: a minute's walk from the plaza at most
                CHECK(std::hypot(gate->position.x - kPlazaX, gate->position.z - kPlazaZ) < 90.0f);
        }
    }
    // Zone 1: the Aydor gate south of the village, the Kargan gate north of it.
    CHECK(zone1().findSpawn("gate_aydor")->position.z < kPlazaZ - 40.0f);
    CHECK(zone1().findSpawn("gate_kargan")->position.z > kPlazaZ + 40.0f);
}

TEST_CASE("zone 1: the Ant Tasi stands in the plaza between the spawns, spawns clear of walls", "[content][env]")
{
    const TerrainData& t = zone1();
    const MeshData& stone = prop("props/ant_tasi.vmesh");
    const float height = stone.boundsMax.y - stone.boundsMin.y;
    CHECK(height > 4.6f); // stone ~4.4 m on its plinth
    CHECK(height < 5.2f);
    // The ember crack glows (emissive map), the rest is grey stone.
    REQUIRE(stone.submeshes.size() == 1);
    CHECK_FALSE(stone.submeshes[0].emissiveTexture.empty());

    int count = 0;
    for (const ObjectPlacement& o : t.objects)
        count += o.mesh == "props/ant_tasi.vmesh" ? 1 : 0;
    REQUIRE(count == 1);
    const ObjectPlacement& st = theStone();
    // About where it always stood: the north-west quarter of the plaza.
    CHECK(std::hypot(st.position.x - 249.0f, st.position.z - 263.5f) < 3.0f);
    const auto stoneQuad = footprint(st, stone);
    CHECK(st.blocks());
    CHECK_FALSE(t.isWalkable(st.position.x, st.position.z));
    for (const Vec3& corner : stoneQuad)
        CHECK_FALSE(t.isWalkable(st.position.x + 0.8f * (corner.x - st.position.x),
                                 st.position.z + 0.8f * (corner.z - st.position.z)));
    // The plaza around it is walkable and flat, and nothing else stands on or right next to it.
    for (int k = 0; k < 48; ++k)
    {
        const float a = 6.2831853f * static_cast<float>(k) / 48.0f;
        for (float r : {3.5f, 6.0f, 9.0f})
        {
            const float x = st.position.x + r * std::cos(a), z = st.position.z + r * std::sin(a);
            INFO("ring point " << x << ", " << z);
            CHECK(t.isWalkable(x, z));
            CHECK(std::abs(t.heightAt(x, z) - st.position.y) < 0.3f);
        }
    }
    for (const ObjectPlacement& o : t.objects)
    {
        if (&o == &st)
            continue;
        INFO(o.mesh << " at " << o.position.x << ", " << o.position.z);
        CHECK(distanceToQuad(o.position.x, o.position.z, stoneQuad) > 2.0f);
    }

    // Spawns: within 40 m of the stone and facing it (+-20 degrees), on the plateau, and no wall
    // (house, cottage or watchtower bounds: walls and eaves) within 8 m so the follow camera starts
    // outside.
    for (const char* name : {"spawn_aydor", "spawn_kargan"})
    {
        const SpawnPoint* s = t.findSpawn(name);
        REQUIRE(s != nullptr);
        INFO(name);
        CHECK(std::hypot(s->position.x - st.position.x, s->position.z - st.position.z) <= 40.0f);
        const float toStone = std::atan2(st.position.x - s->position.x, st.position.z - s->position.z);
        float diff = std::fmod(std::abs(toStone - s->yaw), 6.2831853f);
        diff = std::min(diff, 6.2831853f - diff);
        CHECK(diff < 20.0f * 3.14159265f / 180.0f);
        float wall = 1e9f;
        for (const ObjectPlacement& o : t.objects)
            if (o.mesh == "props/house_a.vmesh" || o.mesh == "props/cottage.vmesh" ||
                o.mesh == "props/watchtower.vmesh")
                wall = std::min(wall, distanceToQuad(s->position.x, s->position.z, footprint(o, prop(o.mesh))));
        CHECK(wall >= 8.0f);
        CHECK(std::abs(s->position.y - 20.0f) < 1.0f);
    }
}

TEST_CASE("homeland race spawns: no wall within 8 m (the follow camera starts outside)", "[content][env]")
{
    for (const ZoneSpec& z : kZones)
    {
        if (z.id == 1)
            continue;
        const TerrainData& t = zoneTerrain(z.id);
        for (const char* name : z.spawns)
        {
            INFO("zone " << z.id << " " << name);
            const SpawnPoint* s = t.findSpawn(name);
            REQUIRE(s != nullptr);
            float wall = 1e9f;
            for (const ObjectPlacement& o : t.objects)
                if (o.blocks() && prop(o.mesh).boundsMax.y > 2.5f && !isTree(o.mesh) &&
                    prop(o.mesh).boundsMax.x - prop(o.mesh).boundsMin.x > 2.0f) // walls, not posts
                    wall = std::min(wall, distanceToQuad(s->position.x, s->position.z, footprint(o, prop(o.mesh))));
            CHECK(wall >= 8.0f);
        }
    }
}

TEST_CASE("zones 1-3 terrain descriptions compile deterministically to the built .vterrain", "[content][env]")
{
    for (int zone = 1; zone <= 3; ++zone)
    {
        INFO("zone " << zone);
        const fs::path desc = sourceDir() / "terrain" / zoneName(zone) / (zoneName(zone) + ".terrain.toml");
        std::vector<std::byte> first, second;
        for (auto* out : {&first, &second})
        {
            TerrainData t;
            std::string err;
            INFO(err);
            REQUIRE(ac::compileTerrain(desc, t, err));
            REQUIRE(serializeTerrain(t, *out, err));
        }
        CHECK(first == second);
        std::vector<std::byte> built;
        std::string err;
        REQUIRE(readFileBytes(assetDir() / "terrain" / zoneName(zone) / (zoneName(zone) + ".vterrain"), built, err));
        CHECK(built == first);
    }
}
