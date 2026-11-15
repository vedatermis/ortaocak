// Content checks on the real assets built from assets_src by `assetc build` (ctest fixture
// `assets.build`). The asset directory comes from the VO_TEST_ASSET_DIR environment variable.

#include "tools/assetc/GltfImport.h"

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

fs::path assetDir()
{
    return envPath("VO_TEST_ASSET_DIR", "assets");
}

fs::path sourceDir()
{
    return envPath("VO_TEST_ASSET_SRC", "assets_src");
}

MeshData requireMesh(const fs::path& p)
{
    MeshData m;
    std::string err;
    INFO(p.string() << ": " << err);
    REQUIRE(loadMesh(p, m, err));
    return m;
}

const char* kRaces[] = {"aydor", "kargan"};

// Three looks per race (tools/blender/gen_character.py LOOKS): characters/<race>_<n>.vmesh, clips in
// characters/<race>_<n>/<clip>.vanim, texture characters/<race>_<n>.png. Target heights per look.
struct Look
{
    const char* name;
    const char* race;
    float height;
};
const Look kLooks[] = {{"aydor_0", "aydor", 1.82f},   {"aydor_1", "aydor", 1.72f},   {"aydor_2", "aydor", 1.80f},
                       {"kargan_0", "kargan", 2.00f}, {"kargan_1", "kargan", 1.90f}, {"kargan_2", "kargan", 1.98f}};

// UAL skeleton without its 12 leaf bones (53) plus the six helper bones shared by every look.
constexpr std::size_t kBones = 59;
const char* const kHelperBones[] = {"cloth_front", "cloth_back", "mantle", "braid_l", "braid_r", "beard"};

// Every clip (tools/blender/vo_ual.py CLIPS): name, loop. sit_ground_* are made procedurally per
// look; the rest come from the Quaternius UAL pack.
const std::pair<const char*, bool> kClips[] = {
    {"idle", true},          {"walk", true},         {"run", true},          {"sword_idle", true},
    {"sword_attack", false}, {"punch_jab", false},   {"punch_cross", false}, {"spell_enter", false},
    {"spell_idle", true},    {"spell_shoot", false}, {"spell_exit", false},  {"hit_chest", false},
    {"hit_head", false},     {"death", false},       {"jump_start", false},  {"jump_loop", true},
    {"jump_land", false},    {"roll", false},        {"sit_enter", false},   {"sit_idle", true},
    {"sit_exit", false},     {"interact", false},    {"pickup", false},      {"idle_talking", true},
    {"crouch_idle", true},   {"crouch_walk", true},  {"sit_ground_enter", false},
    {"sit_ground_idle", true}, {"sit_ground_exit", false}};

// Character textures: <look>.png (base colour), <look>_n.png (normal), <look>_orm.png, all 2048^2.
constexpr std::uint32_t kTextureSize = 2048;
const char* const kTextureSuffixes[] = {".png", "_n.png", "_orm.png"};

// Game ground speeds the locomotion clips are played at (client: rate = speed / clip.moveSpeed).
constexpr float kWalkSpeed = 1.4f;
constexpr float kRunSpeed = 5.5f;

std::vector<std::byte> fileBytes(const fs::path& p)
{
    std::vector<std::byte> bytes;
    std::string err;
    INFO(p.string() << ": " << err);
    REQUIRE(readFileBytes(p, bytes, err));
    return bytes;
}

float bodyTop(const MeshData& m, float& bottom)
{
    float top = -1e9f;
    bottom = 1e9f;
    for (const Submesh& s : m.submeshes)
    {
        if (s.name != "body")
            continue;
        for (std::uint32_t i = s.indexStart; i < s.indexStart + s.indexCount; ++i)
        {
            const Vec3 p = m.position(m.index(i));
            top = std::max(top, p.y);
            bottom = std::min(bottom, p.y);
        }
    }
    return top;
}

} // namespace

