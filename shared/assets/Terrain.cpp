#include "shared/assets/Terrain.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <set>

namespace vo::assets {

namespace {

constexpr std::size_t kInfoEnd = 88;
constexpr std::uint32_t kMaxResolution = 4097;
constexpr std::uint32_t kMaxMap = 4096;
constexpr std::uint32_t kLayerCount = 4;
constexpr std::uint32_t kMaxSpawns = 1024;
constexpr std::uint32_t kMaxObjects = 1'000'000;
constexpr std::uint32_t kMaxStringTable = 16u << 20;
constexpr std::uint32_t kLayerSizeV1 = 12;
constexpr std::uint32_t kLayerSizeV2 = 28;
constexpr std::uint32_t kSpawnSize = 24;
constexpr std::uint32_t kObjectSizeV1 = 32;
constexpr std::uint32_t kObjectSizeV2 = 48;
constexpr float kMaxStretch = 4.0f;

bool finite(float f) noexcept { return std::isfinite(f); }
bool isPow2Plus1(std::uint32_t n) noexcept
{
    const std::uint32_t m = n - 1;
    return n >= 3 && (m & (m - 1)) == 0;
}

} // namespace

Mat4 ObjectPlacement::worldMatrix() const noexcept
{
    return composeTrs(position, quatFromAxisAngle({0, 1, 0}, yaw),
                      {scale * stretch.x, scale * stretch.y, scale * stretch.z});
}

float ObjectPlacement::maxScale() const noexcept
{
    return scale * std::max({stretch.x, stretch.y, stretch.z});
}

float TerrainData::sampleHeight(int i, int j) const noexcept
{
    if (resolution == 0 || heights.size() != static_cast<std::size_t>(resolution) * resolution)
        return 0.0f;
    const int n = static_cast<int>(resolution);
    i = std::clamp(i, 0, n - 1);
    j = std::clamp(j, 0, n - 1);
    const std::uint16_t h = heights[static_cast<std::size_t>(j) * resolution + static_cast<std::size_t>(i)];
    return minHeight + (static_cast<float>(h) / 65535.0f) * (maxHeight - minHeight);
}

float TerrainData::heightAt(float x, float z) const noexcept
{
    if (resolution < 2 || !(worldSize > 0.0f))
        return 0.0f;
    const float cell = worldSize / static_cast<float>(resolution - 1);
    if (!finite(x))
        x = 0.0f;
    if (!finite(z))
        z = 0.0f;
    const float fx = std::clamp(x, 0.0f, worldSize) / cell;
    const float fz = std::clamp(z, 0.0f, worldSize) / cell;
    const int last = static_cast<int>(resolution) - 2;
    const int i = std::clamp(static_cast<int>(std::floor(fx)), 0, last);
    const int j = std::clamp(static_cast<int>(std::floor(fz)), 0, last);
    const float tx = std::clamp(fx - static_cast<float>(i), 0.0f, 1.0f);
    const float tz = std::clamp(fz - static_cast<float>(j), 0.0f, 1.0f);
    const float h00 = sampleHeight(i, j), h10 = sampleHeight(i + 1, j);
    const float h01 = sampleHeight(i, j + 1), h11 = sampleHeight(i + 1, j + 1);
    const float a = h00 + (h10 - h00) * tx;
    const float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

Vec3 TerrainData::normalAt(float x, float z) const noexcept
{
    const float d = resolution > 1 ? worldSize / static_cast<float>(resolution - 1) : 1.0f;
    const float hl = heightAt(x - d, z), hr = heightAt(x + d, z);
    const float hb = heightAt(x, z - d), hf = heightAt(x, z + d);
    return normalize(Vec3{hl - hr, 2.0f * d, hb - hf});
}

bool TerrainData::contains(float x, float z) const noexcept
{
    return x >= 0.0f && z >= 0.0f && x < worldSize && z < worldSize;
}

bool TerrainData::isWalkable(float x, float z) const noexcept
{
    if (!contains(x, z) || maskResolution == 0 ||
        walkMask.size() != static_cast<std::size_t>(maskResolution) * maskResolution)
        return false;
    const float cell = worldSize / static_cast<float>(maskResolution);
    const auto last = static_cast<int>(maskResolution) - 1;
    const int i = std::clamp(static_cast<int>(x / cell), 0, last);
    const int j = std::clamp(static_cast<int>(z / cell), 0, last);
    return walkMask[static_cast<std::size_t>(j) * maskResolution + static_cast<std::size_t>(i)] != 0;
}

const SpawnPoint* TerrainData::findSpawn(std::string_view name) const noexcept
{
    for (const SpawnPoint& s : spawns)
        if (s.name == name)
            return &s;
    return nullptr;
}

std::uint16_t TerrainData::quantizeHeight(float metres) const noexcept
{
    const float range = maxHeight - minHeight;
    if (!(range > 0.0f) || !finite(metres))
        return 0;
    const float t = std::clamp((metres - minHeight) / range, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(t * 65535.0f));
}

bool parseTerrain(std::span<const std::byte> file, TerrainData& out, std::string& error)
{
    std::uint16_t version = 0;
    if (!detail::checkHeader(file, kMagicTerrain, 1, kTerrainVersion, version, error))
        return false;
    const std::uint32_t kLayerSize = version >= 2 ? kLayerSizeV2 : kLayerSizeV1;
    const std::uint32_t kObjectSize = version >= 2 ? kObjectSizeV2 : kObjectSizeV1;
    if (file.size() < kInfoEnd)
        return error = "file too small for TerrainInfo", false;
    detail::ByteReader r(file);
    const auto resolution = r.read<std::uint32_t>(16);
    const auto worldSize = r.read<float>(20);
    const auto minHeight = r.read<float>(24);
    const auto maxHeight = r.read<float>(28);
    const auto splatRes = r.read<std::uint32_t>(32);
    const auto maskRes = r.read<std::uint32_t>(36);
    const auto layerCount = r.read<std::uint32_t>(40);
    const auto spawnCount = r.read<std::uint32_t>(44);
    const auto objectCount = r.read<std::uint32_t>(48);
    const auto heightOffset = r.read<std::uint32_t>(52);
    const auto splatOffset = r.read<std::uint32_t>(56);
    const auto maskOffset = r.read<std::uint32_t>(60);
    const auto layerOffset = r.read<std::uint32_t>(64);
    const auto spawnOffset = r.read<std::uint32_t>(68);
    const auto objectOffset = r.read<std::uint32_t>(72);
    const auto stringOffset = r.read<std::uint32_t>(76);
    const auto stringSize = r.read<std::uint32_t>(80);
    const auto reserved = r.read<std::uint32_t>(84);
    if (resolution > kMaxResolution || !isPow2Plus1(resolution))
        return error = std::format("resolution {} invalid (must be 2^k+1, 3..4097)", resolution), false;
    if (!finite(worldSize) || !(worldSize > 0.0f) || worldSize > 1e6f)
        return error = "worldSize invalid", false;
    if (!finite(minHeight) || !finite(maxHeight) || !(maxHeight > minHeight))
        return error = "height range invalid", false;
    if (splatRes < 1 || splatRes > kMaxMap || maskRes < 1 || maskRes > kMaxMap)
        return error = "splat/mask resolution out of range", false;
    if (layerCount != kLayerCount)
        return error = std::format("layerCount {} (must be 4)", layerCount), false;
    if (spawnCount > kMaxSpawns || objectCount > kMaxObjects)
        return error = "spawn/object count out of range", false;
    if (stringSize > kMaxStringTable || reserved != 0)
        return error = "bad string table size or reserved field", false;
    if (!r.hasRange(heightOffset, static_cast<std::uint64_t>(resolution) * resolution, 2) ||
        !r.hasRange(splatOffset, static_cast<std::uint64_t>(splatRes) * splatRes, 4) ||
        !r.hasRange(maskOffset, static_cast<std::uint64_t>(maskRes) * maskRes, 1) ||
        !r.hasRange(layerOffset, layerCount, kLayerSize) || !r.hasRange(spawnOffset, spawnCount, kSpawnSize) ||
        !r.hasRange(objectOffset, objectCount, kObjectSize) || !r.hasRange(stringOffset, stringSize, 1))
        return error = "section outside the file", false;
    const auto strings = r.bytes(stringOffset, stringSize);

    out.resolution = resolution;
    out.worldSize = worldSize;
    out.minHeight = minHeight;
    out.maxHeight = maxHeight;
    out.splatResolution = splatRes;
    out.maskResolution = maskRes;
    {
        const std::size_t n = static_cast<std::size_t>(resolution) * resolution;
        out.heights.resize(n);
        const auto src = r.bytes(heightOffset, n * 2);
        std::memcpy(out.heights.data(), src.data(), src.size());
        const std::size_t s = static_cast<std::size_t>(splatRes) * splatRes * 4;
        out.splat.resize(s);
        const auto ssrc = r.bytes(splatOffset, s);
        std::memcpy(out.splat.data(), ssrc.data(), ssrc.size());
        const std::size_t m = static_cast<std::size_t>(maskRes) * maskRes;
        out.walkMask.resize(m);
        const auto msrc = r.bytes(maskOffset, m);
        std::memcpy(out.walkMask.data(), msrc.data(), msrc.size());
        for (const std::uint8_t v : out.walkMask)
            if (v > 1)
                return error = "walk mask values must be 0 or 1", false;
    }

    out.layers.assign(layerCount, {});
    for (std::uint32_t l = 0; l < layerCount; ++l)
    {
        const std::uint64_t b = layerOffset + static_cast<std::uint64_t>(l) * kLayerSize;
        TerrainLayer& layer = out.layers[l];
        if (!detail::readStringRef(r, b, strings, layer.texture) || !isValidAssetPath(layer.texture))
            return error = std::format("layer {}: invalid texture path", l), false;
        if (version >= 2)
        {
            if (!detail::readStringRef(r, b + 8, strings, layer.normalTexture) ||
                !detail::readStringRef(r, b + 16, strings, layer.ormTexture) ||
                (!layer.normalTexture.empty() && !isValidAssetPath(layer.normalTexture)) ||
                (!layer.ormTexture.empty() && !isValidAssetPath(layer.ormTexture)))
                return error = std::format("layer {}: invalid normal/ORM texture path", l), false;
            layer.tileSize = r.read<float>(b + 24);
        }
        else
        {
            layer.tileSize = r.read<float>(b + 8);
        }
        if (!finite(layer.tileSize) || !(layer.tileSize > 0.0f))
            return error = std::format("layer {}: tileSize invalid", l), false;
    }

    out.spawns.assign(spawnCount, {});
    std::set<std::string, std::less<>> spawnNames;
    for (std::uint32_t s = 0; s < spawnCount; ++s)
    {
        const std::uint64_t b = spawnOffset + static_cast<std::uint64_t>(s) * kSpawnSize;
        SpawnPoint& sp = out.spawns[s];
        if (!detail::readStringRef(r, b, strings, sp.name) || sp.name.empty())
            return error = std::format("spawn {}: bad name", s), false;
        if (!spawnNames.insert(sp.name).second)
            return error = std::format("duplicate spawn '{}'", sp.name), false;
        sp.position = {r.read<float>(b + 8), r.read<float>(b + 12), r.read<float>(b + 16)};
        sp.yaw = r.read<float>(b + 20);
        if (!finite(sp.position.x) || !finite(sp.position.y) || !finite(sp.position.z) || !finite(sp.yaw))
            return error = std::format("spawn '{}': non-finite values", sp.name), false;
    }

    out.objects.assign(objectCount, {});
    for (std::uint32_t o = 0; o < objectCount; ++o)
    {
        const std::uint64_t b = objectOffset + static_cast<std::uint64_t>(o) * kObjectSize;
        ObjectPlacement& obj = out.objects[o];
        if (!detail::readStringRef(r, b, strings, obj.mesh) || !isValidAssetPath(obj.mesh))
            return error = std::format("object {}: invalid mesh path", o), false;
        obj.position = {r.read<float>(b + 8), r.read<float>(b + 12), r.read<float>(b + 16)};
        obj.yaw = r.read<float>(b + 20);
        obj.scale = r.read<float>(b + 24);
        if (version >= 2)
        {
            obj.stretch = {r.read<float>(b + 28), r.read<float>(b + 32), r.read<float>(b + 36)};
            for (std::size_t k = 0; k < 4; ++k)
                obj.tint[k] = r.read<std::uint8_t>(b + 40 + k);
            obj.flags = r.read<std::uint32_t>(b + 44);
        }
        else
        {
            obj.flags = r.read<std::uint32_t>(b + 28);
        }
        const auto stretchOk = [](float s) { return finite(s) && s > 0.0f && s <= kMaxStretch; };
        if (!finite(obj.position.x) || !finite(obj.position.y) || !finite(obj.position.z) || !finite(obj.yaw) ||
            !finite(obj.scale) || !(obj.scale > 0.0f) || !stretchOk(obj.stretch.x) || !stretchOk(obj.stretch.y) ||
            !stretchOk(obj.stretch.z))
            return error = std::format("object {}: invalid transform", o), false;
        if ((obj.flags & ~static_cast<std::uint32_t>(kObjectBlocks)) != 0)
            return error = std::format("object {}: unknown flags", o), false;
    }
    if (!r.ok)
        return error = "truncated data", false;
    return true;
}

bool loadTerrain(const std::filesystem::path& path, TerrainData& out, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!readFileBytes(path, bytes, error))
        return false;
    if (!parseTerrain(bytes, out, error))
    {
        error = std::format("{}: {}", core::pathToUtf8(path), error);
        return false;
    }
    return true;
}

bool serializeTerrain(const TerrainData& t, std::vector<std::byte>& out, std::string& error)
{
    if (t.heights.size() != static_cast<std::size_t>(t.resolution) * t.resolution)
        return error = "heights size does not match resolution", false;
    if (t.splat.size() != static_cast<std::size_t>(t.splatResolution) * t.splatResolution * 4)
        return error = "splat size does not match splatResolution", false;
    if (t.walkMask.size() != static_cast<std::size_t>(t.maskResolution) * t.maskResolution)
        return error = "walk mask size does not match maskResolution", false;
    if (t.spawns.size() > kMaxSpawns || t.objects.size() > kMaxObjects || t.layers.size() != kLayerCount)
        return error = "layer/spawn/object count out of range", false;

    // Version 2 only when its data is used, so v1 terrains stay byte-identical.
    bool v2 = false;
    for (const TerrainLayer& l : t.layers)
        v2 = v2 || !l.normalTexture.empty() || !l.ormTexture.empty();
    for (const ObjectPlacement& o : t.objects)
        v2 = v2 || o.stretch.x != 1.0f || o.stretch.y != 1.0f || o.stretch.z != 1.0f ||
             o.tint != std::array<std::uint8_t, 4>{255, 255, 255, 255};
    detail::ByteWriter w;
    detail::StringTableBuilder strings;
    w.beginFile(kMagicTerrain, v2 ? kTerrainVersion : std::uint16_t{1});
    w.putZeros(kInfoEnd - w.size());
    w.patch(16, t.resolution);
    w.patch(20, t.worldSize);
    w.patch(24, t.minHeight);
    w.patch(28, t.maxHeight);
    w.patch(32, t.splatResolution);
    w.patch(36, t.maskResolution);
    w.patch(40, static_cast<std::uint32_t>(t.layers.size()));
    w.patch(44, static_cast<std::uint32_t>(t.spawns.size()));
    w.patch(48, static_cast<std::uint32_t>(t.objects.size()));

    w.patch(52, static_cast<std::uint32_t>(w.size()));
    w.putBytes(std::as_bytes(std::span(t.heights)));
    w.align4();
    w.patch(56, static_cast<std::uint32_t>(w.size()));
    w.putBytes(std::as_bytes(std::span(t.splat)));
    w.align4();
    w.patch(60, static_cast<std::uint32_t>(w.size()));
    w.putBytes(std::as_bytes(std::span(t.walkMask)));
    w.align4();
    w.patch(64, static_cast<std::uint32_t>(w.size()));
    for (const TerrainLayer& l : t.layers)
    {
        const auto ref = strings.add(l.texture);
        w.put(ref.offset);
        w.put(ref.length);
        if (v2)
        {
            const auto n = strings.add(l.normalTexture);
            w.put(n.offset);
            w.put(n.length);
            const auto m = strings.add(l.ormTexture);
            w.put(m.offset);
            w.put(m.length);
        }
        w.put(l.tileSize);
    }
    w.patch(68, static_cast<std::uint32_t>(w.size()));
    for (const SpawnPoint& s : t.spawns)
    {
        const auto ref = strings.add(s.name);
        w.put(ref.offset);
        w.put(ref.length);
        w.put(s.position);
        w.put(s.yaw);
    }
    w.patch(72, static_cast<std::uint32_t>(w.size()));
    for (const ObjectPlacement& o : t.objects)
    {
        const auto ref = strings.add(o.mesh);
        w.put(ref.offset);
        w.put(ref.length);
        w.put(o.position);
        w.put(o.yaw);
        w.put(o.scale);
        if (v2)
        {
            w.put(o.stretch);
            for (std::uint8_t c : o.tint)
                w.put(c);
        }
        w.put(o.flags);
    }
    w.patch(76, static_cast<std::uint32_t>(w.size()));
    w.patch(80, static_cast<std::uint32_t>(strings.bytes().size()));
    w.putBytes(strings.bytes());
    w.align4();
    if (w.size() > kMaxFileSize)
        return error = "terrain too large", false;
    out = w.finishFile();

    TerrainData check;
    std::string why;
    if (!parseTerrain(out, check, why))
    {
        error = "serialised terrain is invalid: " + why;
        out.clear();
        return false;
    }
    return true;
}

} // namespace vo::assets
