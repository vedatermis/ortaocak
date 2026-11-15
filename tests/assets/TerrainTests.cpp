// TerrainData queries (bilinear heightAt, isWalkable, normals) and the terrain compiler.

#include "tests/assets/AssetTestUtil.h"

#include "tools/assetc/Image.h"
#include "tools/assetc/TerrainCompiler.h"

#include "shared/assets/Terrain.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace vo::assets;
using namespace vo::tests::assets;
namespace ac = vo::tools::assetc;
using Catch::Approx;

namespace {

// 3x3 samples over 2 m (1 m spacing), heights in metres:
//   z=0: 0 1 2
//   z=1: 1 2 3
//   z=2: 2 3 4      (h = x + z), stored with range [0, 4].
TerrainData plane()
{
    TerrainData t;
    t.resolution = 3;
    t.worldSize = 2.0f;
    t.minHeight = 0.0f;
    t.maxHeight = 4.0f;
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            t.heights.push_back(t.quantizeHeight(static_cast<float>(i + j)));
    t.splatResolution = 1;
    t.splat = {255, 0, 0, 0};
    t.maskResolution = 2;
    t.walkMask = {1, 0, 1, 1}; // cell (1,0) blocked: x in [1,2), z in [0,1)
    t.layers.assign(4, {"textures/terrain/grass.png", 6.0f});
    return t;
}

} // namespace

TEST_CASE("heightAt interpolates bilinearly and clamps outside the zone", "[assets][terrain]")
{
    const TerrainData t = plane();
    CHECK(t.heightAt(0.0f, 0.0f) == Approx(0.0f).margin(1e-3));
    CHECK(t.heightAt(1.0f, 1.0f) == Approx(2.0f).margin(1e-3));
    CHECK(t.heightAt(0.5f, 0.25f) == Approx(0.75f).margin(1e-3));
    CHECK(t.heightAt(1.75f, 1.5f) == Approx(3.25f).margin(1e-3));
    CHECK(t.heightAt(2.0f, 2.0f) == Approx(4.0f).margin(1e-3));
    // Clamped to the edges.
    CHECK(t.heightAt(-5.0f, 0.5f) == Approx(0.5f).margin(1e-3));
    CHECK(t.heightAt(9.0f, 9.0f) == Approx(4.0f).margin(1e-3));
    CHECK(t.heightAt(std::nanf(""), 1.0f) == Approx(1.0f).margin(1e-3));
    // A non-planar case: one raised corner, bilinear (not triangle) interpolation.
    TerrainData b = plane();
    for (auto& h : b.heights)
        h = 0;
    b.heights[4] = 65535; // centre sample (1,1) = 4 m
    CHECK(b.heightAt(0.5f, 0.5f) == Approx(1.0f).margin(1e-3));  // 0.25 * 4
    CHECK(b.heightAt(0.5f, 0.75f) == Approx(1.5f).margin(1e-3)); // 0.375 * 4
    // Normal of the plane h = x + z: (-1, 1, -1) / sqrt(3).
    const Vec3 n = t.normalAt(1.0f, 1.0f);
    CHECK(n.x == Approx(-0.57735f).margin(1e-3));
    CHECK(n.y == Approx(0.57735f).margin(1e-3));
    CHECK(n.z == Approx(-0.57735f).margin(1e-3));
}

TEST_CASE("isWalkable uses the mask cells and rejects positions outside the zone", "[assets][terrain]")
{
    const TerrainData t = plane();
    CHECK(t.isWalkable(0.5f, 0.5f));
    CHECK_FALSE(t.isWalkable(1.5f, 0.5f));
    CHECK(t.isWalkable(1.5f, 1.5f));
    CHECK(t.isWalkable(0.0f, 0.0f));
    CHECK_FALSE(t.isWalkable(1.0f, 0.999f)); // cell (1,0)
    CHECK_FALSE(t.isWalkable(-0.01f, 0.5f));
    CHECK_FALSE(t.isWalkable(0.5f, 2.0f)); // == worldSize is outside [0, worldSize)
    CHECK_FALSE(t.isWalkable(std::nanf(""), 0.5f));
}

TEST_CASE("object placement world matrix is T * RotY(yaw) * S", "[assets][terrain]")
{
    ObjectPlacement o;
    o.position = {10, 2, 20};
    o.yaw = 1.5707963f; // +Z turns towards +X
    o.scale = 2.0f;
    const Mat4 w = o.worldMatrix();
    const Vec3 front = transformPoint(w, {0, 0, 1});
    CHECK(front.x == Approx(12.0f).margin(1e-4));
    CHECK(front.y == Approx(2.0f).margin(1e-4));
    CHECK(front.z == Approx(20.0f).margin(1e-4));
}