TEST_CASE("all six character looks load as skinned meshes with one shared skeleton", "[content]")
{
    const MeshData a = requireMesh(assetDir() / "characters" / "aydor_0.vmesh");
    REQUIRE(a.isSkinned());
    // UAL skeleton without its 12 leaf bones plus six helper bones; the client's skinning palette
    // holds 64 matrices.
    CHECK(a.skeleton.bones.size() == kBones);
    CHECK(a.skeleton.bones.size() <= 64);
    for (const char* name : kHelperBones)
    {
        INFO("helper bone " << name);
        CHECK(a.skeleton.findBone(name) >= 0);
    }
    for (const char* name : {"root", "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "Head", "clavicle_l",
                             "upperarm_l", "lowerarm_l", "hand_l", "thumb_01_l", "index_03_r", "pinky_01_r", "thigh_r",
                             "calf_r", "foot_r", "ball_r"})
    {
        INFO("bone " << name);
        CHECK(a.skeleton.findBone(name) >= 0);
    }
    for (const Bone& b : a.skeleton.bones)
        CHECK(b.name.find("_leaf") == std::string::npos);
    for (const Look& look : kLooks)
    {
        INFO(look.name);
        const MeshData k = requireMesh(assetDir() / "characters" / (std::string(look.name) + ".vmesh"));
        REQUIRE(k.isSkinned());
        // Same bone names in the same order with the same parents (clips and the client's bone
        // lookups are shared by every look).
        REQUIRE(k.skeleton.bones.size() == a.skeleton.bones.size());
        for (std::size_t i = 0; i < a.skeleton.bones.size(); ++i)
        {
            const Bone& b = a.skeleton.bones[i];
            const Bone& kb = k.skeleton.bones[i];
            INFO("bone " << b.name);
            CHECK(kb.name == b.name);
            const std::string pa = b.parent < 0 ? "" : a.skeleton.bones[static_cast<std::size_t>(b.parent)].name;
            const std::string pk = kb.parent < 0 ? "" : k.skeleton.bones[static_cast<std::size_t>(kb.parent)].name;
            CHECK(pa == pk);
        }
    }
}

TEST_CASE("legacy un-suffixed character names are byte copies of look 0", "[content]")
{
    // Kept until every client loads <race>_<n>; see docs/ASSET_PIPELINE.md.
    for (const char* race : kRaces)
    {
        INFO(race);
        const fs::path dir = assetDir() / "characters";
        CHECK(fileBytes(dir / (std::string(race) + ".vmesh")) == fileBytes(dir / (std::string(race) + "_0.vmesh")));
        int clips = 0;
        for (const auto& e : fs::directory_iterator(dir / (std::string(race) + "_0")))
        {
            INFO(e.path().filename().string());
            CHECK(fileBytes(e.path()) == fileBytes(dir / race / e.path().filename()));
            ++clips;
        }
        CHECK(clips == static_cast<int>(std::size(kClips)));
    }
}

