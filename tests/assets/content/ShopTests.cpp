// Content checks for the M14 "Pazar" art (tools/blender/gen_npcs.py, gen_shop_props.py,
// gen_shop_icons.py; docs/SHOP_ART.md): the six merchant NPCs (the players' skeleton, three clips, one
// PBR material), the four stall props, the potion item icons and the shop UI art. Runs on the assets
// built by the `assets.build` fixture.

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace vo::assets;
namespace fs = std::filesystem;

namespace {

fs::path assetDir()
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

MeshData requireMesh(const fs::path& p)
{
    MeshData m;
    std::string err;
    INFO(p.string() << ": " << err);
    REQUIRE(loadMesh(p, m, err));
    return m;
}

TextureData requireTexture(const fs::path& p)
{
    TextureData tex;
    std::string err;
    INFO(p.string() << ": " << err);
    REQUIRE(loadTexture(p, tex, err));
    return tex;
}

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// gen_npcs.NPCS: model key (the server's NPC `model`), the player look it is built on, and whether
// something on it glows (potion liquids, the Kargan smith's hot ingot).
struct Npc
{
    const char* key;
    const char* base;
    bool emissive;
};
const Npc kNpcs[] = {{"npc_potion_esik", "aydor_1", true},   {"npc_smith_esik", "kargan_0", false},
                     {"npc_potion_aydor", "aydor_2", true},  {"npc_smith_aydor", "aydor_0", false},
                     {"npc_potion_kargan", "kargan_1", true}, {"npc_smith_kargan", "kargan_2", true}};

const std::pair<const char*, bool> kNpcClips[] = {{"idle", true}, {"idle_talking", true}, {"interact", false}};

std::vector<Mat4> modelPose(const MeshData& m, const AnimationClip* c, const ClipBinding* binding, float t)
{
    std::vector<Transform> local;
    std::vector<Mat4> model;
    if (c == nullptr)
        bindPose(m.skeleton, local);
    else
        samplePose(*c, *binding, m.skeleton, t, false, local);
    localToModel(m.skeleton, local, model);
    return model;
}

float meshTop(const MeshData& m)
{
    float top = -1e9f;
    for (std::uint32_t v = 0; v < m.vertexCount; ++v)
        top = std::max(top, m.position(v).y);
    return top;
}

} // namespace

TEST_CASE("merchant NPCs: the players' skeleton, standing at their base look's height, one PBR material",
          "[content][shop]")
{
    for (const Npc& npc : kNpcs)
    {
        INFO(npc.key);
        const MeshData m = requireMesh(assetDir() / "npcs" / (std::string(npc.key) + ".vmesh"));
        const MeshData base = requireMesh(assetDir() / "characters" / (std::string(npc.base) + ".vmesh"));
        REQUIRE(m.isSkinned());
        REQUIRE(m.skeleton.bones.size() == base.skeleton.bones.size());
        for (std::size_t i = 0; i < m.skeleton.bones.size(); ++i)
        {
            INFO("bone " << base.skeleton.bones[i].name);
            CHECK(m.skeleton.bones[i].name == base.skeleton.bones[i].name);
            CHECK(m.skeleton.bones[i].parent == base.skeleton.bones[i].parent);
        }
        // As tall as the look it is built on (a hood peak or a cap adds up to ~8 cm), feet on the ground.
        const float top = meshTop(m);
        CHECK(top >= meshTop(base) - 0.01f);
        CHECK(top <= meshTop(base) + 0.10f);
        float bottom = 1e9f;
        for (std::uint32_t v = 0; v < m.vertexCount; ++v)
            bottom = std::min(bottom, m.position(v).y);
        CHECK(std::abs(bottom) < 0.03f);
        // Inverse binds match the bind pose; facing +Z like the players.
        const std::vector<Mat4> model = modelPose(m, nullptr, nullptr, 0.0f);
        std::vector<Mat4> skin;
        computeSkinningMatrices(m.skeleton, model, skin);
        for (const Mat4& s : skin)
            CHECK(maxAbsDifference(s, Mat4::identity()) < 1e-3f);
        const auto pos = [&](const char* bone, int axis) {
            return model[static_cast<std::size_t>(m.skeleton.findBone(bone))].m[12 + axis];
        };
        CHECK(pos("hand_l", 0) < -0.2f);
        CHECK(pos("ball_l", 2) > pos("foot_l", 2) + 0.05f);
        // Budget: the body (decimated a little further than the players) plus the outfit.
        const std::uint32_t tris = m.indexCount() / 3;
        CHECK(tris >= 24000);
        CHECK(tris <= 30000); // like the player looks
        CHECK(m.indexSize == (m.vertexCount <= 65535 ? 2u : 4u));
        REQUIRE(m.submeshes.size() == 1);
        const Submesh& s = m.submeshes[0];
        const std::string stem = std::string("npcs/") + npc.key;
        CHECK(s.texture == stem + ".png");
        CHECK(s.normalTexture == stem + "_n.png");
        CHECK(s.ormTexture == stem + "_orm.png");
        CHECK(s.emissiveTexture == (npc.emissive ? stem + "_e.png" : std::string()));
        std::vector<std::string> maps = {s.texture, s.normalTexture, s.ormTexture};
        if (npc.emissive)
            maps.push_back(s.emissiveTexture);
        for (const std::string& p : maps)
        {
            INFO(p);
            const TextureData tex = requireTexture(assetDir() / compiledTexturePath(p));
            CHECK(tex.width() == 1024);
            CHECK(tex.height() == 1024);
        }
    }
}

