// glTF -> engine conversion with hand-made glTF files and hand-computed answers:
// handedness (X mirror), winding, normals, tangents, skeleton + animation + CPU skinning, static node
// transforms, appearance variant naming, sidecar clip settings and determinism.

#include "tests/assets/AssetTestUtil.h"

#include "tools/assetc/Build.h"
#include "tools/assetc/GltfImport.h"
#include "tools/assetc/Image.h"

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

std::string base64(const std::vector<std::uint8_t>& data)
{
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3)
    {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], k[v & 63]};
    }
    if (i + 1 == data.size())
    {
        const std::uint32_t v = data[i] << 16;
        out += {k[(v >> 18) & 63], k[(v >> 12) & 63], '=', '='};
    }
    else if (i + 2 == data.size())
    {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], '='};
    }
    return out;
}

// Minimal glTF writer: one buffer (data URI), one bufferView per accessor.
class GltfWriter
{
public:
    int floats(const std::vector<float>& v, const char* type, bool minmax = false)
    {
        const std::size_t comps = std::string_view(type) == "SCALAR" ? 1
                                  : std::string_view(type) == "VEC2" ? 2
                                  : std::string_view(type) == "VEC3" ? 3
                                  : std::string_view(type) == "VEC4" ? 4
                                                                     : 16;
        std::string extra;
        if (minmax)
        {
            std::string mn, mx;
            for (std::size_t c = 0; c < comps; ++c)
            {
                float lo = 1e30f, hi = -1e30f;
                for (std::size_t i = c; i < v.size(); i += comps)
                {
                    lo = std::min(lo, v[i]);
                    hi = std::max(hi, v[i]);
                }
                mn += std::format("{}{}", c ? "," : "", lo);
                mx += std::format("{}{}", c ? "," : "", hi);
            }
            extra = std::format(R"(,"min":[{}],"max":[{}])", mn, mx);
        }
        return add(v.data(), v.size() * 4, 5126, v.size() / comps, type, extra);
    }
    int bytes4(const std::vector<std::uint8_t>& v) { return add(v.data(), v.size(), 5121, v.size() / 4, "VEC4", ""); }
    int indices(const std::vector<std::uint16_t>& v)
    {
        return add(v.data(), v.size() * 2, 5123, v.size(), "SCALAR", "", true);
    }

    std::string buffersJson() const
    {
        std::vector<std::uint8_t> copy(m_bin.begin(), m_bin.end());
        return std::format(R"("buffers":[{{"byteLength":{},"uri":"data:application/octet-stream;base64,{}"}}],"bufferViews":[{}],"accessors":[{}])",
                           m_bin.size(), base64(copy), m_views, m_accessors);
    }

private:
    int add(const void* data, std::size_t size, int componentType, std::size_t count, const char* type,
            const std::string& extra, bool isIndex = false)
    {
        while (m_bin.size() % 4)
            m_bin.push_back(0);
        const std::size_t offset = m_bin.size();
        const auto* p = static_cast<const std::uint8_t*>(data);
        m_bin.insert(m_bin.end(), p, p + size);
        const int index = m_count++;
        m_views += std::format(R"({}{{"buffer":0,"byteOffset":{},"byteLength":{}{}}})", index ? "," : "", offset, size,
                               isIndex ? R"(,"target":34963)" : "");
        m_accessors += std::format(R"({}{{"bufferView":{},"componentType":{},"count":{},"type":"{}"{}}})",
                                   index ? "," : "", index, componentType, count, type, extra);
        return index;
    }

    std::vector<std::uint8_t> m_bin;
    std::string m_views, m_accessors;
    int m_count = 0;
};

