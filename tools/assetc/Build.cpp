#include "tools/assetc/Build.h"

#include "tools/assetc/GltfImport.h"
#include "tools/assetc/TerrainCompiler.h"

#include "shared/assets/Assets.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace vo::tools::assetc {

namespace fs = std::filesystem;

namespace {

enum class ItemKind
{
    Copy,
    UiIcon, // copied like Copy, plus a premultiplied RGBA8 .vtex (ui/items/*.png, ui/skills/*.png)
    Gltf,
    Terrain,
};

struct Item
{
    ItemKind kind = ItemKind::Copy;
    fs::path source; // absolute
    fs::path rel;    // relative to the source root
};

bool endsWith(std::string_view s, std::string_view suffix) noexcept
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return s;
}

bool isTerrainDesc(const fs::path& p) { return endsWith(lower(core::pathToUtf8(p.filename())), ".terrain.toml"); }

bool underPreviews(const fs::path& rel)
{
    for (const fs::path& part : rel.parent_path())
        if (lower(core::pathToUtf8(part)) == "previews")
            return true;
    return false;
}

// Inventory, skill and status icons and the skill bar / tree pieces: PNGs directly in ui/items/,
// ui/skills/, ui/status/, ui/skillbar/ or ui/hud/ (ASSET_FORMATS.md §5.1).
bool isUiIcon(const fs::path& rel)
{
    std::string dir = lower(core::pathToUtf8(rel.parent_path()));
    std::replace(dir.begin(), dir.end(), '\\', '/');
    return dir == "ui/items" || dir == "ui/skills" || dir == "ui/status" || dir == "ui/skillbar" ||
           dir == "ui/hud";
}

bool collect(const fs::path& root, std::vector<Item>& items, std::string& error)
{
    std::error_code ec;
    std::vector<fs::path> files;
    std::vector<fs::path> terrainDirs;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        files.push_back(it->path());
        if (isTerrainDesc(it->path()))
            terrainDirs.push_back(it->path().parent_path());
    }
    if (ec)
        return error = std::format("cannot scan '{}': {}", core::pathToUtf8(root), ec.message()), false;
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files)
    {
        const fs::path rel = f.lexically_relative(root);
        const std::string ext = lower(core::pathToUtf8(f.extension()));
        Item item{ItemKind::Copy, f, rel};
        if (isTerrainDesc(f))
            item.kind = ItemKind::Terrain;
        else if (ext == ".glb" || ext == ".gltf")
            item.kind = ItemKind::Gltf;
        else if (ext == ".png")
        {
            if (underPreviews(rel) ||
                std::find(terrainDirs.begin(), terrainDirs.end(), f.parent_path()) != terrainDirs.end())
                continue;
            if (isUiIcon(rel))
                item.kind = ItemKind::UiIcon;
        }
        else
            continue;
        items.push_back(std::move(item));
    }
    // Terrains last (they reference converted meshes); otherwise keep the sorted order.
    std::stable_partition(items.begin(), items.end(), [](const Item& i) { return i.kind != ItemKind::Terrain; });
    return true;
}

fs::file_time_type mtime(const fs::path& p)
{
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? fs::file_time_type::max() : t;
}

// Stamp: <out>/.assetc/<rel>.stamp listing the outputs (relative to <out>), one per line.
fs::path stampPath(const fs::path& outRoot, const fs::path& rel)
{
    fs::path p = outRoot / ".assetc" / rel;
    p += ".stamp";
    return p;
}

bool upToDate(const fs::path& outRoot, const Item& item, const std::vector<fs::path>& inputs,
              const BuildOptions& options)
{
    if (options.force)
        return false;
    const fs::path stamp = stampPath(outRoot, item.rel);
    std::error_code ec;
    if (!fs::exists(stamp, ec))
        return false;
    const auto stampTime = mtime(stamp);
    if (stampTime < options.toolTime)
        return false;
    for (const fs::path& in : inputs)
        if (!fs::exists(in, ec) || mtime(in) > stampTime)
            return false;
    std::ifstream f(stamp);
    std::string line;
    bool any = false;
    constexpr std::string_view kInput = "input:";
    while (std::getline(f, line))
    {
        if (line.empty())
            continue;
        if (line.starts_with(kInput))
        {
            // A texture source the conversion read (discovered while converting).
            const fs::path in = core::pathFromUtf8(line.substr(kInput.size()));
            if (!fs::exists(in, ec) || mtime(in) > stampTime)
                return false;
            continue;
        }
        any = true;
        if (!fs::exists(outRoot / core::pathFromUtf8(line), ec))
            return false;
    }
    return any;
}

