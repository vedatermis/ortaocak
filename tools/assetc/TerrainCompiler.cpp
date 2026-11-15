#include "tools/assetc/TerrainCompiler.h"

#include "tools/assetc/Image.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Paths.h"

#include <toml++/toml.hpp>

#include <cmath>
#include <format>
#include <initializer_list>
#include <optional>
#include <set>
#include <string_view>

namespace vo::tools::assetc {

namespace {

struct Desc
{
    std::filesystem::path heightmap, splat, mask;
    float worldSize = 0, heightMin = 0, heightMax = 0;
    const toml::array* layers = nullptr;
    const toml::array* spawns = nullptr;
    const toml::array* objects = nullptr;
};

bool checkKeys(const toml::table& t, std::initializer_list<std::string_view> allowed, std::string_view where,
               std::string& error)
{
    for (auto&& [key, value] : t)
    {
        bool ok = false;
        for (std::string_view a : allowed)
            ok = ok || key.str() == a;
        if (!ok)
        {
            error = std::format("{}: unknown key '{}'", where, key.str());
            return false;
        }
    }
    return true;
}

bool getNumber(const toml::table& t, std::string_view key, std::optional<float> fallback, float& out,
               std::string_view where, std::string& error)
{
    const toml::node* n = t.get(key);
    if (n == nullptr)
    {
        if (fallback)
        {
            out = *fallback;
            return true;
        }
        error = std::format("{}: missing '{}'", where, key);
        return false;
    }
    const auto d = n->value<double>();
    if (!d || !std::isfinite(*d))
    {
        error = std::format("{}: '{}' must be a number", where, key);
        return false;
    }
    out = static_cast<float>(*d);
    return true;
}

bool getString(const toml::table& t, std::string_view key, std::string& out, std::string_view where, std::string& error)
{
    const auto s = t[key].value<std::string>();
    if (!s || s->empty())
    {
        error = std::format("{}: '{}' must be a non-empty string", where, key);
        return false;
    }
    out = *s;
    return true;
}

bool parseDesc(const std::filesystem::path& descPath, toml::table& root, Desc& d, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!assets::readFileBytes(descPath, bytes, error))
        return false;
    const std::string where = core::pathToUtf8(descPath);
    try
    {
        root = toml::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), where);
    }
    catch (const toml::parse_error& e)
    {
        error = std::format("{}: {}", where, e.description());
        return false;
    }
    if (!checkKeys(root, {"terrain", "layer", "spawn", "object"}, where, error))
        return false;
    const toml::table* t = root["terrain"].as_table();
    if (t == nullptr)
        return error = std::format("{}: missing [terrain] table", where), false;
    if (!checkKeys(*t, {"heightmap", "world_size", "height_min", "height_max", "splat", "walk_mask"}, where, error))
        return false;
    std::string hm, sp, wm;
    if (!getString(*t, "heightmap", hm, where, error) || !getString(*t, "splat", sp, where, error) ||
        !getString(*t, "walk_mask", wm, where, error) ||
        !getNumber(*t, "world_size", std::nullopt, d.worldSize, where, error) ||
        !getNumber(*t, "height_min", std::nullopt, d.heightMin, where, error) ||
        !getNumber(*t, "height_max", std::nullopt, d.heightMax, where, error))
        return false;
    const std::filesystem::path dir = descPath.parent_path();
    d.heightmap = dir / core::pathFromUtf8(hm);
    d.splat = dir / core::pathFromUtf8(sp);
    d.mask = dir / core::pathFromUtf8(wm);
    const auto arrayOf = [&](std::string_view key, const toml::array*& out) {
        const toml::node* n = root.get(key);
        if (n == nullptr)
            return true;
        out = n->as_array();
        if (out == nullptr || !out->is_array_of_tables())
        {
            error = std::format("{}: '{}' must be an array of tables ([[{}]])", where, key, key);
            return false;
        }
        return true;
    };
    return arrayOf("layer", d.layers) && arrayOf("spawn", d.spawns) && arrayOf("object", d.objects);
}

} // namespace

bool terrainDependencies(const std::filesystem::path& desc, std::vector<std::filesystem::path>& deps,
                         std::string& error)
{
    toml::table root;
    Desc d;
    if (!parseDesc(desc, root, d, error))
        return false;
    deps = {desc, d.heightmap, d.splat, d.mask};
    return true;
}