// One triangle A(0,0,0) B(1,0,0) C(0,1,0), counter-clockwise seen from +Z in glTF, normal +Z,
// tangent +X with w = +1. Node `nodeExtra` adds a transform.
std::filesystem::path writeTriangle(const std::filesystem::path& dir, const std::string& nodeName,
                                    const std::string& nodeExtra = "", const std::string& material = "")
{
    GltfWriter w;
    const int pos = w.floats({0, 0, 0, 1, 0, 0, 0, 1, 0}, "VEC3", true);
    const int nrm = w.floats({0, 0, 1, 0, 0, 1, 0, 0, 1}, "VEC3");
    const int tan = w.floats({1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1}, "VEC4");
    const int idx = w.indices({0, 1, 2});
    const std::string mat =
        material.empty() ? std::string() : std::format(R"(,"materials":[{{"pbrMetallicRoughness":{{"baseColorFactor":{}}}}}])", material);
    const std::string json = std::format(
        R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{{"name":"{}","mesh":0{}}}],)"
        R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":{},"NORMAL":{},"TANGENT":{}}},"indices":{}{}}}]}}],{}{}}})",
        nodeName, nodeExtra, pos, nrm, tan, idx, material.empty() ? "" : R"(,"material":0)", w.buffersJson(), mat);
    const auto path = dir / (nodeName + ".gltf");
    writeText(path, json);
    return path;
}

// Two-bone chain: joint "a" at the origin, joint "b" 1 m above it (glTF +Y). Triangle:
// v0 (0.5, 0, 0) -> a, v1 (0.5, 2, 0) -> b, v2 (0, 2, 0) -> b. Animation "bend": b rotates about
// glTF +Z from 0 to 90 degrees over 1 s (LINEAR).
std::filesystem::path writeChain(const std::filesystem::path& dir)
{
    GltfWriter w;
    const int pos = w.floats({0.5f, 0, 0, 0.5f, 2, 0, 0, 2, 0}, "VEC3", true);
    const int nrm = w.floats({0, 0, 1, 0, 0, 1, 0, 0, 1}, "VEC3");
    const int jnt = w.bytes4({0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});
    const int wgt = w.floats({1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, "VEC4");
    const int idx = w.indices({0, 1, 2});
    const int ibm = w.floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, // a: identity
                              1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1}, // b: translate (0,-1,0)
                             "MAT4");
    const float s = std::sin(0.785398163f);
    const int tin = w.floats({0.0f, 1.0f}, "SCALAR", true);
    const int rot = w.floats({0, 0, 0, 1, 0, 0, s, s}, "VEC4");
    const std::string json = std::format(
        R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0,2]}}],)"
        R"("nodes":[{{"name":"a","children":[1]}},{{"name":"b","translation":[0,1,0]}},{{"name":"body","mesh":0,"skin":0}}],)"
        R"("skins":[{{"joints":[0,1],"inverseBindMatrices":{}}}],)"
        R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":{},"NORMAL":{},"JOINTS_0":{},"WEIGHTS_0":{}}},"indices":{}}}]}}],)"
        R"("animations":[{{"name":"bend","samplers":[{{"input":{},"output":{},"interpolation":"LINEAR"}}],)"
        R"("channels":[{{"sampler":0,"target":{{"node":1,"path":"rotation"}}}}]}}],{}}})",
        ibm, pos, nrm, jnt, wgt, idx, tin, rot, w.buffersJson());
    const auto path = dir / "chain.gltf";
    writeText(path, json);
    return path;
}

void checkVec(Vec3 got, Vec3 want, float eps = 1e-4f)
{
    INFO("got (" << got.x << ", " << got.y << ", " << got.z << ") want (" << want.x << ", " << want.y << ", "
                 << want.z << ")");
    CHECK(std::abs(got.x - want.x) < eps);
    CHECK(std::abs(got.y - want.y) < eps);
    CHECK(std::abs(got.z - want.z) < eps);
}

} // namespace

