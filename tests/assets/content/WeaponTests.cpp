// Content checks for the M8 weapons (tools/blender/gen_weapons.py, gen_attachments.py): weapon meshes,
// inventory icons (premultiplied RGBA8 .vtex) and gamedata/attachments.toml against the six character
// skeletons and gamedata/items.toml. Runs on the assets built by the `assets.build` fixture; the
// working directory is the repository root (gamedata/ is read from there).

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"

#include <catch2/catch_test_macros.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <tuple>

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

fs::path assetSourceDir()
{
    char* value = nullptr;
    std::size_t size = 0;
    fs::path result = "assets_src";
    if (_dupenv_s(&value, &size, "VO_TEST_ASSET_SRC") == 0 && value != nullptr)
    {
        result = vo::core::pathFromUtf8(value);
        std::free(value);
    }
    return result;
}

struct Weapon
{
    const char* model;
    const char* grip;
    float length; // metres along the grip frame's +Y (shield: diameter)
    std::uint32_t maxTris;
};
const Weapon kWeapons[] = {{"sword_soldier", "sword", 0.95f, 2000},  {"shield_oak", "shield", 0.70f, 3000},
                           {"dagger_hunter", "dagger", 0.45f, 1500}, {"staff_ember", "staff", 1.70f, 3000},
                           {"mace_sun", "mace", 0.60f, 3000},        {"axe_hearth", "axe2h", 1.40f, 3000},
                           {"sword_keep", "sword", 1.00f, 2400},     {"staff_frost", "staff", 1.70f, 3000}};
const char* const kLooks[] = {"aydor_0", "aydor_1", "aydor_2", "kargan_0", "kargan_1", "kargan_2"};
// Grip -> hands it is attached to (gen_attachments.GRIPS).
const std::map<std::string, std::set<std::string>> kGripHands = {
    {"sword", {"main", "off"}}, {"dagger", {"main", "off"}}, {"mace", {"main", "off"}},
    {"staff", {"main"}},        {"axe2h", {"main"}},         {"shield", {"off"}}};

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

struct Attach
{
    std::string bone;
    Vec3 t;
    Quat q;
    float s = 0.0f;
};
using AttachKey = std::tuple<std::string, std::string, std::string>; // grip, look, hand

std::map<AttachKey, Attach> loadAttachments()
{
    toml::table doc;
    try
    {
        doc = toml::parse_file("gamedata/attachments.toml");
    }
    catch (const toml::parse_error& e)
    {
        FAIL("gamedata/attachments.toml: " << e.description());
    }
    REQUIRE(doc["version"].value<int>() == 1);
    std::map<AttachKey, Attach> out;
    const toml::array* rows = doc["attach"].as_array();
    REQUIRE(rows != nullptr);
    for (const toml::node& n : *rows)
    {
        const toml::table& r = *n.as_table();
        Attach a;
        a.bone = r["bone"].value_or(std::string{});
        const toml::array& t = *r["translation"].as_array();
        const toml::array& q = *r["rotation"].as_array();
        REQUIRE(t.size() == 3);
        REQUIRE(q.size() == 4);
        a.t = {t[0].value_or(0.0f), t[1].value_or(0.0f), t[2].value_or(0.0f)};
        a.q = {q[0].value_or(0.0f), q[1].value_or(0.0f), q[2].value_or(0.0f), q[3].value_or(0.0f)};
        a.s = r["scale"].value_or(0.0f);
        const AttachKey key{r["grip"].value_or(std::string{}), r["look"].value_or(std::string{}),
                            r["hand"].value_or(std::string{})};
        INFO(std::get<0>(key) << "/" << std::get<1>(key) << "/" << std::get<2>(key));
        CHECK(out.emplace(key, a).second); // no duplicates
    }
    return out;
}

MeshData requireMesh(const fs::path& p)
{
    MeshData m;
    std::string err;
    INFO(p.string() << ": " << err);
    REQUIRE(loadMesh(p, m, err));
    return m;
}

} // namespace

TEST_CASE("weapon meshes are static, within budget, in the grip frame and textured", "[content][weapons]")
{
    for (const Weapon& w : kWeapons)
    {
        INFO(w.model);
        const MeshData m = requireMesh(assetDir() / "weapons" / (std::string(w.model) + ".vmesh"));
        CHECK_FALSE(m.isSkinned());
        const std::uint32_t tris = m.indexCount() / 3;
        CHECK(tris <= w.maxTris);
        CHECK(tris >= 1000);
        // Grip origin inside the box; length along +Y (shields: the disc spans X and Y, face +Z).
        CHECK(m.boundsMin.x < 0.0f);
        CHECK(m.boundsMax.x > 0.0f);
        CHECK(m.boundsMin.y < 0.0f);
        CHECK(m.boundsMax.y > 0.0f);
        const float len = m.boundsMax.y - m.boundsMin.y;
        CHECK(std::abs(len - w.length) < 0.05f * w.length); // the dagger leans off its handle: 0.469 m
        if (std::string(w.grip) == "shield")
            CHECK(std::abs((m.boundsMax.x - m.boundsMin.x) - w.length) < 0.04f * w.length);
        else // the business end is up: more of the weapon above the grip than below
            CHECK(m.boundsMax.y > -m.boundsMin.y);
        REQUIRE(m.submeshes.size() == 1);
        const Submesh& s = m.submeshes[0];
        for (const std::string& tex : {s.texture, s.normalTexture, s.ormTexture})
        {
            INFO(tex);
            REQUIRE_FALSE(tex.empty());
            CHECK(fs::exists(assetDir() / vo::core::pathFromUtf8(compiledTexturePath(tex))));
        }
    }
}