// Stamp lines: output asset paths, then "input:<absolute path>" for extra inputs.
bool writeStamp(const fs::path& outRoot, const Item& item, const std::vector<std::string>& outputs,
                const std::vector<fs::path>& extraInputs, std::string& error)
{
    std::string text;
    for (const std::string& o : outputs)
        text += o + "\n";
    for (const fs::path& in : extraInputs)
        text += "input:" + core::pathToUtf8(in) + "\n";
    return assets::writeFileBytes(stampPath(outRoot, item.rel), std::as_bytes(std::span(text)), error);
}

std::string assetPathOf(const fs::path& rel)
{
    std::string s = core::pathToUtf8(rel);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

bool convertGltf(const fs::path& srcRoot, const fs::path& outRoot, const Item& item, const BuildOptions& options,
                 std::vector<std::string>& outputs, std::vector<fs::path>& extraInputs, std::string& error)
{
    assets::MeshData mesh;
    MeshImportOptions mo;
    mo.assetRoot = srcRoot;
    mo.generatedDir = assetPathOf(item.rel.parent_path());
    mo.generatedStem = core::pathToUtf8(item.rel.stem());
    mo.assumeOrmOcclusion = options.assumeOrmOcclusion;
    std::vector<TextureRequest> textures;
    if (!importMesh(item.source, mo, mesh, error, &extraInputs, &textures))
        return false;
    std::vector<std::byte> bytes;
    if (!assets::serializeMesh(mesh, bytes, error))
        return false;
    fs::path meshRel = item.rel;
    meshRel.replace_extension(".vmesh");
    if (!assets::writeFileBytes(outRoot / meshRel, bytes, error))
        return false;
    outputs.push_back(assetPathOf(meshRel));
    // Images assetc writes must not collide with source PNGs (those are copied verbatim).
    std::error_code ec;
    for (const TextureRequest& t : textures)
        if (t.writePng && fs::exists(srcRoot / core::pathFromUtf8(t.assetPath), ec))
            return error = std::format("generated texture '{}' would overwrite the source image of the same name",
                                       t.assetPath),
                   false;
    if (!writeMeshTextures(textures, outRoot, options.textures, &outputs, error))
        return false;

    AnimImportOptions ao;
    ao.quantizeRotations = options.quantizeRotations;
    if (!readAnimSidecar(animSidecarPath(item.source), ao.clips, error))
        return false;
    std::vector<assets::AnimationClip> clips;
    if (!importAnimations(item.source, ao, clips, error))
        return false;
    fs::path animDir = item.rel;
    animDir.replace_extension();
    for (assets::AnimationClip& clip : clips)
    {
        assets::AnimationFile file;
        const std::string name = clip.name;
        file.clips.push_back(std::move(clip));
        if (!assets::serializeAnimation(file, bytes, error))
            return false;
        const fs::path rel = animDir / core::pathFromUtf8(name + ".vanim");
        if (!assets::writeFileBytes(outRoot / rel, bytes, error))
            return false;
        outputs.push_back(assetPathOf(rel));
    }
    return true;
}

// ui/items/<icon>.png and ui/skills/<id>.png: the PNG is copied verbatim (as every PNG) and
// `<name>.vtex` is written next to it: RGBA8, colour usage (sRGB view), alpha premultiplied in linear light, full mip chain.
bool convertUiIcon(const fs::path& outRoot, const Item& item, const BuildOptions& options,
                   std::vector<std::string>& outputs, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!assets::readFileBytes(item.source, bytes, error) || !assets::writeFileBytes(outRoot / item.rel, bytes, error))
        return false;
    outputs.push_back(assetPathOf(item.rel));
    TextureRequest req;
    req.assetPath = assetPathOf(item.rel);
    req.usage = assets::TextureUsage::Color;
    req.image.file = item.source;
    req.premultiplyAlpha = true;
    req.uncompressed = true;
    return writeMeshTextures({req}, outRoot, options.textures, &outputs, error);
}