TEST_CASE("glTF triangle: X mirror, clockwise front face, normal and tangent handedness", "[assets][gltf]")
{
    TempDir dir("gltf_tri");
    MeshData m;
    std::string err;
    REQUIRE(ac::importMesh(writeTriangle(dir.path(), "tri"), {}, m, err));
    REQUIRE(m.vertexCount == 3);
    REQUIRE(m.indexCount() == 3);
    CHECK_FALSE(m.isSkinned());
    REQUIRE(m.hasTangents());
    // glTF +X (the asset's left) becomes engine -X; Y and Z unchanged.
    checkVec(m.position(1), {-1, 0, 0});
    checkVec(m.position(2), {0, 1, 0});
    checkVec(m.normal(0), {0, 0, 1});
    // Index order swapped: (0, 2, 1).
    CHECK(m.index(0) == 0);
    CHECK(m.index(1) == 2);
    CHECK(m.index(2) == 1);
    // Front face: cross(p1 - p0, p2 - p0) points along the outward normal (+Z).
    const Vec3 p0 = m.position(m.index(0)), p1 = m.position(m.index(1)), p2 = m.position(m.index(2));
    const Vec3 geo = normalize(cross(p1 - p0, p2 - p0));
    CHECK(dot(geo, m.normal(0)) > 0.999f);
    // Screen check: a viewer on the normal's side (z = +5) looks towards -Z. In the left-handed engine
    // space that viewer's screen-right is world -X and screen-up is +Y, so screen coordinates are
    // (-x, y). The signed area in screen space must be negative = clockwise = D3D11 front face.
    const auto sx = [](Vec3 p) { return -p.x; };
    const float screenArea2 = (sx(p1) - sx(p0)) * (p2.y - p0.y) - (sx(p2) - sx(p0)) * (p1.y - p0.y);
    CHECK(screenArea2 < 0.0f);
    // Tangent: xyz mirrored, w negated, so the bitangent cross(n, t) * w is the mirrored glTF
    // bitangent (glTF bitangent = cross(+Z, +X) * 1 = +Y; mirrored +Y = +Y).
    const auto t = m.tangent(0);
    checkVec({t[0], t[1], t[2]}, {-1, 0, 0});
    CHECK(t[3] == -1.0f);
    const Vec3 bitangent = cross(m.normal(0), Vec3{t[0], t[1], t[2]}) * t[3];
    checkVec(bitangent, {0, 1, 0});
}

TEST_CASE("glTF static node transforms are baked and mirrored", "[assets][gltf]")
{
    TempDir dir("gltf_node");
    MeshData m;
    std::string err;
    // Node translated by (2, 0, 3) and rotated 90 degrees about glTF +Y (+Z -> +X in glTF).
    const float s = std::sqrt(0.5f);
    const auto path = writeTriangle(dir.path(), "moved",
                                    std::format(R"(,"translation":[2,0,3],"rotation":[0,{},0,{}])", s, s));
    REQUIRE(ac::importMesh(path, {}, m, err));
    // glTF: B (1,0,0) rotated about +Y by 90 deg -> (0,0,-1), + (2,0,3) -> (2,0,2); engine (-2,0,2).
    checkVec(m.position(1), {-2, 0, 2});
    // Normal +Z -> rotated to +X in glTF -> engine -X.
    checkVec(m.normal(0), {-1, 0, 0});
    const Vec3 p0 = m.position(m.index(0)), p1 = m.position(m.index(1)), p2 = m.position(m.index(2));
    CHECK(dot(normalize(cross(p1 - p0, p2 - p0)), m.normal(0)) > 0.999f);
}

