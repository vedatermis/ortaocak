// .vmesh / .vanim / .vterrain: round trips and rejection of truncated, corrupt and malicious files.

#include "tests/assets/AssetTestUtil.h"

#include "shared/assets/Assets.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace vo::assets;
using namespace vo::tests::assets;

namespace {

AnimationFile makeSampleAnim(bool quantized)
{
    AnimationFile f;
    AnimationClip c;
    c.name = "walk";
    c.sampleRate = 30.0f;
    c.frameCount = 4;
    c.duration = 3.0f / 30.0f;
    c.loop = true;
    c.moveSpeed = 2.0f;
    c.quantizedRotation = quantized;
    for (const char* bone : {"a", "b"})
    {
        AnimationTrack t;
        t.bone = bone;
        for (std::uint32_t k = 0; k < c.frameCount; ++k)
        {
            Transform x;
            x.translation = {0.1f * static_cast<float>(k), 1.0f, 0.0f};
            x.rotation = quatFromAxisAngle({0, 0, 1}, 0.2f * static_cast<float>(k));
            t.keys.push_back(x);
        }
        c.tracks.push_back(t);
    }
    f.clips.push_back(c);
    return f;
}

TerrainData makeSampleTerrain()
{
    TerrainData t;
    t.resolution = 3;
    t.worldSize = 2.0f;
    t.minHeight = -1.0f;
    t.maxHeight = 3.0f;
    t.heights = {0, 16384, 32768, 16384, 32768, 49152, 32768, 49152, 65535};
    t.splatResolution = 2;
    t.splat.assign(16, 64);
    t.maskResolution = 2;
    t.walkMask = {1, 0, 1, 1};
    t.layers = {{"textures/terrain/grass.png", 6.0f}, {"textures/terrain/dirt.png", 4.0f},
                {"textures/terrain/rock.png", 8.0f}, {"textures/terrain/drygrass.png", 6.0f}};
    t.spawns = {{"spawn_aydor", {0.5f, 0.0f, 0.5f}, 1.0f}};
    t.objects = {{"props/house.vmesh", {1.0f, 0.5f, 1.0f}, 0.5f, 1.2f, kObjectBlocks}};
    return t;
}

template <class Data, class Parse>
void expectRejectsAllTruncations(const std::vector<std::byte>& file, Parse parse)
{
    for (std::size_t n = 0; n < file.size(); ++n)
    {
        Data d;
        std::string err;
        INFO("prefix length " << n);
        CHECK_FALSE(parse(std::span<const std::byte>(file.data(), n), d, err));
        CHECK_FALSE(err.empty());
    }
}

template <class Data, class Parse>
void expectRejectsAllByteFlips(const std::vector<std::byte>& file, Parse parse)
{
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

// Random mutations with a recomputed CRC: parsing must never crash or accept inconsistent data;
// whatever it accepts must serialise again.
template <class Data, class Parse, class Serialize>
void fuzz(const std::vector<std::byte>& file, Parse parse, Serialize serialize, unsigned seed)
{
    std::mt19937 rng(seed);
    for (int iter = 0; iter < 3000; ++iter)
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

} // namespace

TEST_CASE("vmesh round trip preserves everything", "[assets][format]")
{
    const MeshData m = makeSampleMesh();
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeMesh(m, bytes, err));
    REQUIRE(detectFileKind(bytes) == FileKind::Mesh);
    MeshData back;
    REQUIRE(parseMesh(bytes, back, err));
    CHECK(back.vertexFormat == m.vertexFormat);
    CHECK(back.vertexCount == 4);
    CHECK(back.vertices == m.vertices);
    CHECK(back.indices == m.indices);
    CHECK(back.indexCount() == 6);
    REQUIRE(back.submeshes.size() == 2);
    CHECK(back.submeshes[1].name == "hair_1");
    CHECK(back.submeshes[1].texture == "textures/hair.png");
    CHECK(back.submeshes[1].variantGroup == VariantGroup::Hair);
    CHECK(back.submeshes[1].variantIndex == 1);
    CHECK(back.submeshes[1].baseColor[1] == 0.25f);
    REQUIRE(back.skeleton.bones.size() == 2);
    CHECK(back.skeleton.bones[1].parent == 0);
    CHECK(back.skeleton.findBone("b") == 1);
    CHECK(back.skeleton.bones[1].inverseBind.m[13] == -1.0f);
    CHECK(back.boundsMax.x == 1.0f);
    CHECK(back.layout().stride == 60);
    // Serialising again gives identical bytes (determinism).
    std::vector<std::byte> again;
    REQUIRE(serializeMesh(back, again, err));
    CHECK(again == bytes);
}

TEST_CASE("vmesh appearance variant selection", "[assets][format]")
{
    const MeshData m = makeSampleMesh();
    CHECK(m.isSubmeshVisible(0, 0, 0));
    CHECK(m.isSubmeshVisible(0, 3, 2));
    CHECK_FALSE(m.isSubmeshVisible(1, 0, 0));
    CHECK(m.isSubmeshVisible(1, 0, 1));
    CHECK_FALSE(m.isSubmeshVisible(1, 1, 0));
    CHECK_FALSE(m.isSubmeshVisible(5, 0, 0));
}

TEST_CASE("vmesh loader rejects truncated and corrupt files", "[assets][format]")
{
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeMesh(makeSampleMesh(), bytes, err));
    expectRejectsAllTruncations<MeshData>(bytes, parseMesh);
    expectRejectsAllByteFlips<MeshData>(bytes, parseMesh);
    // Appended garbage: size mismatch.
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    MeshData d;
    CHECK_FALSE(parseMesh(longer, d, err));
}

