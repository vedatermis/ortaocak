#include "tools/assetc/Info.h"

#include "shared/assets/Assets.h"
#include "shared/core/Paths.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <map>

namespace vo::tools::assetc {

namespace {

const char* groupName(assets::VariantGroup g) noexcept
{
    switch (g)
    {
    case assets::VariantGroup::Always: return "always";
    case assets::VariantGroup::Hair: return "hair";
    case assets::VariantGroup::Face: return "face";
    }
    return "?";
}

void describeMesh(const assets::MeshData& m, std::string& s)
{
    auto out = std::back_inserter(s);
    const auto l = m.layout();
    std::format_to(out, "  vertices   {} (stride {} B:{}{}{}{})\n", m.vertexCount, l.stride, " pos nrm",
                   m.hasTangents() ? " tan" : "", " uv0", m.hasColors() ? " col" : "");
    if (m.isSkinned())
        s += "               + joints/weights (skinned)\n";
    std::format_to(out, "  indices    {} ({}-bit), {} triangles\n", m.indexCount(), m.indexSize * 8, m.indexCount() / 3);
    std::format_to(out, "  bounds     min ({:.3f}, {:.3f}, {:.3f})  max ({:.3f}, {:.3f}, {:.3f})\n", m.boundsMin.x,
                   m.boundsMin.y, m.boundsMin.z, m.boundsMax.x, m.boundsMax.y, m.boundsMax.z);
    std::format_to(out, "  size       {:.3f} x {:.3f} x {:.3f} m\n", m.boundsMax.x - m.boundsMin.x,
                   m.boundsMax.y - m.boundsMin.y, m.boundsMax.z - m.boundsMin.z);
    std::format_to(out, "  sphere     centre ({:.3f}, {:.3f}, {:.3f}) radius {:.3f}\n", m.sphereCenter.x,
                   m.sphereCenter.y, m.sphereCenter.z, m.sphereRadius);
    std::format_to(out, "  submeshes  {}\n", m.submeshes.size());
    for (const auto& sm : m.submeshes)
    {
        std::format_to(out, "    {:<14} {:>6}#{:<2} {:>6} tris  colour ({:.2f} {:.2f} {:.2f} {:.2f}){}{}\n",
                       sm.name.empty() ? "(unnamed)" : sm.name, groupName(sm.variantGroup), sm.variantIndex,
                       sm.indexCount / 3, sm.baseColor[0], sm.baseColor[1], sm.baseColor[2], sm.baseColor[3],
                       sm.texture.empty() ? "" : "  tex ", sm.texture);
    }
    if (m.isSkinned())
    {
        std::format_to(out, "  bones      {}\n", m.skeleton.bones.size());
        for (std::size_t b = 0; b < m.skeleton.bones.size(); ++b)
        {
            const auto& bone = m.skeleton.bones[b];
            std::format_to(out, "    {:>2} {:<14} parent {:<14} t ({:.3f}, {:.3f}, {:.3f})\n", b, bone.name,
                           bone.parent < 0 ? std::string("-") : m.skeleton.bones[static_cast<std::size_t>(bone.parent)].name,
                           bone.bindLocal.translation.x, bone.bindLocal.translation.y, bone.bindLocal.translation.z);
        }
    }
}

void describeAnim(const assets::AnimationFile& a, std::string& s)
{
    auto out = std::back_inserter(s);
    std::format_to(out, "  clips      {}\n", a.clips.size());
    for (const auto& c : a.clips)
        std::format_to(out, "    {:<10} {:.3f} s, {} frames @ {} Hz, {}, move speed {:.2f} m/s, {} tracks{}\n", c.name,
                       c.duration, c.frameCount, c.sampleRate, c.loop ? "loop" : "once", c.moveSpeed, c.tracks.size(),
                       c.quantizedRotation ? ", snorm16 rotations" : "");
}

void describeTerrain(const assets::TerrainData& t, std::string& s)
{
    auto out = std::back_inserter(s);
    const auto [lo, hi] = std::minmax_element(t.heights.begin(), t.heights.end());
    const float hMin = t.minHeight + (static_cast<float>(*lo) / 65535.0f) * (t.maxHeight - t.minHeight);
    const float hMax = t.minHeight + (static_cast<float>(*hi) / 65535.0f) * (t.maxHeight - t.minHeight);
    std::size_t walkable = 0;
    for (auto v : t.walkMask)
        walkable += v;
    std::format_to(out, "  heightmap  {}x{} samples over {:.1f} m ({:.3f} m spacing)\n", t.resolution, t.resolution,
                   t.worldSize, t.worldSize / static_cast<float>(t.resolution - 1));
    std::format_to(out, "  heights    range [{:.2f}, {:.2f}] m, used [{:.2f}, {:.2f}] m\n", t.minHeight, t.maxHeight,
                   hMin, hMax);
    std::format_to(out, "  splat      {}x{} RGBA8\n", t.splatResolution, t.splatResolution);
    std::format_to(out, "  walk mask  {}x{} ({:.2f} m cells), {:.1f}% walkable\n", t.maskResolution, t.maskResolution,
                   t.worldSize / static_cast<float>(t.maskResolution),
                   100.0 * static_cast<double>(walkable) / static_cast<double>(t.walkMask.size()));
    for (std::size_t i = 0; i < t.layers.size(); ++i)
        std::format_to(out, "  layer {}    {} (tile {:.2f} m)\n", i, t.layers[i].texture, t.layers[i].tileSize);
    std::format_to(out, "  spawns     {}\n", t.spawns.size());
    for (const auto& sp : t.spawns)
        std::format_to(out, "    {:<14} ({:.2f}, {:.2f}, {:.2f}) yaw {:.3f}{}\n", sp.name, sp.position.x, sp.position.y,
                       sp.position.z, sp.yaw, t.isWalkable(sp.position.x, sp.position.z) ? "" : "  BLOCKED");
    std::map<std::string, std::pair<int, int>> perMesh;
    for (const auto& o : t.objects)
    {
        auto& e = perMesh[o.mesh];
        ++e.first;
        e.second += o.blocks() ? 1 : 0;
    }
    std::format_to(out, "  objects    {} ({} meshes)\n", t.objects.size(), perMesh.size());
    for (const auto& [mesh, c] : perMesh)
        std::format_to(out, "    {:<36} x{:<4} ({} blocking)\n", mesh, c.first, c.second);
}

} // namespace

bool describeFile(const std::filesystem::path& path, std::string& text, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!assets::readFileBytes(path, bytes, error))
        return false;
    text = std::format("{}\n", core::pathToUtf8(path));
    if (bytes.size() >= assets::kHeaderSize)
    {
        std::uint16_t version = 0;
        std::uint32_t size = 0, crc = 0;
        std::memcpy(&version, bytes.data() + 4, 2);
        std::memcpy(&size, bytes.data() + 8, 4);
        std::memcpy(&crc, bytes.data() + 12, 4);
        text += std::format("  header     magic {}{}{}{} version {} size {} crc 0x{:08x}\n", static_cast<char>(bytes[0]),
                            static_cast<char>(bytes[1]), static_cast<char>(bytes[2]), static_cast<char>(bytes[3]),
                            version, size, crc);
    }
    switch (assets::detectFileKind(bytes))
    {
    case assets::FileKind::Mesh:
    {
        assets::MeshData m;
        if (!assets::parseMesh(bytes, m, error))
            return false;
        text += m.isSkinned() ? "  type       skinned mesh\n" : "  type       static mesh\n";
        describeMesh(m, text);
        return true;
    }
    case assets::FileKind::Animation:
    {
        assets::AnimationFile a;
        if (!assets::parseAnimation(bytes, a, error))
            return false;
        text += "  type       animation\n";
        describeAnim(a, text);
        return true;
    }
    case assets::FileKind::Terrain:
    {
        assets::TerrainData t;
        if (!assets::parseTerrain(bytes, t, error))
            return false;
        text += "  type       terrain\n";
        describeTerrain(t, text);
        return true;
    }
    case assets::FileKind::Unknown: break;
    }
    error = std::format("{}: not a .vmesh/.vanim/.vterrain file", core::pathToUtf8(path));
    return false;
}

} // namespace vo::tools::assetc