TEST_CASE("glTF material colour is baked into sRGB vertex colours and variant names are parsed", "[assets][gltf]")
{
    TempDir dir("gltf_mat");
    MeshData m;
    std::string err;
    // Linear 0.2140 -> sRGB ~0.5 (128); linear 1.0 -> 255.
    REQUIRE(ac::importMesh(writeTriangle(dir.path(), "hair_3", "", "[0.21404,1.0,0.0,1.0]"), {}, m, err));
    REQUIRE(m.hasColors());
    REQUIRE(m.submeshes.size() == 1);
    CHECK(m.submeshes[0].name == "hair_3");
    CHECK(m.submeshes[0].variantGroup == VariantGroup::Hair);
    CHECK(m.submeshes[0].variantIndex == 3);
    CHECK(m.submeshes[0].baseColor[0] == 1.0f);
    const auto c = m.color(0);
    CHECK(std::abs(static_cast<int>(c[0]) - 128) <= 1);
    CHECK(c[1] == 255);
    CHECK(c[2] == 0);
    MeshData f;
    REQUIRE(ac::importMesh(writeTriangle(dir.path(), "face_0"), {}, f, err));
    CHECK(f.submeshes[0].variantGroup == VariantGroup::Face);
    MeshData b;
    REQUIRE(ac::importMesh(writeTriangle(dir.path(), "hair_x"), {}, b, err));
    CHECK(b.submeshes[0].variantGroup == VariantGroup::Always);
}

TEST_CASE("glTF skinned 2-bone chain: skeleton, animation sampling and skinned positions in engine space",
          "[assets][gltf]")
{
    TempDir dir("gltf_chain");
    const auto path = writeChain(dir.path());
    MeshData m;
    std::string err;
    REQUIRE(ac::importMesh(path, {}, m, err));
    REQUIRE(m.isSkinned());
    REQUIRE(m.skeleton.bones.size() == 2);
    CHECK(m.skeleton.bones[0].name == "a");
    CHECK(m.skeleton.bones[1].name == "b");
    CHECK(m.skeleton.bones[1].parent == 0);
    checkVec(m.skeleton.bones[1].bindLocal.translation, {0, 1, 0});
    // Engine positions: X mirrored.
    checkVec(m.position(0), {-0.5f, 0, 0});
    checkVec(m.position(1), {-0.5f, 2, 0});
    for (std::uint32_t v = 0; v < 3; ++v)
    {
        const auto w = m.weights(v);
        CHECK(w[0] + w[1] + w[2] + w[3] == 255);
    }

    std::vector<AnimationClip> clips;
    ac::AnimImportOptions ao;
    REQUIRE(ac::importAnimations(path, ao, clips, err));
    REQUIRE(clips.size() == 1);
    const AnimationClip& clip = clips[0];
    CHECK(clip.name == "bend");
    CHECK(clip.frameCount == 31);
    CHECK(clip.duration == Approx(1.0f));
    CHECK(clip.loop); // default without a sidecar
    REQUIRE(clip.tracks.size() == 2);

    const ClipBinding binding = bindClip(clip, m.skeleton);
    REQUIRE(binding == ClipBinding{0, 1});
    std::vector<Transform> local;
    std::vector<Mat4> model, skin;

    // Bind pose: skinning matrices are the identity.
    bindPose(m.skeleton, local);
    localToModel(m.skeleton, local, model);
    computeSkinningMatrices(m.skeleton, model, skin);
    for (const Mat4& s : skin)
        CHECK(maxAbsDifference(s, Mat4::identity()) < 1e-5f);

    // t = 1 s (clamped, non-looping): b rotated 90 degrees about glTF +Z.
    // glTF: v1 = T(0,1,0) Rz(90) T(0,-1,0) (0.5,2,0) = (-1, 1.5, 0)  -> engine (1, 1.5, 0).
    samplePose(clip, binding, m.skeleton, 1.0f, false, local);
    localToModel(m.skeleton, local, model);
    computeSkinningMatrices(m.skeleton, model, skin);
    checkVec(skinPosition(m, 1, skin), {1.0f, 1.5f, 0});
    checkVec(skinPosition(m, 2, skin), {1.0f, 1.0f, 0}); // glTF (-1, 1, 0)
    checkVec(skinPosition(m, 0, skin), {-0.5f, 0, 0});   // bone a is not animated
    // The skinned normal follows: glTF +Z stays +Z under a rotation about Z.
    checkVec(skinNormal(m, 1, skin), {0, 0, 1});

    // t = 0.5 s: 45 degrees. glTF v1 = (0.5c - s, 1 + 0.5s + c, 0) with c = s = sqrt(0.5).
    const float h = std::sqrt(0.5f);
    samplePose(clip, binding, m.skeleton, 0.5f, false, local);
    localToModel(m.skeleton, local, model);
    computeSkinningMatrices(m.skeleton, model, skin);
    checkVec(skinPosition(m, 1, skin), {-(0.5f * h - h), 1.0f + 0.5f * h + h, 0}, 2e-4f);

    // Engine-space rotation of bone b at t = 1: glTF +Z rotation by +90 degrees becomes a rotation
    // by -90 degrees about engine +Z (mirror), i.e. quaternion (0, 0, -sin45, cos45).
    samplePose(clip, binding, m.skeleton, 1.0f, false, local);
    CHECK(std::abs(std::abs(local[1].rotation.z) - h) < 1e-4f);
    CHECK(local[1].rotation.z * local[1].rotation.w < 0.0f);

    // Looping wraps time: t = 1.25 behaves like t = 0.25.
    std::vector<Transform> a, b;
    samplePose(clip, binding, m.skeleton, 1.25f, true, a);
    samplePose(clip, binding, m.skeleton, 0.25f, true, b);
    CHECK(std::abs(dot(a[1].rotation, b[1].rotation)) > 0.999999f);
}

