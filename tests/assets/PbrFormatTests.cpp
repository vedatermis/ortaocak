// .vmesh version 2 (snorm16 tangents, PBR material fields), version 1 compatibility, meshes beyond
// 65535 vertices, and the .vtex texture container: round trips and rejection of truncated, corrupt
// and malicious files.

#include "tests/assets/AssetTestUtil.h"

#include "tools/assetc/TextureCompiler.h"

#include "shared/assets/Assets.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

using namespace vo::assets;
using namespace vo::tests::assets;
namespace ac = vo::tools::assetc;

namespace {

// The M5 sample (2 bones, 2 submeshes) moved to the v2 layout: snorm16 tangents, a normal-mapped body
// with an ORM map and factors.
MeshData makeSampleMeshV2()
{
    MeshData f32 = makeSampleMesh();
    MeshData m = f32;
    m.vertexFormat = (f32.vertexFormat & ~static_cast<std::uint32_t>(kVertexTangent)) | kVertexTangentSnorm16;
    const VertexLayout src = f32.layout();
    const VertexLayout dst = m.layout();
    m.vertices.assign(static_cast<std::size_t>(dst.stride) * m.vertexCount, 0);
    for (std::uint32_t v = 0; v < m.vertexCount; ++v)
    {
        const std::uint8_t* s = f32.vertices.data() + static_cast<std::size_t>(v) * src.stride;
        std::uint8_t* d = m.vertices.data() + static_cast<std::size_t>(v) * dst.stride;
        std::memcpy(d + dst.position, s + src.position, 12);
        std::memcpy(d + dst.normal, s + src.normal, 12);
        std::memcpy(d + dst.uv0, s + src.uv0, 8);
        std::memcpy(d + dst.color, s + src.color, 4);
        std::memcpy(d + dst.joints, s + src.joints, 4);
        std::memcpy(d + dst.weights, s + src.weights, 4);
        m.setTangent(v, {0.6f, 0.0f, 0.8f, v % 2 == 0 ? 1.0f : -1.0f});
    }
    m.submeshes[0].texture = "characters/k.png";
    m.submeshes[0].normalTexture = "characters/k_n.png";
    m.submeshes[0].ormTexture = "characters/k_orm.png";
    m.submeshes[0].normalScale = 0.75f;
    m.submeshes[0].roughnessFactor = 0.9f;
    m.submeshes[0].metallicFactor = 1.0f;
    m.submeshes[0].occlusionStrength = 0.5f;
    return m;
}

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
    for (std::size_t i = 0; i < file.size(); ++i)
    {
        std::vector<std::byte> bad = file;
        bad[i] ^= std::byte{0x5A};
        Data d;
        std::string err;
        INFO("flipped byte " << i);
        CHECK_FALSE(parse(bad, d, err));
    }
}

template <class Data, class Parse, class Serialize>
void fuzzFile(const std::vector<std::byte>& file, Parse parse, Serialize serialize, unsigned seed, int iterations)
{
    std::mt19937 rng(seed);
    for (int iter = 0; iter < iterations; ++iter)
    {
        std::vector<std::byte> bad = file;
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int e = 0; e < edits; ++e)
        {
            const std::size_t at = 16 + rng() % (bad.size() - 16);
            if (rng() % 3 == 0 && at + 4 <= bad.size())
                patch<std::uint32_t>(bad, at, rng());
            else
                bad[at] = static_cast<std::byte>(rng() & 0xFF);
        }
        fixCrc(bad);
        Data d;
        std::string err;
        if (parse(bad, d, err))
        {
            std::vector<std::byte> again;
            std::string err2;
            CHECK(serialize(d, again, err2));
        }
    }
}

bool rejectsPatched(const std::vector<std::byte>& good, std::size_t offset, std::uint32_t value)
{
    std::vector<std::byte> bad = good;
    patch(bad, offset, value);
    fixCrc(bad);
    MeshData d;
    std::string e;
    return !parseMesh(bad, d, e);
}

TextureData makeTexture(TextureFormat format, TextureUsage usage, std::uint32_t size)
{
    TextureData t;
    t.format = format;
    t.usage = usage;
    for (std::uint32_t w = size;; w /= 2)
    {
        TextureMip m;
        m.width = m.height = w;
        m.data.resize(static_cast<std::size_t>(textureMipSize(format, w, w)));
        for (std::size_t i = 0; i < m.data.size(); ++i)
            m.data[i] = static_cast<std::uint8_t>((i * 37 + w) & 0xFF);
        t.mips.push_back(std::move(m));
        if (w == 1)
            break;
    }
    return t;
}

} // namespace