TEST_CASE("character looks have the expected size, orientation and a single textured mesh", "[content]")
{
    for (const Look& look : kLooks)
    {
        const std::string name = look.name;
        INFO(name);
        const MeshData m = requireMesh(assetDir() / "characters" / (name + ".vmesh"));
        float bottom = 0.0f;
        const float top = bodyTop(m, bottom);
        CHECK(std::abs(top - look.height) < 0.03f);
        CHECK(std::abs(bottom) < 0.02f); // feet on the ground
        // One mesh per look: every submesh is always visible (variant group 0) and textured with the
        // look's own base-colour texture, which exists in the built asset tree.
        REQUIRE(!m.submeshes.empty());
        for (std::size_t s = 0; s < m.submeshes.size(); ++s)
        {
            const Submesh& sm = m.submeshes[s];
            INFO("submesh " << sm.name);
            CHECK(sm.variantGroup == VariantGroup::Always);
            CHECK(sm.texture == "characters/" + name + ".png");
            CHECK(fs::exists(assetDir() / sm.texture));
            for (std::uint8_t f = 0; f < 4; ++f)
                for (std::uint8_t h = 0; h < 4; ++h)
                    CHECK(m.isSubmeshVisible(s, f, h));
        }
        // Base colour, normal and ORM maps exist in the built tree, 2048^2 each (PNG IHDR width /
        // height, big-endian).
        for (const char* suffix : kTextureSuffixes)
        {
            const fs::path p = assetDir() / "characters" / (name + suffix);
            INFO(p.string());
            REQUIRE(fs::exists(p));
            const std::vector<std::byte> png = fileBytes(p);
            REQUIRE(png.size() > 24);
            const auto be32 = [&](std::size_t o) {
                return (std::to_integer<std::uint32_t>(png[o]) << 24) | (std::to_integer<std::uint32_t>(png[o + 1]) << 16) |
                       (std::to_integer<std::uint32_t>(png[o + 2]) << 8) | std::to_integer<std::uint32_t>(png[o + 3]);
            };
            CHECK(be32(16) == kTextureSize);
            CHECK(be32(20) == kTextureSize);
        }
        // UVs are present and not all zero (textured mesh).
        float uvSpan = 0.0f;
        for (std::uint32_t v = 0; v < m.vertexCount; v += 17)
        {
            const auto uv = m.uv0(v);
            uvSpan = std::max(uvSpan, std::abs(uv[0]) + std::abs(uv[1]));
        }
        CHECK(uvSpan > 0.1f);
        // Facing +Z: the balls of the feet are in front of the ankles; the character's left side
        // (bones *_l) is at -X in engine space.
        std::vector<Transform> local;
        std::vector<Mat4> model;
        bindPose(m.skeleton, local);
        localToModel(m.skeleton, local, model);
        const auto pos = [&](const char* bone, int axis) {
            return model[static_cast<std::size_t>(m.skeleton.findBone(bone))].m[12 + axis];
        };
        CHECK(pos("hand_l", 0) < -0.2f);
        CHECK(pos("hand_r", 0) > 0.2f);
        CHECK(pos("foot_l", 0) < 0.0f);
        CHECK(pos("ball_l", 2) > pos("foot_l", 2) + 0.08f);
        CHECK(pos("ball_r", 2) > pos("foot_r", 2) + 0.08f);
        CHECK(pos("Head", 1) > 1.4f);
        // Inverse bind matrices agree with the bind-pose locals.
        std::vector<Mat4> skin;
        computeSkinningMatrices(m.skeleton, model, skin);
        for (const Mat4& s : skin)
            CHECK(maxAbsDifference(s, Mat4::identity()) < 1e-3f);
        // Triangle budget: ~38k-triangle sources decimated to at most 30k.
        std::uint32_t tris = 0;
        for (const Submesh& s : m.submeshes)
            tris += s.indexCount / 3;
        CHECK(tris >= 20000);
        CHECK(tris <= 30000);
        // Index width follows the vertex count (16-bit while the vertices fit).
        CHECK(m.indexSize == (m.vertexCount <= 65535 ? 2u : 4u));
    }
}