TEST_CASE("terrain compiler builds TerrainData from a description and PNG maps", "[assets][terrain]")
{
    TempDir dir("terrain_compile");
    std::string err;
    // 5x5 heightmap (8-bit, expanded to 16-bit), 4x4 splat, 4x4 mask with one blocked cell (2,1).
    ac::Image8 hm{5, 5, 1, {}};
    for (int j = 0; j < 5; ++j)
        for (int i = 0; i < 5; ++i)
            hm.pixels.push_back(static_cast<std::uint8_t>(i * 50));
    ac::Image8 splat{4, 4, 4, std::vector<std::uint8_t>(64, 0)};
    for (int k = 0; k < 16; ++k)
        splat.pixels[static_cast<std::size_t>(k) * 4] = 255;
    ac::Image8 mask{4, 4, 1, std::vector<std::uint8_t>(16, 255)};
    mask.pixels[1 * 4 + 2] = 0;
    REQUIRE(ac::writePng8(dir.path() / "h.png", hm, err));
    REQUIRE(ac::writePng8(dir.path() / "s.png", splat, err));
    REQUIRE(ac::writePng8(dir.path() / "m.png", mask, err));
    const std::string head = "[terrain]\nheightmap = \"h.png\"\nworld_size = 8.0\nheight_min = 0.0\nheight_max = 10.0\n"
                             "splat = \"s.png\"\nwalk_mask = \"m.png\"\n"
                             "[[layer]]\ntexture = \"t/a.png\"\ntile_size = 4.0\n"
                             "[[layer]]\ntexture = \"t/b.png\"\ntile_size = 4.0\n"
                             "[[layer]]\ntexture = \"t/c.png\"\ntile_size = 4.0\n"
                             "[[layer]]\ntexture = \"t/d.png\"\ntile_size = 4.0\n";
    writeText(dir.path() / "z.terrain.toml",
              head + "[[spawn]]\nname = \"spawn_aydor\"\nx = 1.0\nz = 1.0\nyaw = 0.5\n"
                     "[[object]]\nmesh = \"props/crate.vmesh\"\nx = 3.0\nz = 6.0\ny_offset = -0.5\nscale = 2.0\n");
    TerrainData t;
    REQUIRE(ac::compileTerrain(dir.path() / "z.terrain.toml", t, err));
    CHECK(t.resolution == 5);
    CHECK(t.worldSize == 8.0f);
    // Height along x: pixel value i*50 -> 8-bit expanded x257 -> metres = i*50/255*10.
    CHECK(t.heightAt(2.0f, 0.0f) == Approx(50.0f / 255.0f * 10.0f).margin(1e-3));
    CHECK(t.walkMask.size() == 16);
    CHECK_FALSE(t.isWalkable(5.0f, 3.0f)); // cell (2,1) with 2 m cells
    CHECK(t.isWalkable(1.0f, 1.0f));
    REQUIRE(t.spawns.size() == 1);
    CHECK(t.spawns[0].position.y == Approx(t.heightAt(1.0f, 1.0f)));
    REQUIRE(t.objects.size() == 1);
    CHECK(t.objects[0].position.y == Approx(t.heightAt(3.0f, 6.0f) - 0.5f));
    CHECK(t.objects[0].scale == 2.0f);
    CHECK(t.objects[0].blocks());

    // A spawn on a blocked cell or outside the zone is an error.
    writeText(dir.path() / "bad.terrain.toml", head + "[[spawn]]\nname = \"s\"\nx = 5.0\nz = 3.0\n");
    CHECK_FALSE(ac::compileTerrain(dir.path() / "bad.terrain.toml", t, err));
    writeText(dir.path() / "bad2.terrain.toml", head + "[[spawn]]\nname = \"s\"\nx = 9.0\nz = 3.0\n");
    CHECK_FALSE(ac::compileTerrain(dir.path() / "bad2.terrain.toml", t, err));
    // Escaping mesh path, unknown key, wrong layer count.
    writeText(dir.path() / "bad3.terrain.toml", head + "[[object]]\nmesh = \"../x.vmesh\"\nx = 1.0\nz = 1.0\n");
    CHECK_FALSE(ac::compileTerrain(dir.path() / "bad3.terrain.toml", t, err));
    writeText(dir.path() / "bad4.terrain.toml", head + "[[object]]\nmesh = \"a.vmesh\"\nx = 1.0\nz = 1.0\nrot = 1\n");
    CHECK_FALSE(ac::compileTerrain(dir.path() / "bad4.terrain.toml", t, err));
    writeText(dir.path() / "bad5.terrain.toml",
              "[terrain]\nheightmap = \"h.png\"\nworld_size = 8.0\nheight_min = 0.0\nheight_max = 10.0\n"
              "splat = \"s.png\"\nwalk_mask = \"m.png\"\n");
    CHECK_FALSE(ac::compileTerrain(dir.path() / "bad5.terrain.toml", t, err));
}