TEST_CASE("glTF animation sidecar sets loop and move speed; unknown clips are errors", "[assets][gltf]")
{
    TempDir dir("gltf_sidecar");
    const auto path = writeChain(dir.path());
    writeText(dir.path() / "chain.anims.toml", "[[clip]]\nname = \"bend\"\nloop = false\nmove_speed = 2.5\n");
    std::vector<ac::ClipSettings> settings;
    std::string err;
    REQUIRE(ac::readAnimSidecar(ac::animSidecarPath(path), settings, err));
    REQUIRE(settings.size() == 1);
    ac::AnimImportOptions ao;
    ao.clips = settings;
    std::vector<AnimationClip> clips;
    REQUIRE(ac::importAnimations(path, ao, clips, err));
    CHECK_FALSE(clips[0].loop);
    CHECK(clips[0].moveSpeed == 2.5f);

    writeText(dir.path() / "chain.anims.toml", "[[clip]]\nname = \"fly\"\n");
    REQUIRE(ac::readAnimSidecar(ac::animSidecarPath(path), settings, err));
    ao.clips = settings;
    CHECK_FALSE(ac::importAnimations(path, ao, clips, err));
    writeText(dir.path() / "chain.anims.toml", "[[clip]]\nname = \"bend\"\nspeed = 1\n");
    CHECK_FALSE(ac::readAnimSidecar(ac::animSidecarPath(path), settings, err));
}

TEST_CASE("glTF conversion is deterministic", "[assets][gltf]")
{
    TempDir dir("gltf_det");
    const auto path = writeChain(dir.path());
    std::vector<std::byte> first, second;
    for (auto* out : {&first, &second})
    {
        MeshData m;
        std::string err;
        REQUIRE(ac::importMesh(path, {}, m, err));
        REQUIRE(serializeMesh(m, *out, err));
    }
    CHECK(first == second);
    std::vector<std::byte> a1, a2;
    for (auto* out : {&a1, &a2})
    {
        std::vector<AnimationClip> clips;
        std::string err;
        REQUIRE(ac::importAnimations(path, {}, clips, err));
        AnimationFile f;
        f.clips = clips;
        REQUIRE(serializeAnimation(f, *out, err));
    }
    CHECK(a1 == a2);
}

TEST_CASE("glTF import rejects broken input", "[assets][gltf]")
{
    TempDir dir("gltf_bad");
    MeshData m;
    std::string err;
    writeText(dir.path() / "garbage.gltf", "{ not json");
    CHECK_FALSE(ac::importMesh(dir.path() / "garbage.gltf", {}, m, err));
    CHECK_FALSE(ac::importMesh(dir.path() / "missing.glb", {}, m, err));
    writeText(dir.path() / "empty.gltf", R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[]}],"scene":0})");
    CHECK_FALSE(ac::importMesh(dir.path() / "empty.gltf", {}, m, err));
}

