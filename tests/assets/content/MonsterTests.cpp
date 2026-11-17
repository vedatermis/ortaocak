// Content checks for the M9 combat art (tools/blender/gen_monsters.py, gen_effects.py): the three
// humanoid monsters (the players' skeleton and clips, PBR + emissive maps), the Mage's ember bolt and
// the four skill icons. Runs on the assets built by the `assets.build` fixture.

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
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

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// gen_monsters.MONSTERS: model name (gamedata/monsters.toml `model`), authored height (m).
struct Monster
{
    const char* model;
    float height;
    float deathDip = 0.35f; // how far the lying body may dip below the ground in the death clip
};
const Monster kMonsters[] = {{"ash_thrall", 1.50f},          {"slag_walker", 2.00f},
                             {"ember_warden", 2.05f},        {"stubble_thrall", 1.50f}, // M13: recoloured sources
                             {"burnt_sentry", 1.80f, 0.6f},  {"rime_rock", 2.00f},
                             {"frozen_hearthguard", 2.00f}};

// gen_monsters.MONSTER_CLIPS: the clips the client plays on monsters (docs/PROTOCOL_COMBAT.md §5).
const std::pair<const char*, bool> kMonsterClips[] = {
    {"idle", true},         {"walk", true},         {"run", true},          {"punch_jab", false},
    {"punch_cross", false}, {"spell_enter", false}, {"spell_idle", true},   {"spell_shoot", false},
    {"spell_exit", false},  {"hit_chest", false},   {"death", false}};

constexpr std::uint32_t kMonsterTexture = 1024;

const char* const kSkillIcons[] = {
    "agir_darbe", "cifte_kesik", "koz_oku", "soluk_gunes",
    // M12 branch skills (tools/blender/gen_skill_art.py)
    "yarma", "kan_ofkesi", "sarsinti", "kalkan_duvari", "meydan_okuma", "direncli_soluk", "sirt_hanceri",
    "golge_adimi", "seri_eller", "zehirli_kesik", "sis_bombasi", "kacamak", "yakici_iz", "alev_halkasi",
    "kor_yagmuru", "kiragi_oku", "buz_kabugu", "donduran_nefes", "yenileyen_isik", "kutlu_siper", "halka_sifasi",
    "isik_carpmasi", "baglayan_soz", "ofkeli_dua"};

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

} // namespace

