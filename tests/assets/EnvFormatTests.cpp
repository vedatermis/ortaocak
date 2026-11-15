// Environment format additions (docs/ASSET_FORMATS.md): .vmesh v3 (alpha-tested and emissive
// materials), .vterrain v2 (per-layer normal + ORM maps, per-object stretch and tint), and the
// assetc side: glTF alphaMode MASK / emissive import, terrain descriptions with the new keys, and
// `assetc build` compiling terrain layer maps to .vtex.

#include "tests/assets/AssetTestUtil.h"
#include "tests/assets/GltfTestWriter.h"

#include "tools/assetc/Build.h"
#include "tools/assetc/GltfImport.h"
#include "tools/assetc/Image.h"
#include "tools/assetc/TerrainCompiler.h"

#include "shared/assets/Assets.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <format>

using namespace vo::assets;
using namespace vo::tests::assets;
namespace ac = vo::tools::assetc;
using Catch::Approx;

namespace {

MeshData makeV3Mesh()
{
    MeshData m = makeSampleMesh();
    m.submeshes[0].normalTexture = "props/a_n.png";
    m.submeshes[0].ormTexture = "props/a_orm.png";
    m.submeshes[0].roughnessFactor = 0.8f;
    m.submeshes[0].emissiveTexture = "props/a_e.png";
    m.submeshes[0].emissiveFactor = {4.0f, 3.0f, 2.0f};
    m.submeshes[1].flags = kSubmeshAlphaTest | kSubmeshDoubleSided;
    m.submeshes[1].alphaCutoff = 0.4f;
    return m;
}

// Every truncation and every single-byte corruption is rejected (the CRC catches the flips).
template <class Data, class Parse>
void rejectsTruncationsAndFlips(const std::vector<std::byte>& file, Parse parse)
{
    for (std::size_t n = 0; n < file.size(); ++n)
    {
        Data d;
        std::string err;
        INFO("prefix " << n);
        CHECK_FALSE(parse(std::span<const std::byte>(file.data(), n), d, err));
    }
    for (std::size_t i = 0; i < file.size(); i += 7)
    {
        std::vector<std::byte> bad = file;
        bad[i] ^= std::byte{0x5A};
        Data d;
        std::string err;
        INFO("flipped byte " << i);
        CHECK_FALSE(parse(bad, d, err));
    }
}

bool parses(std::vector<std::byte> bytes)
{
    fixCrc(bytes);
    MeshData d;
    std::string e;
    return parseMesh(bytes, d, e);
}

void writeSolid(const std::filesystem::path& p, std::array<std::uint8_t, 4> texel, int size = 4)
{
    ac::Image8 img{size, size, 4, {}};
    for (int i = 0; i < size * size; ++i)
        img.pixels.insert(img.pixels.end(), texel.begin(), texel.end());
    std::string err;
    REQUIRE(ac::writePng8(p, img, err));
}

// Unit quad in the glTF XY plane with the given material JSON (one embedded-buffer glTF).
std::filesystem::path writeQuadGltf(const std::filesystem::path& dir, const std::string& name,
                                    const std::string& materialJson, const std::string& imagesJson)
{
    PbrGltfWriter w;
    const int pos = w.floats({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}, "VEC3", true);
    const int nrm = w.floats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, "VEC3");
    const int tc = w.floats({0, 1, 1, 1, 1, 0, 0, 0}, "VEC2");
    const int idx = w.indices32({0, 1, 2, 0, 2, 3});
    const std::string json = std::format(
        R"({{"asset":{{"version":"2.0"}},"extensionsUsed":["KHR_materials_emissive_strength"],"scene":0,)"
        R"("scenes":[{{"nodes":[0]}}],"nodes":[{{"name":"lod0","mesh":0}}],)"
        R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":{},"NORMAL":{},"TEXCOORD_0":{}}},"indices":{},"material":0}}]}}],)"
        R"("materials":[{}],{}{})",
        pos, nrm, tc, idx, materialJson, imagesJson, w.buffersJson() + "}");
    const auto path = dir / (name + ".gltf");
    writeText(path, json);
    return path;
}

} // namespace