TEST_CASE("vmesh loader rejects oversized counts and bad offsets with a valid CRC", "[assets][format]")
{
    std::vector<std::byte> good;
    std::string err;
    REQUIRE(serializeMesh(makeSampleMesh(), good, err));
    const auto rejects = [&](std::size_t offset, std::uint32_t value) {
        std::vector<std::byte> bad = good;
        patch(bad, offset, value);
        fixCrc(bad);
        MeshData d;
        std::string e;
        const bool ok = parseMesh(bad, d, e);
        INFO("offset " << offset << " value " << value << " -> " << e);
        CHECK_FALSE(ok);
    };
    rejects(16, 0xFFFFFFFFu);     // vertexCount
    rejects(16, 5);               // vertexCount larger than the data
    rejects(20, 0x40000000u);     // indexCount
    rejects(20, 7);               // not a multiple of 3
    rejects(24, 0x7FFFFFFFu);     // submeshCount
    rejects(24, 0);
    rejects(28, 300);             // boneCount > 255
    rejects(28, 0);               // SKIN flag without bones
    rejects(32, 0x10 | 0x1);      // unknown vertex format bit
    rejects(36, 36);              // stride does not match the format
    rejects(40, 3);               // index size
    rejects(84, 0xFFFFFF00u);     // vertexOffset outside the file
    rejects(88, static_cast<std::uint32_t>(good.size() - 4)); // indices run off the end
    rejects(100, static_cast<std::uint32_t>(good.size()));    // string table offset at EOF + size > 0
    rejects(104, 0x7FFFFFFFu);    // string table size
    // Index value out of range.
    {
        std::vector<std::byte> bad = good;
        const auto indexOffset = peek<std::uint32_t>(bad, 88);
        patch<std::uint16_t>(bad, indexOffset, 4);
        fixCrc(bad);
        MeshData d;
        CHECK_FALSE(parseMesh(bad, d, err));
    }
    // Weights that do not sum to 255.
    {
        std::vector<std::byte> bad = good;
        const auto vo = peek<std::uint32_t>(bad, 84);
        const auto l = VertexLayout::fromFormat(peek<std::uint32_t>(bad, 32));
        patch<std::uint8_t>(bad, vo + l.weights, 200);
        fixCrc(bad);
        MeshData d;
        CHECK_FALSE(parseMesh(bad, d, err));
    }
}

