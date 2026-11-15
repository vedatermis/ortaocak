// glTF PBR material import with hand-made files and hand-computed answers: the tangent frame and the
// normal-map convention through the X mirror (a quad with a known normal-map texel must give a known
// engine-space shading normal, for both handedness signs, mirrored and flipped UVs, with glTF TANGENT
// and with generated MikkTSpace tangents), material factors, ORM packing, embedded images, tangent
// generation determinism, and `assetc build` producing .vtex files.

#include "tests/assets/AssetTestUtil.h"
#include "tests/assets/GltfTestWriter.h"

#include "tools/assetc/Build.h"
#include "tools/assetc/GltfImport.h"
#include "tools/assetc/Image.h"
#include "tools/assetc/Tangents.h"

#include "shared/assets/Assets.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <format>
#include <thread>

using namespace vo::assets;
using namespace vo::tests::assets;
namespace ac = vo::tools::assetc;

namespace {

std::uint8_t encUnit(float c)
{
    return static_cast<std::uint8_t>(std::lround((c * 0.5f + 0.5f) * 255.0f));
}

// 4x4 PNG filled with one RGBA texel.
void writeSolidPng(const std::filesystem::path& p, std::array<std::uint8_t, 4> texel)
{
    ac::Image8 img{4, 4, 4, {}};
    for (int i = 0; i < 16; ++i)
        img.pixels.insert(img.pixels.end(), texel.begin(), texel.end());
    std::string err;
    REQUIRE(ac::writePng8(p, img, err));
}

enum class UvMapping
{
    Normal,   // image right = glTF +X, image up = glTF +Y
    MirrorU,  // image right = glTF -X
    FlipV,    // image up = glTF -Y
};

// Quad in the glTF XY plane facing +Z (counter-clockwise seen from +Z), unit size.
std::filesystem::path writeQuad(const std::filesystem::path& dir, const std::string& name, UvMapping mapping,
                                bool withTangents, const std::string& materialJson, const std::string& imagesJson)
{
    PbrGltfWriter w;
    const int pos = w.floats({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}, "VEC3", true);
    const int nrm = w.floats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, "VEC3");
    // Corners: bottom-left, bottom-right, top-right, top-left (as seen from +Z).
    std::vector<float> uv;
    std::vector<float> tan;
    switch (mapping)
    {
    case UvMapping::Normal:
        uv = {0, 1, 1, 1, 1, 0, 0, 0};
        tan = {1, 0, 0, 1};
        break;
    case UvMapping::MirrorU:
        uv = {1, 1, 0, 1, 0, 0, 1, 0};
        tan = {-1, 0, 0, -1};
        break;
    case UvMapping::FlipV:
        uv = {0, 0, 1, 0, 1, 1, 0, 1};
        tan = {1, 0, 0, -1};
        break;
    }
    const int tc = w.floats(uv, "VEC2");
    std::vector<float> tangents;
    for (int i = 0; i < 4; ++i)
        tangents.insert(tangents.end(), tan.begin(), tan.end());
    const int ta = w.floats(tangents, "VEC4");
    const int idx = w.indices32({0, 1, 2, 0, 2, 3});
    const std::string attributes = withTangents
                                       ? std::format(R"("POSITION":{},"NORMAL":{},"TEXCOORD_0":{},"TANGENT":{})", pos, nrm, tc, ta)
                                       : std::format(R"("POSITION":{},"NORMAL":{},"TEXCOORD_0":{})", pos, nrm, tc);
    const std::string json = std::format(
        R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{{"name":"quad","mesh":0}}],)"
        R"("meshes":[{{"primitives":[{{"attributes":{{{}}},"indices":{},"material":0}}]}}],)"
        R"("materials":[{}],{}{})",
        attributes, idx, materialJson, imagesJson, w.buffersJson() + "}");
    const auto path = dir / (name + ".gltf");
    writeText(path, json);
    return path;
}

void checkVec(Vec3 got, Vec3 want, float eps)
{
    INFO("got (" << got.x << ", " << got.y << ", " << got.z << ") want (" << want.x << ", " << want.y << ", " << want.z
                 << ")");
    CHECK(std::abs(got.x - want.x) < eps);
    CHECK(std::abs(got.y - want.y) < eps);
    CHECK(std::abs(got.z - want.z) < eps);
}

constexpr const char* kNormalMaterial =
    R"({"normalTexture":{"index":0},"pbrMetallicRoughness":{"metallicFactor":0.25,"roughnessFactor":0.5}})";
