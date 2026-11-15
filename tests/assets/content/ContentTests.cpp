// Content checks on the real assets built from assets_src by `assetc build` (ctest fixture
// `assets.build`). The asset directory comes from the VO_TEST_ASSET_DIR environment variable.

#include "tools/assetc/GltfImport.h"

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("both characters load as skinned meshes with matching skeletons", "[content]")
{
    const MeshData a = requireMesh(assetDir() / "characters" / "aydor.vmesh");
    const MeshData k = requireMesh(assetDir() / "characters" / "kargan.vmesh");
    REQUIRE(a.isSkinned());
    REQUIRE(k.isSkinned());
    // UAL skeleton without its 12 leaf bones; the client's skinning palette holds 64 matrices.
    CHECK(a.skeleton.bones.size() == 53);
    CHECK(a.skeleton.bones.size() <= 64);
    REQUIRE(a.skeleton.bones.size() == k.skeleton.bones.size());
    for (const char* name : {"root", "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "Head", "clavicle_l",
                             "upperarm_l", "lowerarm_l", "hand_l", "thumb_01_l", "index_03_r", "pinky_01_r", "thigh_r",
                             "calf_r", "foot_r", "ball_r"})
    {
        INFO("bone " << name);
        CHECK(a.skeleton.findBone(name) >= 0);
    }
    for (const Bone& b : a.skeleton.bones)
        CHECK(b.name.find("_leaf") == std::string::npos);
    for (const Bone& b : a.skeleton.bones)
    {
        const int i = k.skeleton.findBone(b.name);
        INFO("bone " << b.name);
        REQUIRE(i >= 0);
        const Bone& kb = k.skeleton.bones[static_cast<std::size_t>(i)];
        const std::string pa = b.parent < 0 ? "" : a.skeleton.bones[static_cast<std::size_t>(b.parent)].name;
        const std::string pk = kb.parent < 0 ? "" : k.skeleton.bones[static_cast<std::size_t>(kb.parent)].name;
        CHECK(pa == pk);
    }
}