TEST_CASE("vmesh writer refuses invalid data (bad texture path, bad bone parent)", "[assets][format]")
{
    std::string err;
    std::vector<std::byte> bytes;
    for (const char* path : {"../secret.png", "/abs.png", "C:/x.png", "a\\b.png", "a//b.png"})
    {
        MeshData m = makeSampleMesh();
        m.submeshes[1].texture = path;
        INFO(path);
        CHECK_FALSE(serializeMesh(m, bytes, err));
    }
    MeshData m = makeSampleMesh();
    m.skeleton.bones[0].parent = 1; // parent after child
    CHECK_FALSE(serializeMesh(m, bytes, err));
}

TEST_CASE("vmesh fuzzed files never crash the loader", "[assets][format][fuzz]")
{
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeMesh(makeSampleMesh(), bytes, err));
    fuzz<MeshData>(bytes, parseMesh, serializeMesh, 1234u);
}

TEST_CASE("vanim round trip, float and quantised rotations", "[assets][format]")
{
    for (bool quantized : {false, true})
    {
        const AnimationFile f = makeSampleAnim(quantized);
        std::vector<std::byte> bytes;
        std::string err;
        REQUIRE(serializeAnimation(f, bytes, err));
        AnimationFile back;
        REQUIRE(parseAnimation(bytes, back, err));
        REQUIRE(back.clips.size() == 1);
        const AnimationClip& c = back.clips[0];
        CHECK(c.name == "walk");
        CHECK(c.frameCount == 4);
        CHECK(c.loop);
        CHECK(c.moveSpeed == 2.0f);
        CHECK(c.quantizedRotation == quantized);
        REQUIRE(c.findTrack("b") != nullptr);
        for (std::uint32_t k = 0; k < 4; ++k)
        {
            const Quat a = f.clips[0].tracks[1].keys[k].rotation;
            const Quat b = c.tracks[1].keys[k].rotation;
            CHECK(std::abs(dot(a, b)) > (quantized ? 0.99999f : 0.9999999f));
            CHECK(c.tracks[1].keys[k].translation.x == f.clips[0].tracks[1].keys[k].translation.x);
        }
        if (quantized)
            CHECK(bytes.size() < 400);
    }
}

TEST_CASE("vanim loader rejects truncated, corrupt and inconsistent files", "[assets][format]")
{
    std::vector<std::byte> good;
    std::string err;
    REQUIRE(serializeAnimation(makeSampleAnim(false), good, err));
    expectRejectsAllTruncations<AnimationFile>(good, parseAnimation);
    expectRejectsAllByteFlips<AnimationFile>(good, parseAnimation);
    const auto clip = peek<std::uint32_t>(good, 20);
    const auto rejects = [&](std::size_t offset, std::uint32_t value) {
        std::vector<std::byte> bad = good;
        patch(bad, offset, value);
        fixCrc(bad);
        AnimationFile d;
        std::string e;
        INFO("offset " << offset << " -> " << e);
        CHECK_FALSE(parseAnimation(bad, d, e));
    };
    rejects(16, 100000);          // clipCount
    rejects(16, 0);
    rejects(clip + 16, 70000);    // frameCount
    rejects(clip + 16, 5);        // frameCount inconsistent with duration
    rejects(clip + 28, 1000);     // trackCount
    rejects(clip + 32, 0xFFFFFFF0u); // trackOffset
    rejects(clip + 20, 0x4);      // unknown flag
    {
        std::vector<std::byte> bad = good;
        float rate = 0.0f;
        patch(bad, clip + 12, rate);
        fixCrc(bad);
        AnimationFile d;
        CHECK_FALSE(parseAnimation(bad, d, err));
    }
}

TEST_CASE("vanim fuzzed files never crash the loader", "[assets][format][fuzz]")
{
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeAnimation(makeSampleAnim(true), bytes, err));
    fuzz<AnimationFile>(bytes, parseAnimation, serializeAnimation, 99u);
}