constexpr const char* kNormalImages = R"("textures":[{"source":0}],"images":[{"uri":"n.png"}],)";

// Imports the quad, compiles its normal map, decodes the texel the shader would read and returns the
// engine-space shading normal at vertex 0 (NormalMapping.h = the shader's formulas).
Vec3 shadingNormal(const std::filesystem::path& gltf, const std::filesystem::path& out, bool compress)
{
    MeshData m;
    std::string err;
    std::vector<ac::TextureRequest> textures;
    REQUIRE(ac::importMesh(gltf, {}, m, err, nullptr, &textures));
    REQUIRE(m.hasTangents());
    REQUIRE(m.submeshes.size() == 1);
    REQUIRE(m.submeshes[0].normalTexture == "n.png");
    REQUIRE(textures.size() == 1);
    CHECK(textures[0].usage == TextureUsage::Normal);
    ac::TextureOptions opts;
    opts.compress = compress;
    REQUIRE(ac::writeMeshTextures(textures, out, opts, nullptr, err));
    TextureData tex;
    REQUIRE(loadTexture(out / "n.vtex", tex, err));
    CHECK(tex.format == (compress ? TextureFormat::Bc5 : TextureFormat::Rgba8));
    std::vector<std::uint8_t> rgba;
    REQUIRE(decodeTextureMip(tex, 0, rgba, err));
    const Vec3 ts = decodeNormalTexel(rgba[0] / 255.0f, rgba[1] / 255.0f, 1.0f);
    // Every vertex of the flat quad has the same frame.
    for (std::uint32_t v = 1; v < m.vertexCount; ++v)
    {
        const auto a = m.tangent(0), b = m.tangent(v);
        CHECK(a[3] == b[3]);
        CHECK(std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 1e-3f);
    }
    return perturbNormal(m.normal(0), m.tangent(0), ts);
}

} // namespace

TEST_CASE("normal map convention: known texel -> known engine-space normal through the X mirror", "[assets][gltf][pbr]")
{
    TempDir dir("gltf_nmap");
    // Tangent-space texel tilted towards image-right (+X) or image-up (+Y), OpenGL convention.
    struct Case
    {
        UvMapping mapping;
        Vec3 texel;
        Vec3 expectEngine; // hand-computed: glTF world normal, then x -> -x
        const char* what;
    };
    const Case cases[] = {
        // Image right = glTF +X: tilt right -> glTF (0.6, 0, 0.8) -> engine (-0.6, 0, 0.8).
        {UvMapping::Normal, {0.6f, 0.0f, 0.8f}, {-0.6f, 0.0f, 0.8f}, "normal UVs, +X texel"},
        // Image up = glTF +Y (green up): glTF (0, 0.6, 0.8) -> engine (0, 0.6, 0.8).
        {UvMapping::Normal, {0.0f, 0.6f, 0.8f}, {0.0f, 0.6f, 0.8f}, "normal UVs, +Y texel"},
        // Mirrored U (handedness -1): image right = glTF -X -> glTF (-0.6, 0, 0.8) -> engine (0.6, 0, 0.8).
        {UvMapping::MirrorU, {0.6f, 0.0f, 0.8f}, {0.6f, 0.0f, 0.8f}, "mirrored U, +X texel"},
        {UvMapping::MirrorU, {0.0f, 0.6f, 0.8f}, {0.0f, 0.6f, 0.8f}, "mirrored U, +Y texel"},
        // Flipped V (handedness -1): image up = glTF -Y -> glTF (0, -0.6, 0.8) -> engine (0, -0.6, 0.8).
        {UvMapping::FlipV, {0.0f, 0.6f, 0.8f}, {0.0f, -0.6f, 0.8f}, "flipped V, +Y texel"},
        {UvMapping::FlipV, {0.6f, 0.0f, 0.8f}, {-0.6f, 0.0f, 0.8f}, "flipped V, +X texel"},
    };
    int n = 0;
    for (const Case& c : cases)
    {
        writeSolidPng(dir.path() / "n.png", {encUnit(c.texel.x), encUnit(c.texel.y), encUnit(c.texel.z), 255});
        for (const bool withTangents : {true, false})
        {
            for (const bool compress : {false, true})
            {
                INFO(c.what << (withTangents ? ", glTF TANGENT" : ", MikkTSpace") << (compress ? ", BC5" : ", RGBA8"));
                const auto gltf = writeQuad(dir.path(), std::format("q{}", n++), c.mapping, withTangents,
                                            kNormalMaterial, kNormalImages);
                checkVec(shadingNormal(gltf, dir.path() / "out", compress), c.expectEngine, compress ? 0.03f : 0.01f);
            }
        }
    }
}