TEST_CASE("vmesh v3 round trip keeps alpha test and emission; plain meshes stay version 2", "[assets][format][env]")
{
    const MeshData m = makeV3Mesh();
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeMesh(m, bytes, err));
    CHECK(meshFileVersion(bytes) == 3);
    MeshData back;
    REQUIRE(parseMesh(bytes, back, err));
    CHECK(back.submeshes[0].emissiveTexture == "props/a_e.png");
    CHECK(back.submeshes[0].emissiveFactor == std::array<float, 3>{4.0f, 3.0f, 2.0f});
    CHECK(back.submeshes[0].normalTexture == "props/a_n.png"); // the v2 part is still read
    CHECK(back.submeshes[0].roughnessFactor == 0.8f);
    CHECK(back.submeshes[0].alphaCutoff == 0.0f);
    CHECK((back.submeshes[1].flags & kSubmeshAlphaTest) != 0);
    CHECK(back.submeshes[1].alphaCutoff == 0.4f);
    std::vector<std::byte> again;
    REQUIRE(serializeMesh(back, again, err));
    CHECK(again == bytes);
    // Submesh records are 104 bytes in v3.
    const auto submeshOffset = peek<std::uint32_t>(bytes, 92);
    const auto boneOffset = peek<std::uint32_t>(bytes, 96);
    CHECK(boneOffset - submeshOffset >= 2 * 104);

    // Without v3 data the writer keeps version 2 (byte-identical to before).
    MeshData plain = makeSampleMesh();
    plain.submeshes[0].normalTexture.clear();
    REQUIRE(serializeMesh(plain, bytes, err));
    CHECK(meshFileVersion(bytes) == kMeshVersion);
    // Versions 1 and 2 cannot hold v3 data.
    std::vector<std::byte> out;
    CHECK_FALSE(serializeMeshAs(m, 2, out, err));
    CHECK_FALSE(serializeMeshAs(m, 1, out, err));
    REQUIRE(serializeMeshAs(m, 3, out, err));
}

TEST_CASE("vmesh v3 rejects bad alpha cutoffs, alpha test with blending and bad emission", "[assets][format][env]")
{
    std::vector<std::byte> good;
    std::string err;
    REQUIRE(serializeMesh(makeV3Mesh(), good, err));
    REQUIRE(parses(good));
    const auto sub = peek<std::uint32_t>(good, 92);
    const std::size_t s1 = sub + 104; // second submesh (alpha-tested)
    auto withFloat = [&](std::size_t offset, float v) {
        std::vector<std::byte> b = good;
        patch(b, offset, v);
        return b;
    };
    CHECK_FALSE(parses(withFloat(s1 + 76, 0.0f)));  // alpha test needs a cutoff in (0, 1)
    CHECK_FALSE(parses(withFloat(s1 + 76, 1.0f)));
    CHECK_FALSE(parses(withFloat(s1 + 76, std::nanf(""))));
    CHECK_FALSE(parses(withFloat(sub + 76, 0.5f)));  // a cutoff without the alpha-test flag
    CHECK_FALSE(parses(withFloat(sub + 88, -1.0f))); // emission must be >= 0 and finite
    CHECK_FALSE(parses(withFloat(sub + 92, 1e6f)));
    CHECK_FALSE(parses(withFloat(sub + 96, std::nanf(""))));
    {
        std::vector<std::byte> b = good; // alpha test + alpha blend together
        patch(b, s1 + 42, static_cast<std::uint16_t>(kSubmeshAlphaTest | kSubmeshAlphaBlend));
        CHECK_FALSE(parses(b));
    }
    {
        std::vector<std::byte> b = good; // reserved word
        patch(b, sub + 100, std::uint32_t{1});
        CHECK_FALSE(parses(b));
    }
    {
        // The alpha-test flag is unknown in a version 2 file.
        MeshData v2 = makeSampleMesh();
        std::vector<std::byte> b;
        REQUIRE(serializeMeshAs(v2, 2, b, err));
        patch(b, peek<std::uint32_t>(b, 92) + 42, static_cast<std::uint16_t>(kSubmeshAlphaTest));
        CHECK_FALSE(parses(b));
    }
    rejectsTruncationsAndFlips<MeshData>(good, parseMesh);
}