TEST_CASE("merchant NPC clips: idle, idle_talking and interact, seamless loops, nothing flies off",
          "[content][shop]")
{
    for (const Npc& npc : kNpcs)
    {
        const std::string key = npc.key;
        const MeshData m = requireMesh(assetDir() / "npcs" / (key + ".vmesh"));
        std::set<std::string> files;
        for (const auto& e : fs::directory_iterator(assetDir() / "npcs" / key))
            files.insert(e.path().filename().string());
        std::set<std::string> expected;
        for (const auto& [name, loops] : kNpcClips)
            expected.insert(std::string(name) + ".vanim");
        CHECK(files == expected);
        const float height = meshTop(m);
        for (const auto& [name, loops] : kNpcClips)
        {
            INFO(key << "/" << name);
            AnimationFile f;
            std::string err;
            REQUIRE(loadAnimation(assetDir() / "npcs" / key / (std::string(name) + ".vanim"), f, err));
            REQUIRE(f.clips.size() == 1);
            const AnimationClip& c = f.clips[0];
            CHECK(c.name == name);
            CHECK(c.loop == loops);
            CHECK(c.moveSpeed == 0.0f);
            const ClipBinding binding = bindClip(c, m.skeleton);
            REQUIRE(binding.size() == m.skeleton.bones.size());
            std::vector<std::vector<Mat4>> poses(c.frameCount);
            for (std::uint32_t k = 0; k < c.frameCount; ++k)
                poses[k] = modelPose(m, &c, &binding, static_cast<float>(k) / c.sampleRate);
            if (loops)
            {
                float seam = 0.0f;
                for (std::size_t b = 0; b < m.skeleton.bones.size(); ++b)
                    seam = std::max(seam, maxAbsDifference(poses.front()[b], poses.back()[b]));
                CHECK(seam < 1e-4f);
            }
            // Every skinned vertex (outfit included) stays near the standing body.
            const auto pelvis = static_cast<std::size_t>(m.skeleton.findBone("pelvis"));
            std::vector<Mat4> skin;
            float far = 0.0f, lowest = 1e9f;
            for (std::uint32_t k = 0; k < c.frameCount; k += 4)
            {
                computeSkinningMatrices(m.skeleton, poses[k], skin);
                const Vec3 hip{poses[k][pelvis].m[12], poses[k][pelvis].m[13], poses[k][pelvis].m[14]};
                for (std::uint32_t v = 0; v < m.vertexCount; v += 5)
                {
                    const Vec3 p = skinPosition(m, v, skin);
                    far = std::max(far, length(p - hip));
                    lowest = std::min(lowest, p.y);
                }
            }
            CHECK(far < 0.8f * height);
            CHECK(lowest > -0.06f);
        }
    }
}