TEST_CASE("vmesh v2 round trip keeps snorm16 tangents and the PBR material", "[assets][format][pbr]")
{
    const MeshData m = makeSampleMeshV2();
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeMesh(m, bytes, err));
    CHECK(meshFileVersion(bytes) == 2);
    MeshData back;
    REQUIRE(parseMesh(bytes, back, err));
    CHECK(back.vertexFormat == m.vertexFormat);
    CHECK(back.layout().stride == 52); // skinned 44 + snorm16 tangent 8 (f32 would be 60)
    CHECK(back.layout().tangentSnorm16);
    CHECK(back.vertices == m.vertices);
    const auto t0 = back.tangent(0);
    CHECK(std::abs(t0[0] - 0.6f) < 1e-4f);
    CHECK(std::abs(t0[2] - 0.8f) < 1e-4f);
    CHECK(t0[3] == 1.0f);
    CHECK(back.tangent(1)[3] == -1.0f);
    const Submesh& s = back.submeshes[0];
    CHECK(s.normalTexture == "characters/k_n.png");
    CHECK(s.ormTexture == "characters/k_orm.png");
    CHECK(s.normalScale == 0.75f);
    CHECK(s.roughnessFactor == 0.9f);
    CHECK(s.metallicFactor == 1.0f);
    CHECK(s.occlusionStrength == 0.5f);
    CHECK(back.submeshes[1].normalTexture.empty());
    CHECK(back.submeshes[1].metallicFactor == 0.0f);
    std::vector<std::byte> again;
    REQUIRE(serializeMesh(back, again, err));
    CHECK(again == bytes);
}

TEST_CASE("vmesh version 1 files still load with the default material", "[assets][format][pbr]")
{
    // Written in the M5 layout: 44-byte submeshes, version 1, f32 tangent stream.
    const MeshData m = makeSampleMesh();
    std::vector<std::byte> v1;
    std::string err;
    REQUIRE(serializeMeshAs(m, 1, v1, err));
    CHECK(meshFileVersion(v1) == 1);
    CHECK(peek<std::uint32_t>(v1, 100) - peek<std::uint32_t>(v1, 92) >= 2 * 44); // submesh table + bones
    MeshData back;
    REQUIRE(parseMesh(v1, back, err));
    CHECK(back.vertices == m.vertices);
    CHECK(back.layout().stride == 60);
    CHECK_FALSE(back.layout().tangentSnorm16);
    CHECK(back.submeshes[1].texture == "textures/hair.png");
    CHECK(back.submeshes[1].normalTexture.empty());
    CHECK(back.submeshes[1].roughnessFactor == 1.0f);
    CHECK(back.submeshes[1].metallicFactor == 0.0f);
    CHECK(back.submeshes[1].occlusionStrength == 1.0f);
    // Read back as v1, written again as v2: the same mesh.
    std::vector<std::byte> v2;
    REQUIRE(serializeMesh(back, v2, err));
    MeshData again;
    REQUIRE(parseMesh(v2, again, err));
    CHECK(again.vertices == m.vertices);
    // v1 cannot hold v2 data.
    std::vector<std::byte> out;
    CHECK_FALSE(serializeMeshAs(makeSampleMeshV2(), 1, out, err));
    MeshData factorsOnly = makeSampleMesh();
    factorsOnly.submeshes[0].roughnessFactor = 0.5f;
    CHECK_FALSE(serializeMeshAs(factorsOnly, 1, out, err));
    CHECK_FALSE(serializeMeshAs(m, 3, out, err));
    // The v1 truncation / bit-flip rejection still holds.
    rejectsTruncationsAndFlips<MeshData>(v1, parseMesh);
}

TEST_CASE("vmesh v2 loader rejects truncated, corrupt and fuzzed files", "[assets][format][pbr][fuzz]")
{
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeMesh(makeSampleMeshV2(), bytes, err));
    rejectsTruncationsAndFlips<MeshData>(bytes, parseMesh);
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    MeshData d;
    CHECK_FALSE(parseMesh(longer, d, err));
    fuzzFile<MeshData>(bytes, parseMesh, serializeMesh, 777u, 3000);
}