TEST_CASE("ground-sit clips put the pelvis on the ground and the feet in front", "[content]")
{
    for (const Look& look : kLooks)
    {
        const std::string lookName = look.name;
        INFO(lookName);
        const MeshData m = requireMesh(assetDir() / "characters" / (lookName + ".vmesh"));
        AnimationFile f;
        std::string err;
        REQUIRE(loadAnimation(assetDir() / "characters" / lookName / "sit_ground_idle.vanim", f, err));
        REQUIRE(f.clips.size() == 1);
        const AnimationClip& c = f.clips[0];
        const ClipBinding binding = bindClip(c, m.skeleton);
        std::vector<Transform> local;
        std::vector<Mat4> model;
        samplePose(c, binding, m.skeleton, 0.0f, false, local);
        localToModel(m.skeleton, local, model);
        const auto pos = [&](const char* bone, int axis) {
            return model[static_cast<std::size_t>(m.skeleton.findBone(bone))].m[12 + axis];
        };
        const float scale = look.height / 1.83f;
        CHECK(pos("pelvis", 1) < 0.35f * scale);   // seated on the ground, not on a chair (~0.5 m)
        CHECK(pos("pelvis", 1) > 0.05f * scale);
        // Upright torso: the head pivot (skull base) of a seated adult is ~0.38 of the standing
        // height above the floor, ~0.70 m at 1.83 m; lying or slumped would be far lower.
        CHECK(pos("Head", 1) > 0.60f * scale);
        for (const char* b : {"foot_l", "foot_r", "ball_l", "ball_r", "calf_l", "calf_r"})
        {
            INFO(b);
            CHECK(pos(b, 1) > -0.02f);             // nothing below the ground
            CHECK(pos(b, 2) > pos("pelvis", 2));   // legs folded in front
        }
    }
}
TEST_CASE("every mesh has normalised bone weights and no degenerate triangles", "[content]")
{
    int meshes = 0;
    for (const auto& e : fs::recursive_directory_iterator(assetDir()))
    {
        if (e.path().extension() != ".vmesh")
            continue;
        ++meshes;
        INFO(e.path().string());
        const MeshData m = requireMesh(e.path());
        if (m.isSkinned())
            for (std::uint32_t v = 0; v < m.vertexCount; ++v)
            {
                const auto w = m.weights(v);
                CHECK(w[0] + w[1] + w[2] + w[3] == 255);
            }
        int degenerate = 0;
        for (std::uint32_t t = 0; t + 2 < m.indexCount(); t += 3)
        {
            const Vec3 a = m.position(m.index(t)), b = m.position(m.index(t + 1)), c = m.position(m.index(t + 2));
            if (length(cross(b - a, c - a)) * 0.5f < 1e-7f)
                ++degenerate;
        }
        CHECK(degenerate == 0);
        for (std::uint32_t v = 0; v < m.vertexCount; ++v)
        {
            const float n = length(m.normal(v));
            if (std::abs(n - 1.0f) > 1e-3f)
            {
                FAIL_CHECK("non-unit normal at vertex " << v);
                break;
            }
        }
    }
    CHECK(meshes >= 15);
}