TEST_CASE("assetc imports alphaMode MASK and emissive materials (texture, factor x strength)", "[assets][gltf][env]")
{
    TempDir dir("env_gltf");
    writeSolid(dir.path() / "c.png", {200, 150, 100, 128});
    writeSolid(dir.path() / "e.png", {255, 160, 60, 255});
    const std::string images = R"("textures":[{"source":0},{"source":1}],"images":[{"uri":"c.png"},{"uri":"e.png"}],)";
    const std::string mat =
        R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0.0,"roughnessFactor":0.9},)"
        R"("alphaMode":"MASK","alphaCutoff":0.35,"doubleSided":true,"emissiveTexture":{"index":1},)"
        R"("emissiveFactor":[1.0,0.5,0.25],"extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":4.0}}})";
    MeshData m;
    std::string err;
    std::vector<ac::TextureRequest> textures;
    INFO(err);
    REQUIRE(ac::importMesh(writeQuadGltf(dir.path(), "card", mat, images), {}, m, err, nullptr, &textures));
    REQUIRE(m.submeshes.size() == 1);
    const Submesh& s = m.submeshes[0];
    CHECK((s.flags & kSubmeshAlphaTest) != 0);
    CHECK((s.flags & kSubmeshDoubleSided) != 0);
    CHECK(s.alphaCutoff == Approx(0.35f));
    CHECK(s.emissiveTexture == "e.png");
    CHECK(s.emissiveFactor[0] == Approx(4.0f));
    CHECK(s.emissiveFactor[1] == Approx(2.0f));
    CHECK(s.emissiveFactor[2] == Approx(1.0f));
    CHECK(s.name == "lod0");
    bool emissiveRequest = false;
    for (const ac::TextureRequest& t : textures)
        emissiveRequest = emissiveRequest || (t.assetPath == "e.png" && t.usage == TextureUsage::Color);
    CHECK(emissiveRequest);
    std::vector<std::byte> bytes;
    REQUIRE(serializeMesh(m, bytes, err));
    CHECK(meshFileVersion(bytes) == 3);
    // The base colour with alpha < 255 compiles to BC3 (alpha kept for the test).
    REQUIRE(ac::writeMeshTextures(textures, dir.path() / "out", {}, nullptr, err));
    TextureData tex;
    REQUIRE(loadTexture(dir.path() / "out" / "c.vtex", tex, err));
    CHECK(tex.format == TextureFormat::Bc3);

    // A factor-only emissive material keeps its own submesh with the factor (not baked to colours).
    const std::string glow = R"({"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]},"emissiveFactor":[0.5,0.5,0.0]})";
    MeshData g;
    REQUIRE(ac::importMesh(writeQuadGltf(dir.path(), "glow", glow, ""), {}, g, err));
    REQUIRE(g.submeshes.size() == 1);
    CHECK(g.submeshes[0].emissiveFactor == std::array<float, 3>{0.5f, 0.5f, 0.0f});
    CHECK(g.submeshes[0].emissiveTexture.empty());

    // Bad: cutoff outside (0, 1); an emissive texture with a zero factor.
    const std::string badCut = R"({"alphaMode":"MASK","alphaCutoff":1.0})";
    CHECK_FALSE(ac::importMesh(writeQuadGltf(dir.path(), "badcut", badCut, ""), {}, g, err));
    const std::string zero = R"({"emissiveTexture":{"index":1},"emissiveFactor":[0,0,0]})";
    CHECK_FALSE(ac::importMesh(writeQuadGltf(dir.path(), "zero", zero, images), {}, g, err));
}