TEST_CASE("monsters use the players' skeleton, stand upright at their height and carry PBR + emissive maps",
          "[content][monsters]")
{
    const MeshData player = requireMesh(assetDir() / "characters" / "aydor_0.vmesh");
    for (const Monster& mon : kMonsters)
    {
        INFO(mon.model);
        const MeshData m = requireMesh(assetDir() / "monsters" / (std::string(mon.model) + ".vmesh"));
        REQUIRE(m.isSkinned());
        // Same bone names, order and parents as the player looks (the clips and the client's bone
        // lookups are shared).
        REQUIRE(m.skeleton.bones.size() == player.skeleton.bones.size());
        CHECK(m.skeleton.bones.size() == 59);
        for (std::size_t i = 0; i < m.skeleton.bones.size(); ++i)
        {
            const Bone& a = player.skeleton.bones[i];
            const Bone& b = m.skeleton.bones[i];
            INFO("bone " << a.name);
            CHECK(a.name == b.name);
            CHECK(a.parent == b.parent);
        }
        // Height: the authored height, feet on the ground (y up in engine space).
        float top = -1e9f, bottom = 1e9f;
        for (std::uint32_t v = 0; v < m.vertexCount; ++v)
        {
            top = std::max(top, m.position(v).y);
            bottom = std::min(bottom, m.position(v).y);
        }
        CHECK(std::abs(top - mon.height) < 0.03f);
        CHECK(std::abs(bottom) < 0.02f);
        // Facing +Z, left side (bones *_l) at -X, a head on top, inverse binds match the bind pose.
        const std::vector<Mat4> model = modelPose(m, nullptr, nullptr, 0.0f);
        const auto pos = [&](const char* bone, int axis) {
            return model[static_cast<std::size_t>(m.skeleton.findBone(bone))].m[12 + axis];
        };
        CHECK(pos("hand_l", 0) < -0.2f);
        CHECK(pos("hand_r", 0) > 0.2f);
        CHECK(pos("ball_l", 2) > pos("foot_l", 2) + 0.05f);
        CHECK(pos("Head", 1) > 0.75f * mon.height);
        CHECK(pos("pelvis", 1) > 0.4f * mon.height);
        std::vector<Mat4> skin;
        computeSkinningMatrices(m.skeleton, model, skin);
        for (const Mat4& s : skin)
            CHECK(maxAbsDifference(s, Mat4::identity()) < 1e-3f);
        // Budget like the player looks.
        std::uint32_t tris = 0;
        for (const Submesh& s : m.submeshes)
            tris += s.indexCount / 3;
        CHECK(tris >= 20000);
        CHECK(tris <= 30000);
        CHECK(m.indexSize == (m.vertexCount <= 65535 ? 2u : 4u));
        // One textured PBR material with an emissive map (glowing cracks / eyes / crystal).
        REQUIRE(m.submeshes.size() == 1);
        const Submesh& s = m.submeshes[0];
        const std::string base = std::string("monsters/") + mon.model;
        CHECK(s.variantGroup == VariantGroup::Always);
        CHECK(s.texture == base + ".png");
        CHECK(s.normalTexture == base + "_n.png");
        CHECK(s.ormTexture == base + "_orm.png");
        CHECK(s.emissiveTexture == base + "_e.png");
        CHECK(s.emissiveFactor[0] >= 1.0f);
        CHECK(s.emissiveFactor[0] <= 6.0f);
        const std::pair<const std::string*, TextureUsage> maps[] = {{&s.texture, TextureUsage::Color},
                                                                    {&s.normalTexture, TextureUsage::Normal},
                                                                    {&s.ormTexture, TextureUsage::Data},
                                                                    {&s.emissiveTexture, TextureUsage::Color}};
        for (const auto& [path, usage] : maps)
        {
            INFO(*path);
            CHECK(fs::exists(assetDir() / vo::core::pathFromUtf8(*path)));
            TextureData tex;
            std::string err;
            REQUIRE(loadTexture(assetDir() / compiledTexturePath(*path), tex, err));
            CHECK(tex.usage == usage);
            CHECK(tex.width() == kMonsterTexture);
            CHECK(tex.height() == kMonsterTexture);
        }
        // The emissive map lights a small part of the atlas (cracks, eyes, crystal), not the body.
        TextureData em;
        std::string err;
        REQUIRE(loadTexture(assetDir() / compiledTexturePath(s.emissiveTexture), em, err));
        std::vector<std::uint8_t> rgba;
        REQUIRE(decodeTextureMip(em, 0, rgba, err));
        std::size_t lit = 0;
        for (std::size_t i = 0; i < rgba.size(); i += 4)
            lit += (rgba[i] > 60) ? 1 : 0;
        const double frac = static_cast<double>(lit) / static_cast<double>(rgba.size() / 4);
        CHECK(frac > 0.0002);
        CHECK(frac < 0.15);
    }
}