TEST_CASE("characters have the expected size, orientation and a single textured look", "[content]")
{
    const std::map<std::string, float> heights{{"aydor", 1.77f}, {"kargan", 1.93f}};
    for (const char* race : kRaces)
    {
        INFO(race);
        const MeshData m = requireMesh(assetDir() / "characters" / (std::string(race) + ".vmesh"));
        float bottom = 0.0f;
        const float top = bodyTop(m, bottom);
        CHECK(std::abs(top - heights.at(race)) < 0.03f);
        CHECK(std::abs(bottom) < 0.02f); // feet on the ground
        // One look per race: every submesh is always visible (variant group 0) and textured with a
        // base-colour texture that exists in the built asset tree.
        REQUIRE(!m.submeshes.empty());
        for (std::size_t s = 0; s < m.submeshes.size(); ++s)
        {
            const Submesh& sm = m.submeshes[s];
            INFO("submesh " << sm.name);
            CHECK(sm.variantGroup == VariantGroup::Always);
            CHECK(sm.texture == "characters/" + std::string(race) + ".png");
            CHECK(fs::exists(assetDir() / sm.texture));
            for (std::uint8_t f = 0; f < 4; ++f)
                for (std::uint8_t h = 0; h < 4; ++h)
                    CHECK(m.isSubmeshVisible(s, f, h));
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
        // Triangle budget (generated source meshes, ~12k each).
        std::uint32_t tris = 0;
        for (const Submesh& s : m.submeshes)
            tris += s.indexCount / 3;
        CHECK(tris >= 3000);
        CHECK(tris <= 13000);
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
    // Clips from the Quaternius UAL pack (tools/blender/vo_ual.py): name, loop. idle/walk/run are
    // the ones the client plays; the others are exported for upcoming features.
    const std::pair<const char*, bool> clips[] = {
        {"idle", true},          {"walk", true},         {"run", true},          {"sword_idle", true},
        {"sword_attack", false}, {"punch_jab", false},   {"punch_cross", false}, {"spell_enter", false},
        {"spell_idle", true},    {"spell_shoot", false}, {"spell_exit", false},  {"hit_chest", false},
        {"hit_head", false},     {"death", false},       {"jump_start", false},  {"jump_loop", true},
        {"jump_land", false},    {"roll", false},        {"sit_enter", false},   {"sit_idle", true},
        {"sit_exit", false},     {"interact", false},    {"pickup", false},      {"idle_talking", true},
        {"crouch_idle", true},   {"crouch_walk", true}};
    for (const char* race : kRaces)
    {
        const MeshData m = requireMesh(assetDir() / "characters" / (std::string(race) + ".vmesh"));
        for (const auto& [name, loops] : clips)
        {
            INFO(race << "/" << name);
            AnimationFile f;
            std::string err;
            REQUIRE(loadAnimation(assetDir() / "characters" / race / (std::string(name) + ".vanim"), f, err));
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
            // ground speed, 2.0 m/s walking and 5.5 m/s running, within its 0.3-3x rate clamp).
            const std::string n = name;
            float speed = 0.0f;
            if (n == "walk")
            {
                CHECK(c.moveSpeed > 2.0f / 3.0f);
                CHECK(c.moveSpeed < 2.0f / 1.5f);
                speed = c.moveSpeed;
            }
            else if (n == "run")
            {
                CHECK(c.moveSpeed > 5.5f / 1.3f);
                CHECK(c.moveSpeed < 5.5f / 0.8f);
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

TEST_CASE("zone 1 terrain: size, spawns and placements", "[content]")
{
    TerrainData t;
    std::string err;
    INFO(err);
    REQUIRE(loadTerrain(assetDir() / "terrain" / "zone1" / "zone1.vterrain", t, err));
    CHECK(t.worldSize == 512.0f);
    CHECK((t.resolution == 513 || t.resolution == 1025));
    const SpawnPoint* a = t.findSpawn("spawn_aydor");
    const SpawnPoint* k = t.findSpawn("spawn_kargan");
    REQUIRE(a != nullptr);
    REQUIRE(k != nullptr);
    for (const SpawnPoint* s : {a, k})
    {
        INFO(s->name);
        CHECK(t.contains(s->position.x, s->position.z));
        CHECK(t.isWalkable(s->position.x, s->position.z));
        CHECK(std::abs(s->position.y - t.heightAt(s->position.x, s->position.z)) < 1e-3f);
        // Room to move: the 3 m around a spawn is walkable.
        for (float dx = -3.0f; dx <= 3.0f; dx += 1.0f)
            for (float dz = -3.0f; dz <= 3.0f; dz += 1.0f)
                CHECK(t.isWalkable(s->position.x + dx, s->position.z + dz));
    }
    const float apart = std::hypot(a->position.x - k->position.x, a->position.z - k->position.z);
    CHECK(apart >= 10.0f);
    CHECK(apart <= 80.0f);
    // The outer margin is blocked.
    for (float s = 0.5f; s < 512.0f; s += 16.0f)
    {
        CHECK_FALSE(t.isWalkable(s, 1.0f));
        CHECK_FALSE(t.isWalkable(1.0f, s));
        CHECK_FALSE(t.isWalkable(s, 510.5f));
        CHECK_FALSE(t.isWalkable(510.5f, s));
    }
    for (const TerrainLayer& l : t.layers)
        CHECK(fs::exists(assetDir() / l.texture));
    std::set<std::string> meshes;
    CHECK(t.objects.size() >= 50);
    for (const ObjectPlacement& o : t.objects)
    {
        INFO(o.mesh << " at " << o.position.x << ", " << o.position.z);
        meshes.insert(o.mesh);
        CHECK(t.contains(o.position.x, o.position.z));
        CHECK(std::hypot(o.position.x - a->position.x, o.position.z - a->position.z) >= 3.0f);
        CHECK(std::hypot(o.position.x - k->position.x, o.position.z - k->position.z) >= 3.0f);
        if (o.blocks())
            CHECK_FALSE(t.isWalkable(o.position.x, o.position.z));
    }
    for (const std::string& mesh : meshes)
    {
        INFO(mesh);
        CHECK(fs::exists(assetDir() / mesh));
        MeshData m;
        CHECK(loadMesh(assetDir() / mesh, m, err));
        CHECK_FALSE(m.isSkinned());
    }
    CHECK(meshes.size() >= 10);
}

TEST_CASE("converting a committed source twice gives identical bytes equal to the build output", "[content]")
{
    const fs::path src = sourceDir() / "characters" / "aydor.glb";
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
    REQUIRE(readFileBytes(assetDir() / "characters" / "aydor.vmesh", built, err));
    CHECK(built == first);
}