TEST_CASE("generated tangents match the glTF TANGENT convention and are deterministic", "[assets][gltf][pbr]")
{
    TempDir dir("gltf_tan");
    writeSolidPng(dir.path() / "n.png", {128, 128, 255, 255});
    for (const UvMapping mapping : {UvMapping::Normal, UvMapping::MirrorU, UvMapping::FlipV})
    {
        MeshData given, generated;
        std::string err;
        REQUIRE(ac::importMesh(writeQuad(dir.path(), "given", mapping, true, kNormalMaterial, kNormalImages), {}, given,
                               err));
        REQUIRE(ac::importMesh(writeQuad(dir.path(), "gen", mapping, false, kNormalMaterial, kNormalImages), {},
                               generated, err));
        const auto a = given.tangent(0), b = generated.tangent(0);
        checkVec({b[0], b[1], b[2]}, {a[0], a[1], a[2]}, 1e-3f);
        CHECK(a[3] == b[3]);
        // Perpendicular to the normal, unit length.
        CHECK(std::abs(dot(Vec3{b[0], b[1], b[2]}, generated.normal(0))) < 1e-3f);
        std::vector<std::byte> x, y;
        REQUIRE(serializeMesh(generated, x, err));
        MeshData again;
        REQUIRE(ac::importMesh(dir.path() / "gen.gltf", {}, again, err));
        REQUIRE(serializeMesh(again, y, err));
        CHECK(x == y);
    }
    // Degenerate UVs (all zero) still give unit tangents perpendicular to the normal.
    std::vector<std::array<float, 4>> corner;
    const std::vector<Vec3> pos{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const std::vector<Vec3> nrm{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    const std::vector<std::array<float, 2>> uv{{0, 0}, {0, 0}, {0, 0}};
    const std::vector<std::uint32_t> tris{0, 1, 2};
    REQUIRE(ac::generateTangents(pos, nrm, uv, tris, corner));
    for (const auto& t : corner)
    {
        CHECK(std::abs(std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) - 1.0f) < 1e-4f);
        CHECK(std::abs(t[2]) < 1e-4f);
    }
}

TEST_CASE("glTF material import: factors, ORM packing and embedded images", "[assets][gltf][pbr]")
{
    TempDir dir("gltf_pbrmat");
    writeSolidPng(dir.path() / "c.png", {200, 100, 50, 255});
    writeSolidPng(dir.path() / "n.png", {128, 128, 255, 255});
    writeSolidPng(dir.path() / "orm.png", {90, 180, 30, 255});
    writeSolidPng(dir.path() / "mr.png", {77, 160, 220, 255});
    writeSolidPng(dir.path() / "ao.png", {40, 99, 99, 255});
    const std::string images = R"("textures":[{"source":0},{"source":1},{"source":2},{"source":3},{"source":4}],)"
                               R"("images":[{"uri":"c.png"},{"uri":"n.png"},{"uri":"orm.png"},{"uri":"mr.png"},{"uri":"ao.png"}],)";
    std::string err;

    // Art contract: one ORM image in both slots -> used as is (no generated PNG).
    {
        const std::string mat =
            R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":2},)"
            R"("metallicFactor":1.0,"roughnessFactor":0.8},"normalTexture":{"index":1,"scale":0.7},)"
            R"("occlusionTexture":{"index":2,"strength":0.9}})";
        MeshData m;
        std::vector<ac::TextureRequest> tex;
        REQUIRE(ac::importMesh(writeQuad(dir.path(), "contract", UvMapping::Normal, false, mat, images), {}, m, err,
                               nullptr, &tex));
        const Submesh& s = m.submeshes[0];
        CHECK(s.texture == "c.png");
        CHECK(s.normalTexture == "n.png");
        CHECK(s.ormTexture == "orm.png");
        CHECK(s.normalScale == 0.7f);
        CHECK(s.roughnessFactor == 0.8f);
        CHECK(s.metallicFactor == 1.0f);
        CHECK(s.occlusionStrength == 0.9f);
        REQUIRE(tex.size() == 3);
        for (const auto& t : tex)
            CHECK_FALSE(t.writePng);
        CHECK(tex[2].orm == ac::TextureRequest::Orm::AsIs);
        REQUIRE(ac::writeMeshTextures(tex, dir.path() / "out", {}, nullptr, err));
        TextureData c, n, o;
        REQUIRE(loadTexture(dir.path() / "out" / "c.vtex", c, err));
        REQUIRE(loadTexture(dir.path() / "out" / "n.vtex", n, err));
        REQUIRE(loadTexture(dir.path() / "out" / "orm.vtex", o, err));
        CHECK(c.usage == TextureUsage::Color);
        CHECK(c.format == TextureFormat::Bc1);
        CHECK(n.format == TextureFormat::Bc5);
        CHECK(o.usage == TextureUsage::Data);
        std::vector<std::uint8_t> rgba;
        REQUIRE(decodeTextureMip(o, 0, rgba, err));
        CHECK(std::abs(rgba[1] - 180) <= 4); // roughness in G survives BC1 (6 bits)
    }
    // Separate occlusion and metallic-roughness images -> packed <stem>_orm.png.
    {
        const std::string mat =
            R"({"pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":3}},"occlusionTexture":{"index":4}})";
        MeshData m;
        std::vector<ac::TextureRequest> tex;
        ac::MeshImportOptions opts;
        opts.generatedDir = "props";
        REQUIRE(ac::importMesh(writeQuad(dir.path(), "packed", UvMapping::Normal, false, mat, images), opts, m, err,
                               nullptr, &tex));
        CHECK(m.submeshes[0].ormTexture == "props/packed_orm.png");
        CHECK(m.submeshes[0].metallicFactor == 1.0f); // glTF defaults
        CHECK(m.submeshes[0].roughnessFactor == 1.0f);
        REQUIRE(tex.size() == 1);
        CHECK(tex[0].writePng);
        CHECK(tex[0].orm == ac::TextureRequest::Orm::Separate);
        std::vector<std::string> written;
        REQUIRE(ac::writeMeshTextures(tex, dir.path() / "out", {}, &written, err));
        CHECK(written == std::vector<std::string>{"props/packed_orm.png", "props/packed_orm.vtex"});
        ac::Image8 png;
        REQUIRE(ac::loadImage8(dir.path() / "out" / "props" / "packed_orm.png", 4, png, err));
        CHECK(png.pixels[0] == 40);  // R occlusion from ao.png
        CHECK(png.pixels[1] == 160); // G roughness from mr.png
        CHECK(png.pixels[2] == 220); // B metallic from mr.png
    }
    // Metallic-roughness without occlusion -> R := 255 (or the image as is with assumeOrmOcclusion).
    {
        const std::string mat = R"({"pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":3}}})";
        MeshData m;
        std::vector<ac::TextureRequest> tex;
        REQUIRE(ac::importMesh(writeQuad(dir.path(), "mronly", UvMapping::Normal, false, mat, images), {}, m, err,
                               nullptr, &tex));
        CHECK(tex[0].orm == ac::TextureRequest::Orm::NoOcclusion);
        REQUIRE(ac::writeMeshTextures(tex, dir.path() / "out", {}, nullptr, err));
        ac::Image8 png;
        REQUIRE(ac::loadImage8(dir.path() / "out" / "mronly_orm.png", 4, png, err));
        CHECK(png.pixels[0] == 255);
        CHECK(png.pixels[1] == 160);
        ac::MeshImportOptions assume;
        assume.assumeOrmOcclusion = true;
        REQUIRE(ac::importMesh(dir.path() / "mronly.gltf", assume, m, err, nullptr, &tex));
        CHECK(m.submeshes[0].ormTexture == "mr.png");
        CHECK(tex[0].orm == ac::TextureRequest::Orm::AsIs);
    }
    // Embedded images (bufferView, as generator .glb files have) are written out next to the mesh.
    {
        std::vector<std::byte> pngBytes = readBytes(dir.path() / "c.png");
        PbrGltfWriter w;
        const int pos = w.floats({0, 0, 0, 1, 0, 0, 1, 1, 0}, "VEC3", true);
        const int tc = w.floats({0, 1, 1, 1, 1, 0}, "VEC2");
        const int idx = w.indices32({0, 1, 2});
        std::vector<std::uint8_t> raw(pngBytes.size());
        std::memcpy(raw.data(), pngBytes.data(), raw.size());
        const int view = w.view(raw);
        const std::string json = std::format(
            R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{{"mesh":0}}],)"
            R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":{},"TEXCOORD_0":{}}},"indices":{},"material":0}}]}}],)"
            R"("materials":[{{"pbrMetallicRoughness":{{"baseColorTexture":{{"index":0}},"metallicFactor":0}}}}],)"
            R"("textures":[{{"source":0}}],"images":[{{"bufferView":{},"mimeType":"image/png"}}],{}}})",
            pos, tc, idx, view, w.buffersJson());
        writeText(dir.path() / "embedded.gltf", json);
        MeshData m;
        std::vector<ac::TextureRequest> tex;
        ac::MeshImportOptions opts;
        opts.generatedDir = "dbg";
        opts.generatedStem = "hero";
        REQUIRE(ac::importMesh(dir.path() / "embedded.gltf", opts, m, err, nullptr, &tex));
        CHECK(m.submeshes[0].texture == "dbg/hero.png");
        CHECK(m.submeshes[0].metallicFactor == 0.0f);
        REQUIRE(tex.size() == 1);
        CHECK(tex[0].writePng);
        REQUIRE(ac::writeMeshTextures(tex, dir.path() / "out", {}, nullptr, err));
        ac::Image8 png;
        REQUIRE(ac::loadImage8(dir.path() / "out" / "dbg" / "hero.png", 4, png, err));
        CHECK(png.pixels[0] == 200);
        CHECK(std::filesystem::exists(dir.path() / "out" / "dbg" / "hero.vtex"));
    }
    // A texture used as two kinds of map is an error; non-zero texCoord sets too.
    {
        const std::string mat =
            R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":1}},"normalTexture":{"index":1}})";
        MeshData m;
        std::vector<ac::TextureRequest> tex;
        CHECK_FALSE(ac::importMesh(writeQuad(dir.path(), "twice", UvMapping::Normal, false, mat, images), {}, m, err,
                                   nullptr, &tex));
        const std::string mat2 = R"({"normalTexture":{"index":1,"texCoord":1}})";
        CHECK_FALSE(ac::importMesh(writeQuad(dir.path(), "tc1", UvMapping::Normal, false, mat2, images), {}, m, err));
    }
}