TEST_CASE("assetc build converts a source tree, skips up-to-date items and validates references", "[assets][build]")
{
    TempDir src("build_src");
    TempDir out("build_out");
    std::string err;
    writeChain(src.path() / "models");
    ac::Image8 png{4, 4, 4, std::vector<std::uint8_t>(64, 200)};
    REQUIRE(ac::writePng8(src.path() / "textures" / "a.png", png, err));
    REQUIRE(ac::writePng8(src.path() / "previews" / "p.png", png, err));
    ac::Image8 gray{3, 3, 1, std::vector<std::uint8_t>(9, 255)};
    REQUIRE(ac::writePng8(src.path() / "terrain" / "h.png", gray, err));
    REQUIRE(ac::writePng8(src.path() / "terrain" / "m.png", gray, err));
    REQUIRE(ac::writePng8(src.path() / "terrain" / "s.png", png, err));
    std::string desc = "[terrain]\nheightmap = \"h.png\"\nworld_size = 8.0\nheight_min = 0.0\nheight_max = 1.0\n"
                       "splat = \"s.png\"\nwalk_mask = \"m.png\"\n";
    for (int i = 0; i < 4; ++i)
        desc += "[[layer]]\ntexture = \"textures/a.png\"\ntile_size = 2.0\n";
    desc += "[[spawn]]\nname = \"spawn_aydor\"\nx = 1.0\nz = 1.0\n";
    desc += "[[object]]\nmesh = \"models/chain.vmesh\"\nx = 4.0\nz = 4.0\n";
    writeText(src.path() / "terrain" / "t.terrain.toml", desc);

    ac::BuildOptions opt;
    ac::BuildStats s1;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, s1));
    CHECK(s1.converted == 2);
    CHECK(s1.copied == 1); // previews/ and terrain inputs are not copied
    CHECK(s1.failed == 0);
    CHECK(std::filesystem::exists(out.path() / "models" / "chain.vmesh"));
    CHECK(std::filesystem::exists(out.path() / "models" / "chain" / "bend.vanim"));
    CHECK(std::filesystem::exists(out.path() / "textures" / "a.png"));
    CHECK_FALSE(std::filesystem::exists(out.path() / "previews" / "p.png"));
    CHECK_FALSE(std::filesystem::exists(out.path() / "terrain" / "h.png"));
    TerrainData t;
    REQUIRE(loadTerrain(out.path() / "terrain" / "t.vterrain", t, err));
    CHECK(t.objects.size() == 1);
    const auto meshBytes = readBytes(out.path() / "models" / "chain.vmesh");

    ac::BuildStats s2;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, s2));
    CHECK(s2.skipped == 3);
    CHECK(s2.converted == 0);
    CHECK(s2.copied == 0);

    // A deleted output is rebuilt; forced rebuilds produce identical bytes.
    std::filesystem::remove(out.path() / "models" / "chain" / "bend.vanim");
    ac::BuildStats s3;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, s3));
    CHECK(s3.converted == 1);
    CHECK(std::filesystem::exists(out.path() / "models" / "chain" / "bend.vanim"));
    opt.force = true;
    ac::BuildStats s4;
    REQUIRE(ac::buildAssets(src.path(), out.path(), opt, s4));
    CHECK(s4.skipped == 0);
    CHECK(readBytes(out.path() / "models" / "chain.vmesh") == meshBytes);

    // A placement referencing a missing mesh fails the build (non-zero), the rest still converts.
    writeText(src.path() / "terrain" / "t.terrain.toml", desc + "[[object]]\nmesh = \"models/nope.vmesh\"\nx = 2.0\nz = 2.0\n");
    ac::BuildStats s5;
    CHECK_FALSE(ac::buildAssets(src.path(), out.path(), opt, s5));
    CHECK(s5.failed == 1);
    CHECK(s5.converted == 1);
}