TEST_CASE("vterrain round trip", "[assets][format]")
{
    const TerrainData t = makeSampleTerrain();
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeTerrain(t, bytes, err));
    TerrainData back;
    REQUIRE(parseTerrain(bytes, back, err));
    CHECK(back.heights == t.heights);
    CHECK(back.splat == t.splat);
    CHECK(back.walkMask == t.walkMask);
    CHECK(back.layers[2].texture == "textures/terrain/rock.png");
    CHECK(back.layers[1].tileSize == 4.0f);
    REQUIRE(back.findSpawn("spawn_aydor") != nullptr);
    CHECK(back.findSpawn("spawn_aydor")->yaw == 1.0f);
    CHECK(back.findSpawn("nope") == nullptr);
    REQUIRE(back.objects.size() == 1);
    CHECK(back.objects[0].mesh == "props/house.vmesh");
    CHECK(back.objects[0].scale == 1.2f);
    CHECK(back.objects[0].blocks());
}

TEST_CASE("vterrain loader rejects truncated, corrupt and malicious files", "[assets][format]")
{
    std::vector<std::byte> good;
    std::string err;
    REQUIRE(serializeTerrain(makeSampleTerrain(), good, err));
    expectRejectsAllTruncations<TerrainData>(good, parseTerrain);
    expectRejectsAllByteFlips<TerrainData>(good, parseTerrain);
    const auto rejects = [&](std::size_t offset, std::uint32_t value) {
        std::vector<std::byte> bad = good;
        patch(bad, offset, value);
        fixCrc(bad);
        TerrainData d;
        std::string e;
        INFO("offset " << offset << " value " << value);
        CHECK_FALSE(parseTerrain(bad, d, e));
    };
    rejects(16, 4);           // resolution not 2^k+1
    rejects(16, 65537);       // resolution too large
    rejects(32, 5000);        // splat resolution
    rejects(36, 0);           // mask resolution
    rejects(40, 3);           // layer count
    rejects(44, 2000);        // spawn count
    rejects(48, 0x7FFFFFFFu); // object count
    rejects(52, 0xFFFFFFF0u); // height offset
    // Mask value other than 0/1.
    {
        std::vector<std::byte> bad = good;
        patch<std::uint8_t>(bad, peek<std::uint32_t>(bad, 60), 7);
        fixCrc(bad);
        TerrainData d;
        CHECK_FALSE(parseTerrain(bad, d, err));
    }
    // Object mesh path escaping the asset root.
    {
        TerrainData t = makeSampleTerrain();
        t.objects[0].mesh = "../../etc/passwd";
        std::vector<std::byte> bytes;
        CHECK_FALSE(serializeTerrain(t, bytes, err));
    }
}

TEST_CASE("vterrain fuzzed files never crash the loader", "[assets][format][fuzz]")
{
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeTerrain(makeSampleTerrain(), bytes, err));
    fuzz<TerrainData>(bytes, parseTerrain, serializeTerrain, 4321u);
}

TEST_CASE("asset files load from disk and missing files report an error", "[assets][format]")
{
    TempDir dir("formats_io");
    std::vector<std::byte> bytes;
    std::string err;
    REQUIRE(serializeMesh(makeSampleMesh(), bytes, err));
    REQUIRE(writeFileBytes(dir.path() / "m.vmesh", bytes, err));
    MeshData m;
    REQUIRE(loadMesh(dir.path() / "m.vmesh", m, err));
    CHECK(m.vertexCount == 4);
    CHECK_FALSE(loadMesh(dir.path() / "missing.vmesh", m, err));
    CHECK_FALSE(err.empty());
    AnimationFile a;
    CHECK_FALSE(loadAnimation(dir.path() / "m.vmesh", a, err)); // wrong magic
    CHECK(err.find("magic") != std::string::npos);
}

TEST_CASE("CRC-32 matches the zlib reference value", "[assets][format]")
{
    const std::string_view s = "123456789";
    CHECK(crc32(std::as_bytes(std::span(s.data(), s.size()))) == 0xCBF43926u);
}