TEST_CASE("monster clips: exactly the combat set, seamless loops, no exploding vertices, planted feet",
          "[content][monsters]")
{
    for (const Monster& mon : kMonsters)
    {
        const std::string model = mon.model;
        const MeshData m = requireMesh(assetDir() / "monsters" / (model + ".vmesh"));
        // Exactly the monster clip set (nothing else is exported, to keep the files small).
        std::set<std::string> files;
        for (const auto& e : fs::directory_iterator(assetDir() / "monsters" / model))
            files.insert(e.path().filename().string());
        std::set<std::string> expected;
        for (const auto& [name, loops] : kMonsterClips)
            expected.insert(std::string(name) + ".vanim");
        CHECK(files == expected);
        for (const auto& [name, loops] : kMonsterClips)
        {
            INFO(model << "/" << name);
            AnimationFile f;
            std::string err;
            REQUIRE(loadAnimation(assetDir() / "monsters" / model / (std::string(name) + ".vanim"), f, err));
            REQUIRE(f.clips.size() == 1);
            const AnimationClip& c = f.clips[0];
            CHECK(c.name == name);
            CHECK(c.loop == loops);
            CHECK(c.frameCount >= 10);
            const ClipBinding binding = bindClip(c, m.skeleton);
            REQUIRE(binding.size() == m.skeleton.bones.size());
            for (int b : binding)
                CHECK(b >= 0);
            // moveSpeed: the clip's natural planted-foot speed, scaled with the monster (the client
            // plays at rate = server move_speed / moveSpeed).
            const std::string n = name;
            if (n == "walk")
            {
                CHECK(c.moveSpeed > 0.9f);
                CHECK(c.moveSpeed < 2.0f);
            }
            else if (n == "run")
            {
                CHECK(c.moveSpeed > 3.5f);
                CHECK(c.moveSpeed < 8.0f);
            }
            else
                CHECK(c.moveSpeed == 0.0f);
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
            // No exploding vertices: every skinned vertex stays within the standing body's reach of
            // the pelvis (the death clip falls away from the bind-pose bounds, so the pelvis of the
            // frame is the reference, not the bounding sphere).
            const auto pelvis = static_cast<std::size_t>(m.skeleton.findBone("pelvis"));
            std::vector<Mat4> skin;
            float far = 0.0f, lowest = 1e9f;
            for (std::uint32_t k = 0; k < c.frameCount; k += 3)
            {
                computeSkinningMatrices(m.skeleton, poses[k], skin);
                const Vec3 hip{poses[k][pelvis].m[12], poses[k][pelvis].m[13], poses[k][pelvis].m[14]};
                for (std::uint32_t v = 0; v < m.vertexCount; v += 7)
                {
                    const Vec3 p = skinPosition(m, v, skin);
                    far = std::max(far, length(p - hip));
                    lowest = std::min(lowest, p.y);
                }
            }
            CHECK(far < 0.85f * mon.height);
            // Nothing sinks deep into the ground. Lying dead, the library clip puts the spine at a
            // slim mannequin's depth, so the warden's pauldrons and the slag walker's shoulder rock
            // dip up to ~0.3 m below the ground (known, hidden by the terrain).
            // burnt_sentry is the aydor_2 look: its plate pauldrons and the surcoat on the cloth helper
            // bones go ~0.56 m under while it lies (the player looks share that clip, untested there).
            CHECK(lowest > (n == "death" ? -mon.deathDip : -0.10f));
            if (n == "walk" || n == "run")
            {
                std::vector<Mat4> bindModel = modelPose(m, nullptr, nullptr, 0.0f);
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
                        slide = std::max(slide, std::abs(p1.m[14] - p0.m[14] + c.moveSpeed / c.sampleRate) +
                                                    std::abs(p1.m[12] - p0.m[12]));
                    }
                }
                // The run's flat contacts are short (the slag walker's boulder feet roll: 2 pairs).
                CHECK(planted >= (n == "walk" ? 4 : 2));
                CHECK(slide < 0.004f);
            }
        }
    }
}

TEST_CASE("ember bolt: a tiny emissive projectile pointing along +Z", "[content][monsters]")
{
    const MeshData m = requireMesh(assetDir() / "effects" / "ember_bolt.vmesh");
    CHECK_FALSE(m.isSkinned());
    const std::uint32_t tris = m.indexCount() / 3;
    CHECK(tris >= 100);
    CHECK(tris <= 300);
    const Vec3 size = m.boundsMax - m.boundsMin;
    CHECK(std::abs(size.z - 0.5f) < 0.05f); // length along Z
    CHECK(size.x < 0.25f);
    CHECK(size.y < 0.25f);
    // The rounded head is at +Z, the long tail behind it: the widest cross-section lies in front
    // of the middle and the bounds reach further back than forward.
    CHECK(m.boundsMax.z < -m.boundsMin.z);
    float widestZ = 0.0f, widest = 0.0f;
    for (std::uint32_t v = 0; v < m.vertexCount; ++v)
    {
        const Vec3 p = m.position(v);
        const float r = std::hypot(p.x, p.y);
        if (r > widest)
        {
            widest = r;
            widestZ = p.z;
        }
    }
    CHECK(widestZ > 0.5f * (m.boundsMin.z + m.boundsMax.z));
    REQUIRE(m.submeshes.size() == 1);
    const Submesh& s = m.submeshes[0];
    CHECK(s.texture == "effects/ember_bolt.png");
    CHECK(s.emissiveTexture == "effects/ember_bolt_e.png");
    CHECK(s.emissiveFactor[0] >= 1.0f);
    for (const std::string& t : {s.texture, s.emissiveTexture})
    {
        INFO(t);
        CHECK(fs::exists(assetDir() / vo::core::pathFromUtf8(compiledTexturePath(t))));
    }
}

