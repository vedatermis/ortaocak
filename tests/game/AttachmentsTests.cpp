// shared/game/Attachments: the gamedata/attachments.toml loader (strict validation, "*" fallback) and
// the attachment maths of docs/ASSET_PIPELINE.md §4b, checked against the worked example there
// (aydor_0, sword, main hand, bind pose). client/world/WeaponPose on top of it.

#include "client/world/WeaponPose.h"
#include "shared/assets/AssetMath.h"
#include "shared/assets/Mesh.h"
#include "shared/core/Paths.h"
#include "shared/game/Attachments.h"
#include "shared/game/Items.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <set>
#include <string>

namespace vg = vo::game;
namespace va = vo::assets;
using vg::Grip;
using vg::WeaponHand;

namespace {

const vg::AttachmentTable& table()
{
    static const vg::AttachmentTable t = [] {
        vg::AttachmentTable out;
        const auto path = vg::locateAttachmentsFile();
        REQUIRE(path.has_value());
        std::string error;
        INFO(error);
        REQUIRE(vg::loadAttachmentTable(*path, out, error));
        return out;
    }();
    return t;
}

const vg::ItemCatalog& catalog()
{
    static const vg::ItemCatalog c = [] {
        vg::ItemCatalog out;
        const auto path = vg::locateItemsFile();
        REQUIRE(path.has_value());
        std::string error;
        REQUIRE(vg::loadItemCatalog(*path, out, error));
        return out;
    }();
    return c;
}

// Matrix from rows (as printed in ASSET_PIPELINE.md).
va::Mat4 rows(std::initializer_list<float> v)
{
    va::Mat4 m; // callers always pass 16 values (no REQUIRE: also used outside test cases)
    auto it = v.begin();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m.at(r, c) = it != v.end() ? *it++ : 0.0f;
    return m;
}

bool near(va::Vec3 a, va::Vec3 b, float eps)
{
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

// hand_r's bind model matrix of aydor_0 (ASSET_PIPELINE.md §4b "Worked example").
const va::Mat4 kHandR = rows({-0.7660f, 0.6428f, 0.0f, 0.5316f, -0.6428f, -0.7660f, 0.0f, 1.0430f, 0.0f, 0.0f, 1.0f,
                              -0.0665f, 0.0f, 0.0f, 0.0f, 1.0f});

constexpr const char* kMinimal = R"(version = 1
[[attach]]
grip = "sword"
look = "*"
hand = "main"
bone = "hand_r"
translation = [0.1, 0.2, 0.3]
rotation = [0.0, 0.0, 0.0, 1.0]
scale = 1.0
)";

std::string minimalWith(const std::string& from, const std::string& to)
{
    std::string s = kMinimal;
    const auto at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    return s;
}

bool parses(const std::string& text, std::string& error)
{
    vg::AttachmentTable t;
    return vg::parseAttachmentTable(text, "test.toml", t, error);
}

} // namespace

TEST_CASE("attachments.toml: the committed file loads and covers every grip the items use", "[game][attachments]")
{
    const vg::AttachmentTable& t = table();
    CHECK(t.size() == 63);
    const char* looks[] = {"aydor_0", "aydor_1", "aydor_2", "kargan_0", "kargan_1", "kargan_2", "*"};
    for (const vg::ItemTemplate& item : catalog().templates())
    {
        const bool main = item.slot != vg::SlotRule::OffHand;
        const bool off = item.slot == vg::SlotRule::OffHand || item.slot == vg::SlotRule::EitherHand;
        for (const char* look : looks)
        {
            INFO(item.name << " " << look);
            if (main)
            {
                const vg::WeaponAttachment* a = t.find(item.grip, look, WeaponHand::Main);
                REQUIRE(a != nullptr);
                CHECK(a->look == look);
                CHECK(a->bone == "hand_r");
            }
            if (off)
            {
                const vg::WeaponAttachment* a = t.find(item.grip, look, WeaponHand::Off);
                REQUIRE(a != nullptr);
                CHECK(a->bone == "hand_l");
            }
        }
    }
    // Two-handed grips have no off-hand rows; shields no main-hand rows.
    CHECK(t.find(Grip::Axe2h, "aydor_0", WeaponHand::Off) == nullptr);
    CHECK(t.find(Grip::Staff, "*", WeaponHand::Off) == nullptr);
    CHECK(t.find(Grip::Shield, "aydor_0", WeaponHand::Main) == nullptr);
}

TEST_CASE("attachments: an unknown look falls back to the \"*\" row (a copy of aydor_0)", "[game][attachments]")
{
    const vg::WeaponAttachment* any = table().find(Grip::Sword, "aydor_9", WeaponHand::Main);
    const vg::WeaponAttachment* a0 = table().find(Grip::Sword, "aydor_0", WeaponHand::Main);
    REQUIRE(any != nullptr);
    REQUIRE(a0 != nullptr);
    CHECK(any->look == "*");
    CHECK(near(any->translation, a0->translation, 1e-6f));
    CHECK(va::maxAbsDifference(any->matrix(), a0->matrix()) < 1e-6f);
}

