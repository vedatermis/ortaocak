// Content checks for M17: the wearable armour pieces (tools/blender/gen_armor.py), their icons, the
// forged weapons (tools/blender/gen_forged_weapons.py) and how gamedata/items.toml points at them. Runs
// on the assets built by the `assets.build` fixture; the working directory is the repository root.

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"
#include "shared/game/Items.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <set>
#include <string>

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

const char* const kLooks[] = {"aydor_0", "aydor_1", "aydor_2", "kargan_0", "kargan_1", "kargan_2"};
const char* const kLines[] = {"heavy", "leather", "cloth"};
const char* const kShapes[] = {"a", "b"};
const char* const kSlots[] = {"head", "chest", "legs", "hands", "feet"};

struct Forged
{
    const char* model;
    const char* grip;
    float length; // along the grip frame's +Y (shields: height)
    float width;  // shields: along X; 0 = not checked
};
const Forged kForged[] = {
    {"sword_ridge", "sword", 0.995f, 0.0f},    {"sword_curved", "sword", 0.92f, 0.0f},
    {"sword_broad", "axe2h", 1.55f, 0.0f},     {"dagger_fang", "dagger", 0.454f, 0.0f},
    {"dagger_twin", "dagger", 0.44f, 0.0f},    {"mace_flanged", "mace", 0.79f, 0.0f},
    {"mace_spiked", "mace", 0.74f, 0.0f},      {"hammer_war", "axe2h", 1.40f, 0.0f},
    {"axe_bearded", "axe2h", 1.44f, 0.0f},     {"halberd", "axe2h", 2.18f, 0.0f},
    {"staff_ritual", "staff", 1.88f, 0.0f},    {"staff_crystal", "staff", 1.79f, 0.0f},
    {"shield_tower", "shield", 1.06f, 0.60f},  {"shield_buckler", "shield", 0.44f, 0.44f},
};

const vo::game::ItemCatalog& catalog()
{
    static const vo::game::ItemCatalog c = [] {
        vo::game::ItemCatalog out;
        std::string error;
        INFO(error);
        REQUIRE(vo::game::loadItemCatalog("gamedata/items.toml", out, error));
        return out;
    }();
    return c;
}

void checkIcon(const std::string& icon)
{
    INFO(icon);
    TextureData tex;
    std::string err;
    INFO(err);
    REQUIRE(loadTexture(assetDir() / "ui" / "items" / (icon + ".vtex"), tex, err));
    CHECK(tex.format == TextureFormat::Rgba8);
    REQUIRE(tex.mips[0].width == 128);
    REQUIRE(tex.mips[0].height == 128);
    const auto& px = tex.mips[0].data;
    CHECK(px[3] == 0);
    std::size_t opaque = 0;
    for (std::size_t i = 3; i < px.size(); i += 4)
        opaque += px[i] > 200 ? 1 : 0;
    CHECK(opaque > 128 * 128 / 40); // long thin polearms and staffs cover ~4 %
}

} // namespace

TEST_CASE("armour pieces exist for every look, line, shape and slot and share the look's skeleton",
          "[content][armor]")
{
    for (const char* look : kLooks)
    {
        const MeshData body = requireMesh(assetDir() / "characters" / (std::string(look) + ".vmesh"));
        REQUIRE(body.isSkinned());
        for (const char* line : kLines)
            for (const char* shape : kShapes)
                for (const char* slot : kSlots)
                {
                    const std::string name = std::format("{}_{}_{}", line, shape, slot);
                    INFO(look << " " << name);
                    const MeshData m = requireMesh(assetDir() / "armor" / look / (name + ".vmesh"));
                    REQUIRE(m.isSkinned());
                    CHECK(m.hasColors());
                    // Same skeleton as the body: the client draws the piece with the body's bone palette.
                    REQUIRE(m.skeleton.size() == body.skeleton.size());
                    for (std::size_t b = 0; b < m.skeleton.size(); ++b)
                    {
                        CHECK(m.skeleton.bones[b].name == body.skeleton.bones[b].name);
                        CHECK(m.skeleton.bones[b].parent == body.skeleton.bones[b].parent);
                        float worst = 0.0f;
                        for (int k = 0; k < 16; ++k)
                            worst = std::max(worst, std::abs(m.skeleton.bones[b].inverseBind.m[static_cast<std::size_t>(k)] -
                                                             body.skeleton.bones[b].inverseBind.m[static_cast<std::size_t>(k)]));
                        CHECK(worst < 1e-5f);
                    }
                    const std::uint32_t tris = m.indexCount() / 3;
                    CHECK(tris >= 300);
                    CHECK(tris <= 6000); // a full set stays under ~20k triangles next to a 30k body
                    // Worn on the body: inside its bounds grown by 25 cm (robes, pauldrons, crests).
                    CHECK(m.boundsMin.x > body.boundsMin.x - 0.25f);
                    CHECK(m.boundsMax.x < body.boundsMax.x + 0.25f);
                    CHECK(m.boundsMin.y > body.boundsMin.y - 0.02f);
                    CHECK(m.boundsMax.y < body.boundsMax.y + 0.25f);
                    CHECK(m.boundsMin.z > body.boundsMin.z - 0.25f);
                    CHECK(m.boundsMax.z < body.boundsMax.z + 0.25f);
                }
    }
}