bool compileTerrain(const std::filesystem::path& descPath, assets::TerrainData& out, std::string& error)
{
    toml::table root;
    Desc d;
    if (!parseDesc(descPath, root, d, error))
        return false;
    const std::string where = core::pathToUtf8(descPath);
    if (!(d.worldSize > 0.0f) || d.worldSize > 1e6f)
        return error = std::format("{}: world_size must be > 0", where), false;
    if (!(d.heightMax > d.heightMin))
        return error = std::format("{}: height_max must be > height_min", where), false;

    assets::TerrainData t;
    t.worldSize = d.worldSize;
    t.minHeight = d.heightMin;
    t.maxHeight = d.heightMax;

    Image16 hm;
    if (!loadImage16(d.heightmap, hm, error))
        return false;
    const auto n = static_cast<std::uint32_t>(hm.width);
    if (hm.width != hm.height || n < 3 || n > 4097 || ((n - 1) & (n - 2)) != 0)
        return error = std::format("{}: heightmap must be square with size 2^k+1 (3..4097), got {}x{}", where,
                                   hm.width, hm.height),
               false;
    t.resolution = n;
    t.heights = std::move(hm.pixels);

    Image8 splat;
    if (!loadImage8(d.splat, 4, splat, error))
        return false;
    if (splat.width != splat.height || splat.width < 1 || splat.width > 4096)
        return error = std::format("{}: splat map must be square (1..4096)", where), false;
    t.splatResolution = static_cast<std::uint32_t>(splat.width);
    t.splat = std::move(splat.pixels);

    Image8 mask;
    if (!loadImage8(d.mask, 1, mask, error))
        return false;
    if (mask.width != mask.height || mask.width < 1 || mask.width > 4096)
        return error = std::format("{}: walk mask must be square (1..4096)", where), false;
    t.maskResolution = static_cast<std::uint32_t>(mask.width);
    t.walkMask.resize(mask.pixels.size());
    for (std::size_t i = 0; i < mask.pixels.size(); ++i)
        t.walkMask[i] = mask.pixels[i] >= 128 ? 1 : 0;

    // Layers.
    if (d.layers == nullptr || d.layers->size() != 4)
        return error = std::format("{}: exactly 4 [[layer]] entries are required", where), false;
    for (const toml::node& node : *d.layers)
    {
        const toml::table& lt = *node.as_table();
        if (!checkKeys(lt, {"texture", "tile_size", "normal", "orm"}, where, error))
            return false;
        assets::TerrainLayer layer;
        if (!getString(lt, "texture", layer.texture, where, error) ||
            !getNumber(lt, "tile_size", std::nullopt, layer.tileSize, where, error))
            return false;
        if ((lt.get("normal") != nullptr && !getString(lt, "normal", layer.normalTexture, where, error)) ||
            (lt.get("orm") != nullptr && !getString(lt, "orm", layer.ormTexture, where, error)))
            return false;
        if (!assets::isValidAssetPath(layer.texture) || !(layer.tileSize > 0.0f) ||
            (!layer.normalTexture.empty() && !assets::isValidAssetPath(layer.normalTexture)) ||
            (!layer.ormTexture.empty() && !assets::isValidAssetPath(layer.ormTexture)))
            return error = std::format("{}: layer '{}' invalid (asset paths, tile_size > 0)", where, layer.texture), false;
        t.layers.push_back(std::move(layer));
    }

    // Spawns.
    std::set<std::string> names;
    if (d.spawns != nullptr)
        for (const toml::node& node : *d.spawns)
        {
            const toml::table& st = *node.as_table();
            if (!checkKeys(st, {"name", "x", "z", "yaw"}, where, error))
                return false;
            assets::SpawnPoint s;
            if (!getString(st, "name", s.name, where, error) ||
                !getNumber(st, "x", std::nullopt, s.position.x, where, error) ||
                !getNumber(st, "z", std::nullopt, s.position.z, where, error) ||
                !getNumber(st, "yaw", 0.0f, s.yaw, where, error))
                return false;
            if (!names.insert(s.name).second)
                return error = std::format("{}: duplicate spawn '{}'", where, s.name), false;
            if (!t.contains(s.position.x, s.position.z))
                return error = std::format("{}: spawn '{}' is outside the zone", where, s.name), false;
            if (!t.isWalkable(s.position.x, s.position.z))
                return error = std::format("{}: spawn '{}' is on blocked ground", where, s.name), false;
            s.position.y = t.heightAt(s.position.x, s.position.z);
            t.spawns.push_back(std::move(s));
        }
    if (t.spawns.size() > 1024)
        return error = std::format("{}: too many spawns", where), false;

    // Objects.
    if (d.objects != nullptr)
        for (const toml::node& node : *d.objects)
        {
            const toml::table& ot = *node.as_table();
            if (!checkKeys(ot, {"mesh", "x", "z", "yaw", "scale", "y_offset", "blocks", "stretch", "tint"}, where,
                           error))
                return false;
            assets::ObjectPlacement o;
            float yOffset = 0.0f;
            if (!getString(ot, "mesh", o.mesh, where, error) ||
                !getNumber(ot, "x", std::nullopt, o.position.x, where, error) ||
                !getNumber(ot, "z", std::nullopt, o.position.z, where, error) ||
                !getNumber(ot, "yaw", 0.0f, o.yaw, where, error) ||
                !getNumber(ot, "scale", 1.0f, o.scale, where, error) ||
                !getNumber(ot, "y_offset", 0.0f, yOffset, where, error))
                return false;
            if (const toml::node* b = ot.get("blocks"))
            {
                const auto v = b->value<bool>();
                if (!v)
                    return error = std::format("{}: 'blocks' must be a boolean", where), false;
                o.flags = *v ? assets::kObjectBlocks : 0u;
            }
            // stretch = [x, y, z] (per-axis factor on top of scale, 0.25 .. 4); tint = [r, g, b] (sRGB 0..255).
            const auto numbers = [&](std::string_view key, std::size_t n, float* out) {
                const toml::node* node = ot.get(key);
                if (node == nullptr)
                    return true;
                const toml::array* a = node->as_array();
                if (a == nullptr || a->size() != n)
                    return error = std::format("{}: '{}' must be an array of {} numbers", where, key, n), false;
                for (std::size_t k = 0; k < n; ++k)
                {
                    const auto d = (*a)[k].value<double>();
                    if (!d || !std::isfinite(*d))
                        return error = std::format("{}: '{}' must be an array of {} numbers", where, key, n), false;
                    out[k] = static_cast<float>(*d);
                }
                return true;
            };
            float stretch[3] = {1.0f, 1.0f, 1.0f};
            float tint[3] = {255.0f, 255.0f, 255.0f};
            if (!numbers("stretch", 3, stretch) || !numbers("tint", 3, tint))
                return false;
            for (std::size_t k = 0; k < 3; ++k)
            {
                if (!(stretch[k] >= 0.25f && stretch[k] <= 4.0f))
                    return error = std::format("{}: object '{}' stretch must be 0.25 .. 4", where, o.mesh), false;
                if (!(tint[k] >= 0.0f && tint[k] <= 255.0f) || tint[k] != std::floor(tint[k]))
                    return error = std::format("{}: object '{}' tint must be integers 0 .. 255", where, o.mesh), false;
                o.tint[k] = static_cast<std::uint8_t>(tint[k]);
            }
            o.stretch = {stretch[0], stretch[1], stretch[2]};
            if (!assets::isValidAssetPath(o.mesh))
                return error = std::format("{}: object mesh path '{}' invalid", where, o.mesh), false;
            if (!t.contains(o.position.x, o.position.z))
                return error = std::format("{}: object '{}' at ({}, {}) is outside the zone", where, o.mesh,
                                           o.position.x, o.position.z),
                       false;
            if (!(o.scale > 0.0f))
                return error = std::format("{}: object '{}' scale must be > 0", where, o.mesh), false;
            o.position.y = t.heightAt(o.position.x, o.position.z) + yOffset;
            t.objects.push_back(std::move(o));
        }
    if (t.objects.size() > 1'000'000)
        return error = std::format("{}: too many objects", where), false;

    out = std::move(t);
    return true;
}

} // namespace vo::tools::assetc