TEST_CASE("character clips exist, loop seamlessly, are continuous and keep planted feet still", "[content]")
{
    for (const Look& look : kLooks)
    {
        const std::string lookName = look.name;
        const MeshData m = requireMesh(assetDir() / "characters" / (lookName + ".vmesh"));
        for (const auto& [name, loops] : kClips)
        {
            INFO(lookName << "/" << name);
            AnimationFile f;
            std::string err;
            REQUIRE(loadAnimation(assetDir() / "characters" / lookName / (std::string(name) + ".vanim"), f, err));
            REQUIRE(f.clips.size() == 1);
            const AnimationClip& c = f.clips[0];
            CHECK(c.name == name);
            CHECK(c.loop == loops);
            CHECK(c.frameCount >= 10);
            const ClipBinding binding = bindClip(c, m.skeleton);
            CHECK(binding.size() == m.skeleton.bones.size());
            for (int b : binding)
                CHECK(b >= 0); // every track drives a bone of the mesh skeleton
            // moveSpeed: the clip's own planted-foot speed (the client scales playback to the real
            // ground speed, 1.4 m/s walking and 5.5 m/s running). The walk is re-timed so it plays
            // close to its authored cadence (rate within 0.85-1.15).
            const std::string n = name;
            float speed = 0.0f;
            if (n == "walk")
            {
                CHECK(kWalkSpeed / c.moveSpeed > 0.85f);
                CHECK(kWalkSpeed / c.moveSpeed < 1.15f);
                speed = c.moveSpeed;
            }
            else if (n == "run")
            {
                CHECK(kRunSpeed / c.moveSpeed > 0.8f);
                CHECK(kRunSpeed / c.moveSpeed < 1.3f);
                speed = c.moveSpeed;
            }
            else if (n == "crouch_walk")
                CHECK(c.moveSpeed > 0.3f); // crouched feet roll on the toes: no flat-foot check
            else
                CHECK(c.moveSpeed == 0.0f);
            if (!loops)
                continue; // one-shot clips: seam, continuity and foot checks below are for loops

            std::vector<std::vector<Mat4>> poses(c.frameCount);
            std::vector<Transform> local;
            for (std::uint32_t k = 0; k < c.frameCount; ++k)
            {
                samplePose(c, binding, m.skeleton, static_cast<float>(k) / c.sampleRate, false, local);
                localToModel(m.skeleton, local, poses[k]);
            }
            // Seamless: first and last pose are identical (model-space bone positions and axes).
            float seam = 0.0f;
            for (std::size_t b = 0; b < m.skeleton.bones.size(); ++b)
                seam = std::max(seam, maxAbsDifference(poses.front()[b], poses.back()[b]));
            CHECK(seam < 1e-4f);
            // Continuous: no bone jumps more than 0.6 m between frames (fast swing feet at 5.5 m/s).
            float jump = 0.0f;
            for (std::uint32_t k = 1; k < c.frameCount; ++k)
                for (std::size_t b = 0; b < m.skeleton.bones.size(); ++b)
                {
                    const Vec3 d{poses[k][b].m[12] - poses[k - 1][b].m[12], poses[k][b].m[13] - poses[k - 1][b].m[13],
                                 poses[k][b].m[14] - poses[k - 1][b].m[14]};
                    jump = std::max(jump, length(d));
                }
            CHECK(jump < 0.6f);
            // No exploding vertices: every skinned vertex stays near the character.
            std::vector<Mat4> skin;
            float far = 0.0f;
            for (std::uint32_t k = 0; k < c.frameCount; k += 3)
            {
                computeSkinningMatrices(m.skeleton, poses[k], skin);
                for (std::uint32_t v = 0; v < m.vertexCount; v += 7)
                    far = std::max(far, length(skinPosition(m, v, skin) - m.sphereCenter));
            }
            CHECK(far < m.sphereRadius * 1.3f);
            // Foot planting: while the ball of a foot (toe bone head) is at its bind-pose height in
            // two consecutive frames (flat foot on the ground) it must move backwards (-Z) at exactly
            // the clip speed.
            if (speed > 0.0f)
            {
                std::vector<Transform> bindLocal;
                std::vector<Mat4> bindModel;
                bindPose(m.skeleton, bindLocal);
                localToModel(m.skeleton, bindLocal, bindModel);
                int planted = 0;
                float slide = 0.0f;
                for (const char* toe : {"ball_l", "ball_r"})
                {
                    const auto b = static_cast<std::size_t>(m.skeleton.findBone(toe));
                    const float ground = bindModel[b].m[13];
                    for (std::uint32_t k = 1; k < c.frameCount; ++k)
                    {
                        const Mat4& p0 = poses[k - 1][b];
                        const Mat4& p1 = poses[k][b];
                        if (std::abs(p0.m[13] - ground) > 0.002f || std::abs(p1.m[13] - ground) > 0.002f)
                            continue;
                        ++planted;
                        const float dz = p1.m[14] - p0.m[14];
                        const float dx = p1.m[12] - p0.m[12];
                        slide = std::max(slide, std::abs(dz + speed / c.sampleRate) + std::abs(dx));
                    }
                }
                CHECK(planted >= 4);
                CHECK(slide < 0.004f); // < 4 mm per frame
            }
        }
    }
}

TEST_CASE("converting a committed source twice gives identical bytes equal to the build output", "[content]")
{
    const fs::path src = sourceDir() / "characters" / "aydor_0.glb";
    std::vector<std::byte> first, second;
    for (auto* out : {&first, &second})
    {
        MeshData m;
        std::string err;
        INFO(err);
        ac::MeshImportOptions opts;
        opts.assetRoot = sourceDir(); // as `assetc build` does (texture paths relative to the root)
        REQUIRE(ac::importMesh(src, opts, m, err));
        REQUIRE(serializeMesh(m, *out, err));
    }
    CHECK(first == second);
    std::vector<std::byte> built;
    std::string err;
    REQUIRE(readFileBytes(assetDir() / "characters" / "aydor_0.vmesh", built, err));
    CHECK(built == first);
}