TEST_CASE("M14 stall props: static, grounded, within budget, PBR and emissive maps", "[content][shop]")
{
    const char* const props[] = {"aydor_potion_stall", "aydor_smithy", "kargan_potion_stall", "kargan_smithy"};
    for (const char* name : props)
    {
        INFO(name);
        const MeshData m = requireMesh(assetDir() / "props" / (std::string(name) + ".vmesh"));
        CHECK_FALSE(m.isSkinned());
        const std::uint32_t tris = m.indexCount() / 3;
        CHECK(tris >= 300);
        CHECK(tris <= 9000);
        const Vec3 size = m.boundsMax - m.boundsMin;
        CHECK(size.x > 2.5f); // a stall / a smithy corner, not a crate
        CHECK(size.x < 4.5f);
        CHECK(size.y > 2.0f);
        CHECK(size.y < 3.2f);
        CHECK(m.boundsMin.y <= 0.0f);
        CHECK(m.boundsMin.y > -0.1f);
        REQUIRE(m.submeshes.size() == 1);
        const Submesh& s = m.submeshes[0];
        const std::string stem = std::string("props/") + name;
        CHECK(s.texture == stem + ".png");
        CHECK(s.emissiveTexture == stem + "_e.png");
        for (const std::string& p : {s.texture, s.normalTexture, s.ormTexture, s.emissiveTexture})
        {
            INFO(p);
            CHECK(fs::exists(assetDir() / vo::core::pathFromUtf8(compiledTexturePath(p))));
        }
    }
}

TEST_CASE("potion icons: 128x128 premultiplied RGBA8 like the other item icons; medium larger than small",
          "[content][shop]")
{
    const char* const icons[] = {"potion_hp_small", "potion_hp_medium", "potion_mp_small", "potion_mp_medium"};
    std::vector<std::size_t> coverage;
    std::vector<double> redness;
    for (const char* name : icons)
    {
        INFO(name);
        const TextureData tex = requireTexture(assetDir() / "ui" / "items" / (std::string(name) + ".vtex"));
        CHECK(tex.format == TextureFormat::Rgba8);
        CHECK(tex.usage == TextureUsage::Color);
        REQUIRE(tex.width() == 128);
        REQUIRE(tex.height() == 128);
        CHECK(tex.mips.size() == 8);
        const auto& px = tex.mips[0].data;
        CHECK(px[3] == 0);
        CHECK(px[(127 * 128 + 127) * 4 + 3] == 0);
        std::size_t opaque = 0;
        bool premultiplied = true;
        double r = 0.0, b = 0.0;
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            const float a = static_cast<float>(px[i + 3]) / 255.0f;
            opaque += px[i + 3] > 200 ? 1 : 0;
            for (std::size_t c = 0; c < 3; ++c)
                premultiplied = premultiplied && srgbToLinear(static_cast<float>(px[i + c]) / 255.0f) <= a + 0.01f;
            r += px[i];
            b += px[i + 2];
        }
        CHECK(premultiplied);
        CHECK(opaque > 128 * 128 / 10);
        coverage.push_back(opaque);
        redness.push_back(r / std::max(b, 1.0));
    }
    // Same frame for all: the medium bottle covers more of the slot than the small flask.
    CHECK(coverage[1] > coverage[0] + coverage[0] / 10);
    CHECK(coverage[3] > coverage[2] + coverage[2] / 10);
    // Health is the red family, mana the blue one.
    CHECK(redness[0] > 1.2);
    CHECK(redness[1] > 1.2);
    CHECK(redness[2] < 0.9);
    CHECK(redness[3] < 0.9);
}

TEST_CASE("shop UI art: the coin, the tab glyphs and the two merchant markers compile to RGBA8 .vtex",
          "[content][shop]")
{
    const std::pair<const char*, std::uint32_t> files[] = {
        {"akce", 64}, {"tab_al", 64}, {"tab_sat", 64}, {"marker_potion", 128}, {"marker_smith", 128}};
    for (const auto& [name, size] : files)
    {
        INFO(name);
        CHECK(fs::exists(assetDir() / "ui" / "shop" / (std::string(name) + ".png")));
        const TextureData tex = requireTexture(assetDir() / "ui" / "shop" / (std::string(name) + ".vtex"));
        CHECK(tex.format == TextureFormat::Rgba8);
        CHECK(tex.width() == size);
        CHECK(tex.height() == size);
        CHECK(tex.mips.size() > 1);
        const auto& px = tex.mips[0].data;
        CHECK(px[3] == 0); // symbol on a transparent background
        std::size_t opaque = 0;
        for (std::size_t i = 3; i < px.size(); i += 4)
            opaque += px[i] > 200 ? 1 : 0;
        CHECK(opaque > static_cast<std::size_t>(size) * size / 6);
    }
}