TEST_CASE("vmesh v2 loader rejects inconsistent material and tangent data with a valid CRC", "[assets][format][pbr]")
{
    std::vector<std::byte> good;
    std::string err;
    REQUIRE(serializeMesh(makeSampleMeshV2(), good, err));
    const auto sub = peek<std::uint32_t>(good, 92);
    const auto vtx = peek<std::uint32_t>(good, 84);
    const VertexLayout l = VertexLayout::fromFormat(peek<std::uint32_t>(good, 32));
    const auto f = [](float x) {
        std::uint32_t u = 0;
        std::memcpy(&u, &x, 4);
        return u;
    };
    CHECK(rejectsPatched(good, 32, peek<std::uint32_t>(good, 32) | kVertexTangent)); // both tangent encodings
    CHECK(rejectsPatched(good, 32, peek<std::uint32_t>(good, 32) | 0x40));           // unknown bit
    CHECK(rejectsPatched(good, sub + 60, f(-1.0f)));     // normal scale < 0
    CHECK(rejectsPatched(good, sub + 60, f(100.0f)));    // normal scale too large
    CHECK(rejectsPatched(good, sub + 64, f(1.5f)));      // roughness > 1
    CHECK(rejectsPatched(good, sub + 68, f(-0.1f)));     // metallic < 0
    CHECK(rejectsPatched(good, sub + 72, f(std::nanf(""))));
    CHECK(rejectsPatched(good, sub + 76, 1));            // reserved
    CHECK(rejectsPatched(good, sub + 48, 0xFFFF));       // normal texture string out of range
    // Tangent w not +-32767, xyz not unit, -32768 component.
    {
        std::vector<std::byte> bad = good;
        patch<std::int16_t>(bad, vtx + l.tangent + 6, 5);
        fixCrc(bad);
        MeshData d;
        CHECK_FALSE(parseMesh(bad, d, err));
    }
    {
        std::vector<std::byte> bad = good;
        patch<std::int16_t>(bad, vtx + l.tangent, 0);
        patch<std::int16_t>(bad, vtx + l.tangent + 4, 0);
        fixCrc(bad);
        MeshData d;
        CHECK_FALSE(parseMesh(bad, d, err));
    }
    {
        std::vector<std::byte> bad = good;
        patch<std::int16_t>(bad, vtx + l.tangent + 2, -32768);
        fixCrc(bad);
        MeshData d;
        CHECK_FALSE(parseMesh(bad, d, err));
    }
    // A normal map on a mesh without tangents.
    MeshData noTangents = makeSampleMesh();
    noTangents.vertexFormat &= ~static_cast<std::uint32_t>(kVertexTangent);
    noTangents.vertices.clear();
    {
        const MeshData withT = makeSampleMesh();
        const VertexLayout a = withT.layout(), b = noTangents.layout();
        noTangents.vertices.assign(static_cast<std::size_t>(b.stride) * 4, 0);
        for (std::uint32_t v = 0; v < 4; ++v)
        {
            const std::uint8_t* s = withT.vertices.data() + static_cast<std::size_t>(v) * a.stride;
            std::uint8_t* d = noTangents.vertices.data() + static_cast<std::size_t>(v) * b.stride;
            std::memcpy(d, s, 24);
            std::memcpy(d + b.uv0, s + a.uv0, 8);
            std::memcpy(d + b.color, s + a.color, 12);
        }
    }
    std::vector<std::byte> out;
    REQUIRE(serializeMesh(noTangents, out, err));
    noTangents.submeshes[0].normalTexture = "a_n.png";
    CHECK_FALSE(serializeMesh(noTangents, out, err));
    // The snorm16 tangent bit is a v2 feature: a version-1 header with it is rejected.
    {
        std::vector<std::byte> bad = good;
        patch<std::uint16_t>(bad, 4, 1);
        fixCrc(bad);
        MeshData d;
        CHECK_FALSE(parseMesh(bad, d, err));
    }
    // Invalid paths in the new fields are refused by the writer.
    for (const char* path : {"../x_n.png", "/abs.png", "a\\b.png"})
    {
        MeshData m = makeSampleMeshV2();
        m.submeshes[0].ormTexture = path;
        INFO(path);
        CHECK_FALSE(serializeMesh(m, out, err));
    }
}