TEST_CASE("vterrain v2 keeps layer maps, stretch and tint; v1 files still load", "[assets][terrain][env]")
{
    TerrainData t;
    t.resolution = 3;
    t.worldSize = 2.0f;
    t.minHeight = 0.0f;
    t.maxHeight = 4.0f;
    t.heights.assign(9, 1000);
    t.splatResolution = 1;
    t.splat = {255, 0, 0, 0};
    t.maskResolution = 2;
    t.walkMask = {1, 1, 1, 0};
    t.layers.assign(4, {"textures/terrain/grass.png", 3.0f});
    ObjectPlacement o;
    o.mesh = "props/house_a.vmesh";
    o.position = {1.0f, 0.5f, 1.0f};
    o.yaw = 0.5f;
    o.scale = 1.0f;
    t.objects.push_back(o);

    // No v2 data: version 1 bytes, readable.
    std::vector<std::byte> v1;
    std::string err;
    REQUIRE(serializeTerrain(t, v1, err));
    CHECK(peek<std::uint16_t>(v1, 4) == 1);
    TerrainData back;
    REQUIRE(parseTerrain(v1, back, err));
    CHECK(back.layers[0].normalTexture.empty());
    CHECK(back.objects[0].stretch.x == 1.0f);
    CHECK(back.objects[0].tint == std::array<std::uint8_t, 4>{255, 255, 255, 255});

    t.layers[2].normalTexture = "textures/terrain/rock_n.png";
    t.layers[2].ormTexture = "textures/terrain/rock_orm.png";
    t.objects[0].stretch = {1.06f, 0.95f, 1.04f};
    t.objects[0].tint = {238, 228, 212, 255};
    std::vector<std::byte> v2;
    REQUIRE(serializeTerrain(t, v2, err));
    CHECK(peek<std::uint16_t>(v2, 4) == kTerrainVersion);
    REQUIRE(parseTerrain(v2, back, err));
    CHECK(back.layers[2].normalTexture == "textures/terrain/rock_n.png");
    CHECK(back.layers[2].ormTexture == "textures/terrain/rock_orm.png");
    CHECK(back.layers[2].tileSize == 3.0f);
    CHECK(back.layers[0].normalTexture.empty());
    CHECK(back.objects[0].stretch.x == 1.06f);
    CHECK(back.objects[0].stretch.y == 0.95f);
    CHECK(back.objects[0].tint == std::array<std::uint8_t, 4>{238, 228, 212, 255});
    CHECK(back.objects[0].maxScale() == Approx(1.06f));
    std::vector<std::byte> again;
    REQUIRE(serializeTerrain(back, again, err));
    CHECK(again == v2);

    // World matrix: T * RotY * S(scale * stretch): local +X (length 1) becomes 1.06 long.
    const Mat4 w = back.objects[0].worldMatrix();
    CHECK(length(transformVector(w, {1, 0, 0})) == Approx(1.06f));
    CHECK(length(transformVector(w, {0, 1, 0})) == Approx(0.95f));
    CHECK(length(transformVector(w, {0, 0, 1})) == Approx(1.04f));
    const Vec3 fwd = normalize(transformVector(w, {0, 0, 1}));
    CHECK(fwd.x == Approx(std::sin(0.5f)).margin(1e-5));
    CHECK(fwd.z == Approx(std::cos(0.5f)).margin(1e-5));

    // Rejections: stretch outside (0, 4], escaping layer map path.
    {
        TerrainData bad = t;
        bad.objects[0].stretch.y = 0.0f;
        std::vector<std::byte> b;
        CHECK_FALSE(serializeTerrain(bad, b, err));
    }
    {
        std::vector<std::byte> b = v2;
        const auto objectOffset = peek<std::uint32_t>(b, 72);
        patch(b, objectOffset + 32, 9.0f); // stretch.y
        fixCrc(b);
        CHECK_FALSE(parseTerrain(b, back, err));
    }
    {
        TerrainData bad = t;
        bad.layers[1].ormTexture = "../x.png";
        std::vector<std::byte> b;
        CHECK_FALSE(serializeTerrain(bad, b, err));
    }
    rejectsTruncationsAndFlips<TerrainData>(v2, parseTerrain);
}