bool convertTerrain(const fs::path& srcRoot, const fs::path& outRoot, const Item& item, const BuildOptions& options,
                    std::vector<std::string>& outputs, std::vector<fs::path>& extraInputs, std::string& error)
{
    assets::TerrainData terrain;
    if (!compileTerrain(item.source, terrain, error))
        return false;
    // Every referenced asset must exist in the output tree. Layer maps also get their compiled
    // .vtex (base colour BC1, normal BC5, ORM BC1, full mips), built from the source PNG.
    std::error_code ec;
    std::vector<TextureRequest> layerTextures;
    for (const assets::TerrainLayer& l : terrain.layers)
    {
        const std::pair<const std::string*, assets::TextureUsage> maps[] = {
            {&l.texture, assets::TextureUsage::Color},
            {&l.normalTexture, assets::TextureUsage::Normal},
            {&l.ormTexture, assets::TextureUsage::Data}};
        for (const auto& [path, usage] : maps)
        {
            if (path->empty())
                continue;
            if (!fs::exists(outRoot / core::pathFromUtf8(*path), ec))
                return error = std::format("layer texture '{}' does not exist in the output", *path), false;
            const fs::path src = srcRoot / core::pathFromUtf8(*path);
            if (!fs::exists(src, ec))
                continue; // not from this source tree (no .vtex; the client decodes the PNG)
            TextureRequest req;
            req.assetPath = *path;
            req.usage = usage;
            req.image.file = src;
            req.orm = TextureRequest::Orm::AsIs;
            layerTextures.push_back(std::move(req));
            extraInputs.push_back(src);
        }
    }
    if (!writeMeshTextures(layerTextures, outRoot, options.textures, &outputs, error))
        return false;
    std::vector<std::string> checked;
    for (const assets::ObjectPlacement& o : terrain.objects)
    {
        if (std::find(checked.begin(), checked.end(), o.mesh) != checked.end())
            continue;
        if (!endsWith(o.mesh, ".vmesh") || !fs::exists(outRoot / core::pathFromUtf8(o.mesh), ec))
            return error = std::format("object mesh '{}' does not exist in the output", o.mesh), false;
        checked.push_back(o.mesh);
    }
    std::vector<std::byte> bytes;
    if (!assets::serializeTerrain(terrain, bytes, error))
        return false;
    std::string name = core::pathToUtf8(item.rel.filename());
    name.resize(name.size() - std::string_view(".terrain.toml").size());
    const fs::path rel = item.rel.parent_path() / core::pathFromUtf8(name + ".vterrain");
    if (!assets::writeFileBytes(outRoot / rel, bytes, error))
        return false;
    outputs.push_back(assetPathOf(rel));
    return true;
}

} // namespace

bool buildAssets(const fs::path& sourceRoot, const fs::path& outputRoot, const BuildOptions& options,
                 BuildStats& stats)
{
    std::error_code ec;
    if (!fs::is_directory(sourceRoot, ec))
    {
        VO_LOG_ERROR("source root '{}' is not a directory", core::pathToUtf8(sourceRoot));
        ++stats.failed;
        return false;
    }
    const fs::path srcRoot = fs::absolute(sourceRoot, ec).lexically_normal();
    const fs::path outRoot = fs::absolute(outputRoot, ec).lexically_normal();
    std::vector<Item> items;
    std::string error;
    if (!collect(srcRoot, items, error))
    {
        VO_LOG_ERROR("{}", error);
        ++stats.failed;
        return false;
    }

    for (const Item& item : items)
    {
        std::vector<fs::path> inputs{item.source};
        if (item.kind == ItemKind::Gltf)
        {
            const fs::path sidecar = animSidecarPath(item.source);
            if (fs::exists(sidecar, ec))
                inputs.push_back(sidecar);
        }
        else if (item.kind == ItemKind::Terrain)
        {
            std::vector<fs::path> deps;
            if (!terrainDependencies(item.source, deps, error))
            {
                VO_LOG_ERROR("FAILED {}: {}", assetPathOf(item.rel), error);
                ++stats.failed;
                continue;
            }
            inputs = deps;
        }
        if (upToDate(outRoot, item, inputs, options))
        {
            ++stats.skipped;
            continue;
        }

        std::vector<std::string> outputs;
        std::vector<fs::path> extraInputs;
        bool ok = false;
        error.clear();
        switch (item.kind)
        {
        case ItemKind::Copy:
        {
            std::vector<std::byte> bytes;
            ok = assets::readFileBytes(item.source, bytes, error) &&
                 assets::writeFileBytes(outRoot / item.rel, bytes, error);
            outputs.push_back(assetPathOf(item.rel));
            break;
        }
        case ItemKind::UiIcon: ok = convertUiIcon(outRoot, item, options, outputs, error); break;
        case ItemKind::Gltf: ok = convertGltf(srcRoot, outRoot, item, options, outputs, extraInputs, error); break;
        case ItemKind::Terrain:
            ok = convertTerrain(srcRoot, outRoot, item, options, outputs, extraInputs, error);
            break;
        }
        if (ok)
            ok = writeStamp(outRoot, item, outputs, extraInputs, error);
        if (!ok)
        {
            VO_LOG_ERROR("FAILED {}: {}", assetPathOf(item.rel), error);
            std::error_code rmEc;
            fs::remove(stampPath(outRoot, item.rel), rmEc);
            ++stats.failed;
            continue;
        }
        VO_LOG_INFO("{} {} -> {}", item.kind == ItemKind::Copy ? "copied   " : "converted", assetPathOf(item.rel),
                    outputs.size() == 1 ? outputs.front() : std::format("{} files", outputs.size()));
        if (item.kind == ItemKind::Copy)
            ++stats.copied;
        else
            ++stats.converted;
    }
    return stats.failed == 0;
}

} // namespace vo::tools::assetc