TEST_CASE("skill icons are 64x64 premultiplied RGBA8 .vtex with transparent corners", "[content][monsters]")
{
    for (const char* id : kSkillIcons)
    {
        INFO(id);
        CHECK(fs::exists(assetDir() / "ui" / "skills" / (std::string(id) + ".png")));
        TextureData tex;
        std::string err;
        INFO(err);
        REQUIRE(loadTexture(assetDir() / "ui" / "skills" / (std::string(id) + ".vtex"), tex, err));
        CHECK(tex.format == TextureFormat::Rgba8);
        CHECK(tex.usage == TextureUsage::Color);
        REQUIRE(tex.mips.size() == 7); // 64 .. 1
        REQUIRE(tex.mips[0].width == 64);
        REQUIRE(tex.mips[0].height == 64);
        const auto& px = tex.mips[0].data;
        CHECK(px[3] == 0); // rounded badge: transparent corners
        CHECK(px[(63 * 64 + 63) * 4 + 3] == 0);
        std::size_t opaque = 0;
        bool premultiplied = true;
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            const float a = static_cast<float>(px[i + 3]) / 255.0f;
            opaque += px[i + 3] > 200 ? 1 : 0;
            for (std::size_t c = 0; c < 3; ++c)
                premultiplied = premultiplied && srgbToLinear(static_cast<float>(px[i + c]) / 255.0f) <= a + 0.01f;
        }
        CHECK(premultiplied);
        CHECK(opaque > 64 * 64 * 3 / 4); // a full badge
        // Legible: the symbol contrasts with the badge (bright texels on a darker ground).
        std::size_t bright = 0;
        for (std::size_t i = 0; i < px.size(); i += 4)
            bright += (px[i] > 200 || px[i + 1] > 200) ? 1 : 0;
        CHECK(bright > 64 * 64 / 20);
    }
}

TEST_CASE("M12 status icons and skill bar pieces compile to premultiplied RGBA8 .vtex", "[content][monsters]")
{
    const char* const files[] = {"status/attack_up", "status/poison", "status/stun", "status/taunted",
                                 "skillbar/slot_frame_normal", "skillbar/slot_empty", "skillbar/slot_locked",
                                 "skillbar/tree_v_lit", "skillbar/tree_elbow_se_unlit", "skillbar/branch_kiragi",
                                 "skillbar/rank_pips_3", "skillbar/skill_point_marker"};
    for (const char* f : files)
    {
        INFO(f);
        TextureData tex;
        std::string err;
        REQUIRE(loadTexture(assetDir() / "ui" / vo::core::pathFromUtf8(std::string(f) + ".vtex"), tex, err));
        CHECK(tex.format == TextureFormat::Rgba8);
        CHECK(tex.usage == TextureUsage::Color);
        CHECK(tex.mips.size() > 1);
    }
}

TEST_CASE("M12 skill effects: small static emissive meshes", "[content][monsters]")
{
    const char* const effects[] = {"frost_bolt",          "ember_mote",          "light_strike",
                                   "ground_ring_flame",   "ground_ring_frost",   "ground_ring_heal",
                                   "ground_ring_fog",     "ground_ring_neutral", "shield_bubble_gold",
                                   "shield_bubble_frost", "shield_bubble_neutral", "buff_aura_gold",
                                   "buff_aura_red",       "buff_aura_steel",     "buff_aura_green",
                                   "buff_aura_neutral",   "tick_wisp_burn",      "tick_wisp_poison",
                                   "stun_marker",         "root_spikes",         "leap_trail"};
    for (const char* e : effects)
    {
        INFO(e);
        const MeshData m = requireMesh(assetDir() / "effects" / (std::string(e) + ".vmesh"));
        CHECK_FALSE(m.isSkinned());
        CHECK(m.indexCount() / 3 <= 800);
        REQUIRE(m.submeshes.size() == 1);
        CHECK(m.submeshes[0].emissiveFactor[0] >= 1.0f);
        CHECK(fs::exists(assetDir() / vo::core::pathFromUtf8(compiledTexturePath(m.submeshes[0].emissiveTexture))));
    }
    const MeshData fog = requireMesh(assetDir() / "effects" / "fog_cloud.vmesh");
    CHECK(fog.indexCount() / 3 <= 800);
    // The frost bolt points along +Z like the ember bolt (nose in front of the origin, tail behind).
    const MeshData bolt = requireMesh(assetDir() / "effects" / "frost_bolt.vmesh");
    CHECK(bolt.boundsMax.z < -bolt.boundsMin.z);
}