TEST_CASE("assetc build compiles material textures to .vtex and tracks them as inputs", "[assets][build][pbr]")
{
    TempDir src("pbr_build_src");
    TempDir out("pbr_build_out");
    writeSolidPng(src.path() / "chars" / "hero.png", {200, 150, 100, 255});
    writeSolidPng(src.path() / "chars" / "hero_n.png", {128, 128, 255, 255});
    writeSolidPng(src.path() / "chars" / "hero_orm.png", {255, 128, 0, 255});
    const std::string mat =
        R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":2}},)"
        R"("normalTexture":{"index":1},"occlusionTexture":{"index":2}})";
    const std::string images = R"("textures":[{"source":0},{"source":1},{"source":2}],)"
                               R"("images":[{"uri":"hero.png"},{"uri":"hero_n.png"},{"uri":"hero_orm.png"}],)";
    writeQuad(src.path() / "chars", "hero", UvMapping::Normal, false, mat, images);
    ac::BuildOptions opt;
    ac::BuildStats s1;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, s1));
    CHECK(s1.converted == 1);
    CHECK(s1.copied == 3);
    for (const char* f : {"hero.vmesh", "hero.vtex", "hero_n.vtex", "hero_orm.vtex", "hero.png", "hero_n.png"})
        CHECK(std::filesystem::exists(out.path() / "chars" / f));
    MeshData m;
    std::string err;
    REQUIRE(loadMesh(out.path() / "chars" / "hero.vmesh", m, err));
    CHECK(m.submeshes[0].normalTexture == "chars/hero_n.png");
    CHECK(m.submeshes[0].ormTexture == "chars/hero_orm.png");
    const auto vtexBytes = readBytes(out.path() / "chars" / "hero_n.vtex");

    ac::BuildStats s2;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, s2));
    CHECK(s2.converted == 0);
    // A changed texture source makes the mesh item (which compiled it) stale.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeSolidPng(src.path() / "chars" / "hero_n.png", {160, 128, 240, 255});
    std::filesystem::last_write_time(src.path() / "chars" / "hero_n.png",
                                     std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    ac::BuildStats s3;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, s3));
    CHECK(s3.converted == 1);
    CHECK(readBytes(out.path() / "chars" / "hero_n.vtex") != vtexBytes);
}