TEST_CASE("vmesh with more than 65535 vertices uses 32-bit indices end to end", "[assets][format][pbr]")
{
    MeshData m;
    m.vertexFormat = kVertexNormal | kVertexTangentSnorm16;
    m.vertexCount = 70'002;
    const VertexLayout l = m.layout();
    m.vertices.assign(static_cast<std::size_t>(l.stride) * m.vertexCount, 0);
    for (std::uint32_t v = 0; v < m.vertexCount; ++v)
    {
        const Vec3 p{static_cast<float>(v % 300), static_cast<float>(v / 300), 0.0f};
        const Vec3 n{0, 0, 1};
        std::uint8_t* d = m.vertices.data() + static_cast<std::size_t>(v) * l.stride;
        std::memcpy(d + l.position, &p, 12);
        std::memcpy(d + l.normal, &n, 12);
        m.setTangent(v, {1, 0, 0, 1});
    }
    std::vector<std::uint32_t> idx;
    for (std::uint32_t v = 0; v + 2 < m.vertexCount; v += 3)
        idx.insert(idx.end(), {v, v + 2, v + 1});
    m.indexSize = 4;
    m.indices.resize(idx.size() * 4);
    std::memcpy(m.indices.data(), idx.data(), m.indices.size());
    Submesh s;
    s.indexCount = static_cast<std::uint32_t>(idx.size());
    s.normalTexture = "big_n.png";
    m.submeshes.push_back(s);
    m.computeBounds();
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeMesh(m, bytes, err));
    MeshData back;
    REQUIRE(parseMesh(bytes, back, err));
    CHECK(back.indexSize == 4);
    CHECK(back.vertexCount == 70'002);
    CHECK(back.index(back.indexCount() - 2) == 69'999 + 2);
    CHECK(back.index(back.indexCount() - 1) == 69'999 + 1);
    // 16-bit indices for that many vertices are refused.
    std::vector<std::byte> bad = bytes;
    patch<std::uint32_t>(bad, 40, 2);
    fixCrc(bad);
    CHECK_FALSE(parseMesh(bad, back, err));
}

TEST_CASE("vtex round trip for RGBA8, BC1 and BC5 with full mip chains", "[assets][format][texture]")
{
    for (const auto& [format, usage] : {std::pair{TextureFormat::Rgba8, TextureUsage::Color},
                                        std::pair{TextureFormat::Bc1, TextureUsage::Data},
                                        std::pair{TextureFormat::Bc5, TextureUsage::Normal},
                                        std::pair{TextureFormat::Bc3, TextureUsage::Color},
                                        std::pair{TextureFormat::Bc4, TextureUsage::Data}})
    {
        const TextureData t = makeTexture(format, usage, 16);
        std::vector<std::byte> bytes;
        std::string err;
        REQUIRE(serializeTexture(t, bytes, err));
        CHECK(detectFileKind(bytes) == FileKind::Texture);
        TextureData back;
        REQUIRE(parseTexture(bytes, back, err));
        CHECK(back.format == format);
        CHECK(back.usage == usage);
        REQUIRE(back.mips.size() == 5);
        for (std::size_t i = 0; i < 5; ++i)
            CHECK(back.mips[i].data == t.mips[i].data);
        CHECK(back.width() == 16);
        std::vector<std::byte> again;
        REQUIRE(serializeTexture(back, again, err));
        CHECK(again == bytes);
    }
    CHECK(compiledTexturePath("characters/kargan_0_n.png") == "characters/kargan_0_n.vtex");
    CHECK(compiledTexturePath("a.b/c") == "a.b/c.vtex");
    CHECK(textureMipSize(TextureFormat::Bc1, 2048, 2048) == 2048ull * 2048 / 2);
    CHECK(textureMipSize(TextureFormat::Bc5, 2, 1) == 16);
}

TEST_CASE("vtex loader rejects truncated, corrupt, inconsistent and fuzzed files", "[assets][format][texture]")
{
    const TextureData t = makeTexture(TextureFormat::Bc5, TextureUsage::Normal, 8);
    std::vector<std::byte> good;
    std::string err;
    REQUIRE(serializeTexture(t, good, err));
    rejectsTruncationsAndFlips<TextureData>(good, parseTexture);
    const auto rejects = [&](std::size_t offset, std::uint32_t value) {
        std::vector<std::byte> bad = good;
        patch(bad, offset, value);
        fixCrc(bad);
        TextureData d;
        std::string e;
        INFO("offset " << offset << " value " << value);
        CHECK_FALSE(parseTexture(bad, d, e));
    };
    rejects(16, 0);             // width
    rejects(16, 6);             // BC needs multiples of 4
    rejects(16, 1u << 20);      // too large
    rejects(24, 5);             // more mips than the chain has
    rejects(24, 0);
    rejects(28, 9);             // unknown format
    rejects(28, 2);             // BC1 is no normal-map format
    rejects(32, 7);             // unknown usage
    rejects(36, 8);             // mip table inside the header
    rejects(36, 0xFFFFFFF0u);   // mip table outside the file
    rejects(40, 1);             // reserved
    const auto table = peek<std::uint32_t>(good, 36);
    rejects(table + 4, 17);     // mip size does not match
    rejects(table, 0xFFFFFF00u); // mip offset outside
    rejects(table + 8, peek<std::uint32_t>(good, table)); // mip 1 overlaps mip 0
    // Colour usage with BC5, normal with BC1: refused by the writer as well.
    TextureData wrong = t;
    wrong.usage = TextureUsage::Color;
    std::vector<std::byte> out;
    CHECK_FALSE(serializeTexture(wrong, out, err));
    fuzzFile<TextureData>(good, parseTexture, serializeTexture, 4242u, 2000);
}

TEST_CASE("texture compiler: sRGB-correct colour mips, renormalised normal mips, BC round trip", "[assets][texture]")
{
    // Colour: a black/white checker averages to linear 0.5 = sRGB 188 (not 128).
    ac::Image8 checker{4, 4, 4, {}};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            const std::uint8_t v = ((x + y) % 2) ? 255 : 0;
            checker.pixels.insert(checker.pixels.end(), {v, v, v, 255});
        }
    const ac::Image8 half = ac::downsampleImage(checker, TextureUsage::Color);
    CHECK(half.width == 2);
    CHECK(std::abs(static_cast<int>(half.pixels[0]) - 188) <= 1);
    const ac::Image8 dataHalf = ac::downsampleImage(checker, TextureUsage::Data);
    CHECK(std::abs(static_cast<int>(dataHalf.pixels[0]) - 128) <= 1);

    // Normal: +X and +Y tilted texels average to a unit vector between them (not shortened).
    ac::Image8 normals{2, 2, 4, {}};
    const auto enc = [](float c) { return static_cast<std::uint8_t>(std::lround((c * 0.5f + 0.5f) * 255.0f)); };
    for (int i = 0; i < 4; ++i)
    {
        const bool tiltX = i % 2 == 0;
        normals.pixels.insert(normals.pixels.end(),
                              {enc(tiltX ? 0.8f : 0.0f), enc(tiltX ? 0.0f : 0.8f), enc(0.6f), 255});
    }
    const ac::Image8 n1 = ac::downsampleImage(normals, TextureUsage::Normal);
    const float nx = n1.pixels[0] / 127.5f - 1.0f, ny = n1.pixels[1] / 127.5f - 1.0f, nz = n1.pixels[2] / 127.5f - 1.0f;
    CHECK(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0f) < 0.02f);
    CHECK(std::abs(nx - ny) < 0.02f);

    // A 64x64 diagonal gradient (BC1-friendly: colours on a line): downscaled to 32 by maxSize, full chain, BC1 decodes close to the source.
    ac::Image8 grad{64, 64, 4, {}};
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            grad.pixels.insert(grad.pixels.end(),
                               {static_cast<std::uint8_t>((x + y) * 2), static_cast<std::uint8_t>((x + y) * 2), 128, 255});
    ac::TextureOptions opts;
    opts.maxSize = 32;
    TextureData tex;
    std::string err;
    REQUIRE(ac::buildTexture(grad, TextureUsage::Data, opts, tex, err));
    CHECK(tex.format == TextureFormat::Bc1);
    CHECK(tex.width() == 32);
    CHECK(tex.mips.size() == 6);
    std::vector<std::uint8_t> decoded;
    REQUIRE(decodeTextureMip(tex, 0, decoded, err));
    int worst = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
        {
            const std::uint8_t* p = decoded.data() + (y * 32 + x) * 4;
            worst = std::max(worst, std::abs(static_cast<int>(p[0]) - (4 * (x + y) + 2)));
            worst = std::max(worst, std::abs(static_cast<int>(p[1]) - (4 * (x + y) + 2)));
        }
    CHECK(worst <= 10);
    // Deterministic.
    TextureData again;
    REQUIRE(ac::buildTexture(grad, TextureUsage::Data, opts, again, err));
    std::vector<std::byte> a, b;
    REQUIRE(serializeTexture(tex, a, err));
    REQUIRE(serializeTexture(again, b, err));
    CHECK(a == b);
    // Normal maps compress to BC5 and decode to unit vectors; colours with alpha to BC3.
    REQUIRE(ac::buildTexture(normals, TextureUsage::Normal, {}, tex, err) == false); // 2x2: too small for BC
    ac::TextureOptions raw;
    raw.compress = false;
    REQUIRE(ac::buildTexture(normals, TextureUsage::Normal, raw, tex, err));
    CHECK(tex.format == TextureFormat::Rgba8);
    ac::Image8 alpha = grad;
    alpha.pixels[3] = 10;
    REQUIRE(ac::buildTexture(alpha, TextureUsage::Color, {}, tex, err));
    CHECK(tex.format == TextureFormat::Bc3);
    ac::Image8 npot{48, 48, 4, std::vector<std::uint8_t>(48 * 48 * 4, 255)};
    CHECK_FALSE(ac::buildTexture(npot, TextureUsage::Color, {}, tex, err));
}