TEST_CASE("attachments: worked example of ASSET_PIPELINE.md §4b (aydor_0, sword, main hand, bind pose)",
          "[game][attachments]")
{
    const vg::WeaponAttachment* a = table().find(Grip::Sword, "aydor_0", WeaponHand::Main);
    REQUIRE(a != nullptr);
    // A = T * R * S as printed.
    const va::Mat4 expectedA = rows({-1.0f, 0.0f, 0.0f, 0.0576f, 0.0f, 0.7431f, 0.6691f, 0.0844f, 0.0f, 0.6691f,
                                     -0.7431f, 0.0058f, 0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(va::maxAbsDifference(a->matrix(), expectedA) < 2e-4f);
    // boneModel * A.
    const va::Mat4 expected = rows({0.7660f, 0.4777f, 0.4301f, 0.5417f, 0.6428f, -0.5693f, -0.5126f, 0.9413f, 0.0f,
                                    0.6691f, -0.7431f, -0.0606f, 0.0f, 0.0f, 0.0f, 1.0f});
    const va::Mat4 w = vg::weaponWorldMatrix(va::Mat4::identity(), kHandR, *a);
    CHECK(va::maxAbsDifference(w, expected) < 5e-4f);
    // Grip in the right hand (+X), blade tip forward (+Z) and down.
    CHECK(near(va::transformPoint(w, {0.0f, 0.0f, 0.0f}), {0.542f, 0.941f, -0.061f}, 2e-3f));
    const va::Vec3 tip = va::transformPoint(w, {0.0f, 0.784f, 0.0f});
    CHECK(near(tip, {0.916f, 0.495f, 0.464f}, 2e-3f));
    CHECK(tip.z > 0.0f);
    // The order matters: A * boneModel (the classic mistake) puts the tip elsewhere.
    CHECK(!near(va::transformPoint(a->matrix() * kHandR, {0.0f, 0.784f, 0.0f}), tip, 0.05f));
}

TEST_CASE("attachments: the character's world matrix is applied last", "[game][attachments]")
{
    const vg::WeaponAttachment* a = table().find(Grip::Sword, "aydor_0", WeaponHand::Main);
    REQUIRE(a != nullptr);
    // Character at (10, 2, -5) turned 180 degrees about +Y: the model-space tip (0.916, 0.495, 0.464)
    // ends up at (10 - 0.916, 2 + 0.495, -5 - 0.464).
    const va::Mat4 world = va::composeTrs({10.0f, 2.0f, -5.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    const va::Mat4 w = vg::weaponWorldMatrix(world, kHandR, *a);
    CHECK(near(va::transformPoint(w, {0.0f, 0.784f, 0.0f}), {10.0f - 0.916f, 2.495f, -5.464f}, 3e-3f));
}

TEST_CASE("attachments: the real aydor_0 skeleton gives the documented hand_r bind matrix", "[game][attachments]")
{
    const auto file = vo::core::locateDataFile("assets/characters/aydor_0.vmesh");
    if (!file)
    {
        WARN("assets/characters/aydor_0.vmesh not built; skipped");
        return;
    }
    va::MeshData mesh;
    std::string error;
    REQUIRE(va::loadMesh(*file, mesh, error));
    const int bone = mesh.skeleton.findBone("hand_r");
    REQUIRE(bone >= 0);
    va::Mat4 bind;
    REQUIRE(va::invert(mesh.skeleton.bones[static_cast<std::size_t>(bone)].inverseBind, bind));
    CHECK(va::maxAbsDifference(bind, kHandR) < 2e-3f);
}

TEST_CASE("attachments: strict validation", "[game][attachments]")
{
    std::string error;
    REQUIRE(parses(kMinimal, error));

    const auto rejects = [&](const std::string& text, const std::string& needle) {
        error.clear();
        CHECK_FALSE(parses(text, error));
        INFO(error);
        CHECK(error.find(needle) != std::string::npos);
        CHECK(error.find("test.toml") != std::string::npos);
    };
    rejects(minimalWith("version = 1", "version = 2"), "version");
    rejects(minimalWith("version = 1", "version = 1\nextra = 3"), "unknown key 'extra'");
    rejects(minimalWith("scale = 1.0", "scale = 1.0\ncolour = 1"), "unknown key 'colour'");
    rejects(minimalWith("grip = \"sword\"", "grip = \"spear\""), "unknown grip 'spear'");
    rejects(minimalWith("hand = \"main\"", "hand = \"left\""), "hand");
    rejects(minimalWith("bone = \"hand_r\"", "bone = \"\""), "'bone'");
    rejects(minimalWith("[0.1, 0.2, 0.3]", "[0.1, 0.2]"), "translation");
    rejects(minimalWith("[0.1, 0.2, 0.3]", "[0.1, \"x\", 0.3]"), "translation");
    rejects(minimalWith("[0.1, 0.2, 0.3]", "[12.0, 0.0, 0.0]"), "10 m");
    rejects(minimalWith("[0.0, 0.0, 0.0, 1.0]", "[0.0, 0.0, 0.5, 1.0]"), "unit quaternion");
    rejects(minimalWith("scale = 1.0", "scale = 0.0"), "scale");
    rejects(minimalWith("scale = 1.0", ""), "scale");
    rejects(minimalWith("look = \"*\"", "look = \"aydor_0\""), "fallback");
    rejects(std::string(kMinimal) + std::string(kMinimal).substr(std::string(kMinimal).find("[[attach]]")),
            "duplicate");
    rejects("version = 1\n", "no [[attach]]");
    rejects("version = 1\n[[attach]\n", "test.toml");

    // Integers are accepted where numbers are expected; the quaternion is normalised.
    vg::AttachmentTable t;
    REQUIRE(vg::parseAttachmentTable(minimalWith("[0.0, 0.0, 0.0, 1.0]", "[0, 0, 0, 1.0004]"), "test.toml", t,
                                     error));
    CHECK(std::fabs(t.rows()[0].rotation.w - 1.0f) < 1e-6f);
    // On failure the output is left alone.
    vg::AttachmentTable kept;
    REQUIRE(vg::parseAttachmentTable(kMinimal, "a.toml", kept, error));
    CHECK_FALSE(vg::parseAttachmentTable("version = 1\n", "b.toml", kept, error));
    CHECK(kept.size() == 1);
}

TEST_CASE("weapon pose: both hands, daggers in both, unknown templates and missing bones draw nothing",
          "[client][attachments]")
{
    // A two-bone skeleton with the hands at known model positions.
    va::Skeleton skel;
    skel.bones.push_back({"hand_r", -1, va::Mat4::identity(), {}});
    skel.bones.push_back({"hand_l", -1, va::Mat4::identity(), {}});
    std::vector<va::Mat4> model(2);
    model[0] = va::composeTrs({0.5f, 1.0f, 0.0f}, {}, {1.0f, 1.0f, 1.0f});
    model[1] = va::composeTrs({-0.5f, 1.0f, 0.0f}, {}, {1.0f, 1.0f, 1.0f});
    std::array<vo::client::world::WeaponPlacement, 2> out{};

    // Sword + shield (warrior kit).
    std::size_t n = vo::client::world::placeWeapons(catalog(), table(), "aydor_0", skel, model, va::Mat4::identity(),
                                                    {1, 2}, out);
    REQUIRE(n == 2);
    CHECK(out[0].hand == WeaponHand::Main);
    CHECK(out[0].item->id == 1);
    CHECK(out[1].hand == WeaponHand::Off);
    CHECK(out[1].item->id == 2);
    CHECK(va::transformPoint(out[0].world, {}).x > 0.3f);  // right hand side
    CHECK(va::transformPoint(out[1].world, {}).x < -0.3f); // left hand side

    // Two daggers: the same template in both hands.
    n = vo::client::world::placeWeapons(catalog(), table(), "kargan_1", skel, model, va::Mat4::identity(), {3, 3}, out);
    REQUIRE(n == 2);
    CHECK(out[0].item == out[1].item);

    // Two-handed staff: one weapon. Unknown template / empty hands: nothing.
    CHECK(vo::client::world::placeWeapons(catalog(), table(), "aydor_1", skel, model, va::Mat4::identity(), {4, 0},
                                          out) == 1);
    CHECK(vo::client::world::placeWeapons(catalog(), table(), "aydor_0", skel, model, va::Mat4::identity(), {999, 0},
                                          out) == 0);
    CHECK(vo::client::world::placeWeapons(catalog(), table(), "aydor_0", skel, model, va::Mat4::identity(), {0, 0},
                                          out) == 0);
    // A skeleton without the bone draws nothing (instead of drawing at the origin).
    va::Skeleton other;
    other.bones.push_back({"spine", -1, va::Mat4::identity(), {}});
    CHECK(vo::client::world::placeWeapons(catalog(), table(), "aydor_0", other, model, va::Mat4::identity(), {1, 2},
                                          out) == 0);
    CHECK(vo::client::world::attachmentLookKey(2, 1) == "kargan_1");
    CHECK(vo::client::world::weaponMeshPath(*catalog().find(6)) == "weapons/axe_hearth.vmesh");
    CHECK(vo::client::world::itemIconPath(*catalog().find(2)) == "ui/items/shield_oak.png");
}
