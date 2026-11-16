// Content checks for the M10 loot art (tools/blender/gen_loot.py): the loot bag world prop and the
// loot item icons (premultiplied RGBA8 .vtex, the same rule as the weapon icons). Runs on the assets
// built by the `assets.build` fixture.

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
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

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

TextureData requireTexture(const fs::path& p)
{
    TextureData tex;
    std::string err;
    INFO(p.string() << ": " << err);
    REQUIRE(loadTexture(p, tex, err));
    return tex;
}

TextureData requireIcon(const std::string& name)
{
    return requireTexture(assetDir() / "ui" / "items" / (name + ".vtex"));
}

// New icon -> the existing icon it has to match in size (variants: the weapon icon it was made from).
const std::pair<const char*, const char*> kIcons[] = {
    {"ash_dust", "sword_soldier"},     {"slag_shard", "sword_soldier"},   {"ember_core", "sword_soldier"},
    {"coin", "sword_soldier"},         {"dagger_thrall", "dagger_hunter"}, {"sword_charred", "sword_soldier"},
    {"mace_slag", "mace_sun"},         {"shield_warden", "shield_oak"},
};
const char* const kVariants[] = {"dagger_thrall", "sword_charred", "mace_slag", "shield_warden"};

} // namespace

TEST_CASE("loot bag: a small grounded sack within budget, with PBR and emissive maps", "[content][loot]")
{
    MeshData m;
    std::string err;
    INFO(err);
    REQUIRE(loadMesh(assetDir() / "props" / "loot_bag.vmesh", m, err));
    CHECK_FALSE(m.isSkinned());
    REQUIRE(m.hasTangents());
    const std::uint32_t tris = m.indexCount() / 3;
    CHECK(tris >= 300);
    CHECK(tris <= 1500);
    // About 0.45 m wide and 0.35 m tall (engine axes: +Y up), origin at the ground centre.
    const Vec3 size = m.boundsMax - m.boundsMin;
    CHECK(size.x >= 0.35f);
    CHECK(size.x <= 0.55f);
    CHECK(size.z >= 0.30f);
    CHECK(size.z <= 0.55f);
    CHECK(size.y >= 0.28f);
    CHECK(size.y <= 0.42f);
    CHECK(m.boundsMin.y <= 0.0f); // never floating
    CHECK(m.boundsMin.y >= -0.02f);
    CHECK(std::abs(0.5f * (m.boundsMin.x + m.boundsMax.x)) < 0.05f);
    CHECK(std::abs(0.5f * (m.boundsMin.z + m.boundsMax.z)) < 0.05f);
    // A base resting on the ground, not a point.
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (std::uint32_t v = 0; v < m.vertexCount; ++v)
    {
        const Vec3 q = m.position(v);
        if (q.y > m.boundsMin.y + 0.02f)
            continue;
        lo[0] = std::min(lo[0], q.x);
        hi[0] = std::max(hi[0], q.x);
        lo[1] = std::min(lo[1], q.z);
        hi[1] = std::max(hi[1], q.z);
    }
    CHECK(hi[0] - lo[0] >= 0.15f);
    CHECK(hi[1] - lo[1] >= 0.15f);

    REQUIRE(m.submeshes.size() == 1);
    const Submesh& s = m.submeshes[0];
    CHECK(s.texture == "props/loot_bag.png");
    CHECK(s.normalTexture == "props/loot_bag_n.png");
    CHECK(s.ormTexture == "props/loot_bag_orm.png");
    CHECK(s.emissiveTexture == "props/loot_bag_e.png");
    CHECK(s.emissiveFactor[0] >= 1.0f); // a subtle glow: a few times the lit surface at most
    CHECK(s.emissiveFactor[0] <= 6.0f);
    const std::pair<const std::string*, TextureUsage> maps[] = {{&s.texture, TextureUsage::Color},
                                                                {&s.normalTexture, TextureUsage::Normal},
                                                                {&s.ormTexture, TextureUsage::Data},
                                                                {&s.emissiveTexture, TextureUsage::Color}};
    for (const auto& [path, usage] : maps)
    {
        INFO(*path);
        const TextureData tex = requireTexture(assetDir() / compiledTexturePath(*path));
        CHECK(tex.usage == usage);
        CHECK(tex.width() == 512);
        CHECK(tex.height() == 512);
    }
    // The emissive map is mostly dark: only the ember bed and the clasp gem glow.
    const TextureData em = requireTexture(assetDir() / compiledTexturePath(s.emissiveTexture));
    std::vector<std::uint8_t> rgba;
    REQUIRE(decodeTextureMip(em, 0, rgba, err));
    std::size_t lit = 0;
    for (std::size_t i = 0; i < rgba.size(); i += 4)
        lit += (rgba[i] > 40 || rgba[i + 1] > 40) ? 1 : 0;
    const double frac = static_cast<double>(lit) / static_cast<double>(rgba.size() / 4);
    CHECK(frac > 0.002);
    CHECK(frac < 0.12);
}