TEST_CASE("inventory icons are 128x128 premultiplied RGBA8 .vtex with transparent corners", "[content][weapons]")
{
    for (const Weapon& w : kWeapons)
    {
        INFO(w.model);
        CHECK(fs::exists(assetDir() / "ui" / "items" / (std::string(w.model) + ".png")));
        TextureData tex;
        std::string err;
        INFO(err);
        REQUIRE(loadTexture(assetDir() / "ui" / "items" / (std::string(w.model) + ".vtex"), tex, err));
        CHECK(tex.format == TextureFormat::Rgba8);
        CHECK(tex.usage == TextureUsage::Color);
        REQUIRE(tex.mips.size() == 8); // 128 .. 1
        REQUIRE(tex.mips[0].width == 128);
        REQUIRE(tex.mips[0].height == 128);
        const auto& px = tex.mips[0].data;
        CHECK(px[3] == 0); // top-left corner
        CHECK(px[(127 * 128 + 127) * 4 + 3] == 0);
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
        CHECK(opaque > 128 * 128 / 20); // the weapon covers a real part of the frame
    }
}

TEST_CASE("attachments cover every grip, look and hand and name existing hand bones", "[content][weapons]")
{
    const std::map<AttachKey, Attach> att = loadAttachments();
    std::vector<std::string> looks(std::begin(kLooks), std::end(kLooks));
    looks.push_back("*");
    for (const std::string& look : looks)
    {
        const std::string meshLook = look == "*" ? "aydor_0" : look;
        const MeshData body = requireMesh(assetDir() / "characters" / (meshLook + ".vmesh"));
        for (const auto& [grip, hands] : kGripHands)
            for (const std::string& hand : hands)
            {
                INFO(grip << "/" << look << "/" << hand);
                const auto it = att.find({grip, look, hand});
                REQUIRE(it != att.end());
                const Attach& a = it->second;
                CHECK(a.bone == (hand == "main" ? "hand_r" : "hand_l"));
                const int b = body.skeleton.findBone(a.bone);
                REQUIRE(b >= 0);
                CHECK(std::abs(a.q.x * a.q.x + a.q.y * a.q.y + a.q.z * a.q.z + a.q.w * a.q.w - 1.0f) < 1e-4f);
                CHECK(std::abs(a.s - 1.0f) < 1e-3f);
                // Engine maths at bind: weaponModel = boneModel * T * R * S. The grip lies in the hand
                // (a few cm from the wrist joint, at most a hand length away).
                Mat4 boneModel;
                REQUIRE(invert(body.skeleton.bones[static_cast<std::size_t>(b)].inverseBind, boneModel));
                const Mat4 weapon = boneModel * composeTrs(a.t, a.q, {a.s, a.s, a.s});
                const Vec3 wrist = transformPoint(boneModel, {0, 0, 0});
                const float d = length(transformPoint(weapon, {0, 0, 0}) - wrist);
                CHECK(d > 0.04f);
                CHECK(d < 0.25f);
                // The hand is on the character's own side (engine +X = right; the body faces +Z).
                const float side = transformPoint(weapon, {0, 0, 0}).x;
                CHECK((hand == "main" ? side > 0.2f : side < -0.2f));
                if (grip == "shield") // the face (+Z of the grip frame) looks outwards, to the left
                    CHECK(transformVector(weapon, {0, 0, 1}).x < -0.5f);
            }
    }
    // Every row is for a known grip / hand and a hand bone.
    for (const auto& [key, a] : att)
    {
        INFO(std::get<0>(key) << "/" << std::get<1>(key) << "/" << std::get<2>(key));
        REQUIRE(kGripHands.count(std::get<0>(key)) == 1);
        CHECK(kGripHands.at(std::get<0>(key)).count(std::get<2>(key)) == 1);
    }
}

TEST_CASE("every item in gamedata/items.toml has its model, icon and attachments", "[content][weapons]")
{
    toml::table doc;
    try
    {
        doc = toml::parse_file("gamedata/items.toml");
    }
    catch (const toml::parse_error& e)
    {
        FAIL("gamedata/items.toml: " << e.description());
    }
    const std::map<AttachKey, Attach> att = loadAttachments();
    const toml::array* items = doc["item"].as_array();
    REQUIRE(items != nullptr);
    REQUIRE(items->size() > 0);
    for (const toml::node& n : *items)
    {
        const toml::table& it = *n.as_table();
        const std::string model = it["model"].value_or(std::string{});
        const std::string icon = it["icon"].value_or(std::string{});
        const std::string grip = it["grip"].value_or(std::string{});
        const std::string slot = it["slot"].value_or(std::string{});
        const std::string kind = it["kind"].value_or(std::string{});
        INFO(model << " / " << icon << " / " << grip << " / " << slot);
        // An icon whose source the art pipeline has delivered must be compiled; one still pending
        // (M10 icons are produced separately) is only reported: the client draws a placeholder.
        if (fs::exists(assetSourceDir() / "ui" / "items" / (icon + ".png")))
            CHECK(fs::exists(assetDir() / "ui" / "items" / (icon + ".vtex")));
        else
            WARN("item icon '" << icon << "' has no source in assets_src/ui/items yet");
        if (kind == "material")
        {
            CHECK(model.empty()); // materials are never held: no mesh, no attachments
            continue;
        }
        CHECK(fs::exists(assetDir() / "weapons" / (model + ".vmesh")));
        std::set<std::string> hands;
        if (slot == "main_hand" || slot == "two_hand" || slot == "either_hand")
            hands.insert("main");
        if (slot == "off_hand" || slot == "either_hand")
            hands.insert("off");
        CHECK_FALSE(hands.empty());
        for (const std::string& hand : hands)
            for (const char* look : kLooks)
            {
                INFO(look << " " << hand);
                CHECK(att.count({grip, look, hand}) == 1);
            }
    }
}