TEST_CASE("terrain descriptions take layer normal/orm maps, stretch and tint; assetc build compiles the maps",
          "[assets][terrain][build][env]")
{
    TempDir src("env_terrain_src");
    TempDir out("env_terrain_out");
    std::string err;
    const auto dir = src.path() / "terrain" / "z";
    ac::Image8 hm{5, 5, 1, std::vector<std::uint8_t>(25, 100)};
    ac::Image8 splat{4, 4, 4, std::vector<std::uint8_t>(64, 0)};
    for (int k = 0; k < 16; ++k)
        splat.pixels[static_cast<std::size_t>(k) * 4] = 255;
    ac::Image8 mask{4, 4, 1, std::vector<std::uint8_t>(16, 255)};
    std::filesystem::create_directories(dir);
    REQUIRE(ac::writePng8(dir / "h.png", hm, err));
    REQUIRE(ac::writePng8(dir / "s.png", splat, err));
    REQUIRE(ac::writePng8(dir / "m.png", mask, err));
    std::filesystem::create_directories(src.path() / "t");
    writeSolid(src.path() / "t" / "g.png", {90, 120, 60, 255}, 8);
    writeSolid(src.path() / "t" / "g_n.png", {128, 128, 255, 255}, 8);
    writeSolid(src.path() / "t" / "g_orm.png", {200, 230, 0, 255}, 8);
    std::string desc = "[terrain]\nheightmap = \"h.png\"\nworld_size = 8.0\nheight_min = 0.0\nheight_max = 10.0\n"
                       "splat = \"s.png\"\nwalk_mask = \"m.png\"\n";
    for (int l = 0; l < 4; ++l)
        desc += "[[layer]]\ntexture = \"t/g.png\"\nnormal = \"t/g_n.png\"\norm = \"t/g_orm.png\"\ntile_size = 3.0\n";
    const std::string objects = "[[object]]\nmesh = \"props/q.vmesh\"\nx = 3.0\nz = 3.0\nstretch = [1.05, 0.96, 1.02]\n"
                                "tint = [230, 220, 200]\n";
    writeText(dir / "z.terrain.toml", desc + objects);
    TerrainData t;
    INFO(err);
    REQUIRE(ac::compileTerrain(dir / "z.terrain.toml", t, err));
    CHECK(t.layers[0].normalTexture == "t/g_n.png");
    CHECK(t.layers[3].ormTexture == "t/g_orm.png");
    REQUIRE(t.objects.size() == 1);
    CHECK(t.objects[0].stretch.x == Approx(1.05f));
    CHECK(t.objects[0].stretch.y == Approx(0.96f));
    CHECK(t.objects[0].tint == std::array<std::uint8_t, 4>{230, 220, 200, 255});
    // Bad values: stretch out of 0.25 .. 4, wrong arity, non-integer tint.
    const std::pair<const char*, const char*> bad[] = {
        {"stretch = [1.0, 5.0, 1.0]", "stretch"}, {"stretch = [1.0, 1.0]", "arity"}, {"tint = [10, 20, 30.5]", "tint"},
        {"tint = [10, 20, 300]", "tint range"}};
    for (const auto& [line, what] : bad)
    {
        INFO(what);
        writeText(dir / "bad.terrain.toml", desc + "[[object]]\nmesh = \"props/q.vmesh\"\nx = 3.0\nz = 3.0\n" + line + "\n");
        TerrainData b;
        CHECK_FALSE(ac::compileTerrain(dir / "bad.terrain.toml", b, err));
        std::filesystem::remove(dir / "bad.terrain.toml");
    }

    // `assetc build`: the referenced mesh must exist; layer maps get .vtex with the right usages.
    std::filesystem::create_directories(src.path() / "props");
    writeSolid(src.path() / "props" / "q.png", {200, 200, 200, 255});
    const std::string mat = R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}})";
    const std::string images = R"("textures":[{"source":0}],"images":[{"uri":"q.png"}],)";
    writeQuadGltf(src.path() / "props", "q", mat, images);
    ac::BuildOptions opt;
    ac::BuildStats stats;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, stats));
    CHECK(stats.failed == 0);
    TerrainData built;
    REQUIRE(loadTerrain(out.path() / "terrain" / "z" / "z.vterrain", built, err));
    CHECK(built.layers[1].normalTexture == "t/g_n.png");
    const std::pair<const char*, TextureUsage> maps[] = {
        {"g.vtex", TextureUsage::Color}, {"g_n.vtex", TextureUsage::Normal}, {"g_orm.vtex", TextureUsage::Data}};
    for (const auto& [file, usage] : maps)
    {
        INFO(file);
        TextureData tex;
        REQUIRE(loadTexture(out.path() / "t" / file, tex, err));
        CHECK(tex.usage == usage);
        CHECK(tex.width() == 8);
        CHECK(tex.mips.size() == 4);
    }
    // Up to date on a second run; a changed layer map makes the terrain item stale again.
    ac::BuildStats again;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, again));
    CHECK(again.converted == 0);
}