TEST_CASE("loot icons match the weapon icons' format, are not blank and differ from their sources", "[content][loot]")
{
    for (const auto& [name, ref] : kIcons)
    {
        INFO(name << " (like " << ref << ")");
        CHECK(fs::exists(assetDir() / "ui" / "items" / (std::string(name) + ".png")));
        const TextureData tex = requireIcon(name);
        const TextureData src = requireIcon(ref);
        CHECK(tex.format == TextureFormat::Rgba8);
        CHECK(tex.usage == TextureUsage::Color);
        CHECK(tex.format == src.format);
        REQUIRE(tex.mips.size() == src.mips.size());
        for (std::size_t l = 0; l < tex.mips.size(); ++l)
        {
            CHECK(tex.mips[l].width == src.mips[l].width);
            CHECK(tex.mips[l].height == src.mips[l].height);
        }
        REQUIRE(tex.width() == 128);
        REQUIRE(tex.height() == 128);
        const auto& px = tex.mips[0].data;
        CHECK(px[3] == 0); // transparent corners
        CHECK(px[(127 * 128 + 127) * 4 + 3] == 0);
        std::size_t opaque = 0;
        bool premultiplied = true;
        float lumMin = 1.0f, lumMax = 0.0f;
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            const float a = static_cast<float>(px[i + 3]) / 255.0f;
            opaque += px[i + 3] > 200 ? 1 : 0;
            for (std::size_t c = 0; c < 3; ++c)
                premultiplied = premultiplied && srgbToLinear(static_cast<float>(px[i + c]) / 255.0f) <= a + 0.01f;
            if (px[i + 3] == 255)
            {
                const float lum = (0.3f * px[i] + 0.59f * px[i + 1] + 0.11f * px[i + 2]) / 255.0f;
                lumMin = std::min(lumMin, lum);
                lumMax = std::max(lumMax, lum);
            }
        }
        CHECK(premultiplied);
        CHECK(opaque > 128 * 128 / 20); // the object covers a real part of the frame
        CHECK(lumMax - lumMin > 0.25f); // shaded, not a flat silhouette
    }
}

TEST_CASE("weapon icon variants are clearly different from the icons they were made from", "[content][loot]")
{
    for (const char* name : kVariants)
    {
        const std::string variant(name);
        const std::string ref = variant == "dagger_thrall"   ? "dagger_hunter"
                                : variant == "sword_charred" ? "sword_soldier"
                                : variant == "mace_slag"     ? "mace_sun"
                                                             : "shield_oak";
        INFO(variant << " vs " << ref);
        const TextureData a = requireIcon(variant);
        const TextureData b = requireIcon(ref);
        REQUIRE(a.mips[0].data.size() == b.mips[0].data.size());
        // Mean colour difference over the pixels either icon covers (same pose, new look).
        double diff = 0.0;
        std::size_t n = 0;
        for (std::size_t i = 0; i < a.mips[0].data.size(); i += 4)
        {
            if (a.mips[0].data[i + 3] < 128 && b.mips[0].data[i + 3] < 128)
                continue;
            for (std::size_t c = 0; c < 3; ++c)
                diff += std::abs(static_cast<int>(a.mips[0].data[i + c]) - static_cast<int>(b.mips[0].data[i + c]));
            ++n;
        }
        REQUIRE(n > 0);
        CHECK(diff / (3.0 * static_cast<double>(n)) > 12.0); // > 12/255 on average
    }
}