TEST_CASE("armour sits where its slot is on the body", "[content][armor]")
{
    for (const char* look : kLooks)
    {
        const MeshData body = requireMesh(assetDir() / "characters" / (std::string(look) + ".vmesh"));
        const float h = body.boundsMax.y;
        for (const char* line : kLines)
            for (const char* shape : kShapes)
            {
                INFO(look << " " << line << "_" << shape);
                const auto piece = [&](const char* slot) {
                    return requireMesh(assetDir() / "armor" / look / std::format("{}_{}_{}.vmesh", line, shape, slot));
                };
                const MeshData head = piece("head"), chest = piece("chest"), legs = piece("legs"), feet = piece("feet"),
                               hands = piece("hands");
                CHECK(head.boundsMin.y > 0.75f * h);  // above the shoulders
                CHECK(feet.boundsMax.y < 0.45f * h);  // below the knees
                CHECK(feet.boundsMin.y > body.boundsMin.y - 0.005f); // not under the soles
                CHECK(chest.boundsMax.y < head.boundsMax.y);
                CHECK(legs.boundsMax.y < chest.boundsMax.y);
                CHECK(legs.boundsMin.y < 0.4f * h);
                CHECK(hands.boundsMax.x - hands.boundsMin.x > 0.8f); // both hands, out in the A-pose
            }
    }
}

TEST_CASE("armour pieces carry their line's tiling detail maps", "[content][armor]")
{
    // Second pass (ASSET_PIPELINE.md §4h): every piece has at least one surface of its line's material with base
    // colour, normal and ORM maps from armor/textures/ (shared by all pieces, sampled with wrap), and tangents.
    const std::string surface[] = {"steel", "leather", "cloth"};
    const std::set<std::string> stems = {"steel", "trim", "leather", "strap", "cloth", "band"};
    for (const char* look : kLooks)
        for (std::size_t l = 0; l < std::size(kLines); ++l)
            for (const char* shape : kShapes)
                for (const char* slot : kSlots)
                {
                    const std::string name = std::format("{}_{}_{}", kLines[l], shape, slot);
                    INFO(look << " " << name);
                    const MeshData m = requireMesh(assetDir() / "armor" / look / (name + ".vmesh"));
                    CHECK(m.hasTangents());
                    CHECK(m.submeshes.size() <= 5); // draw calls per piece stay few
                    bool lineSurface = false;
                    for (const Submesh& s : m.submeshes)
                    {
                        if (s.texture.empty())
                            continue;
                        INFO(s.texture);
                        REQUIRE(s.texture.rfind("armor/textures/", 0) == 0);
                        const std::string file = s.texture.substr(15);
                        const std::string stem = file.substr(0, file.find("_c."));
                        CHECK(stems.contains(stem));
                        CHECK(s.normalTexture == std::format("armor/textures/{}_n.png", stem));
                        CHECK(s.ormTexture == std::format("armor/textures/{}_orm.png", stem));
                        CHECK(fs::exists(assetDir() / "armor" / "textures" / (stem + "_c.vtex")));
                        lineSurface = lineSurface || stem == surface[l];
                    }
                    CHECK(lineSurface);
                }
}

TEST_CASE("armour icons: one per line and slot, and every armour template is drawn", "[content][armor]")
{
    for (const char* line : kLines)
        for (const char* slot : kSlots)
            checkIcon(std::format("armor_{}_{}", line, slot));
    std::size_t armour = 0;
    for (const vo::game::ItemTemplate& t : catalog().templates())
    {
        if (!t.isArmor())
            continue;
        ++armour;
        INFO(t.id << " " << t.name);
        CHECK_FALSE(t.model.empty());
        CHECK(t.icon.rfind("armor_", 0) == 0);
        CHECK(fs::exists(assetDir() / "ui" / "items" / (t.icon + ".vtex")));
        for (const char* look : kLooks)
            CHECK(fs::exists(assetDir() / "armor" / look / (t.model + ".vmesh")));
    }
    CHECK(armour >= 84);
}

TEST_CASE("forged weapons are static, in the grip frame, within budget, with icons", "[content][weapons][armor]")
{
    for (const Forged& w : kForged)
    {
        INFO(w.model);
        const MeshData m = requireMesh(assetDir() / "weapons" / (std::string(w.model) + ".vmesh"));
        CHECK_FALSE(m.isSkinned());
        const std::uint32_t tris = m.indexCount() / 3;
        CHECK(tris >= 400);
        CHECK(tris <= 2500);
        CHECK(m.boundsMin.x < 0.0f);
        CHECK(m.boundsMax.x > 0.0f);
        CHECK(m.boundsMin.y < 0.0f);
        CHECK(m.boundsMax.y > 0.0f);
        const float len = m.boundsMax.y - m.boundsMin.y;
        CHECK(std::abs(len - w.length) < 0.03f * w.length);
        if (w.width > 0.0f)
        {
            CHECK(std::abs((m.boundsMax.x - m.boundsMin.x) - w.width) < 0.04f * w.width);
            CHECK(m.boundsMax.z > 0.05f); // the face (boss) is in front of the grip, +Z
        }
        else
        {
            CHECK(m.boundsMax.y > -m.boundsMin.y); // business end up
        }
        CHECK(m.hasColors()); // flat PBR colours baked into the vertices
        checkIcon(w.model);
    }
}

TEST_CASE("every weapon template's model and icon exist", "[content][weapons][armor]")
{
    std::set<std::string> forged;
    for (const Forged& w : kForged)
        forged.insert(w.model);
    std::size_t usingForged = 0;
    for (const vo::game::ItemTemplate& t : catalog().templates())
    {
        if (t.kind != vo::game::ItemKind::Weapon && t.kind != vo::game::ItemKind::Shield)
            continue;
        INFO(t.id << " " << t.name);
        CHECK(fs::exists(assetDir() / "weapons" / (t.model + ".vmesh")));
        CHECK(fs::exists(assetDir() / "ui" / "items" / (t.icon + ".vtex")));
        usingForged += forged.contains(t.model) ? 1 : 0;
    }
    CHECK(usingForged >= 8); // the Kor tier shows the new models in game
}
