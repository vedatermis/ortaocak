#include "shared/assets/Mesh.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <set>

namespace vo::assets {

namespace {

constexpr std::size_t kInfoEnd = 120;
constexpr std::uint32_t kMaxVertices = 16'777'216;
constexpr std::uint32_t kMaxIndices = 50'331'648;
constexpr std::uint32_t kMaxSubmeshes = 1024;
constexpr std::uint32_t kMaxBones = 255;
constexpr std::uint32_t kMaxStringTable = 16u << 20;
constexpr std::uint32_t kSubmeshSize = 44;
constexpr std::uint32_t kBoneSize = 120;
constexpr std::uint32_t kKnownFormatBits = kVertexNormal | kVertexTangent | kVertexColor | kVertexSkin;

bool finite(float f) noexcept { return std::isfinite(f); }
bool finite(Vec3 v) noexcept { return finite(v.x) && finite(v.y) && finite(v.z); }

Vec3 readVec3(detail::ByteReader& r, std::uint64_t off)
{
    return {r.read<float>(off), r.read<float>(off + 4), r.read<float>(off + 8)};
}

template <class T>
T loadAt(const std::vector<std::uint8_t>& data, std::size_t off) noexcept
{
    T v{};
    std::memcpy(&v, data.data() + off, sizeof(T));
    return v;
}

} // namespace

VertexLayout VertexLayout::fromFormat(std::uint32_t vertexFormat) noexcept
{
    VertexLayout l;
    std::uint32_t off = 12;
    l.normal = off;
    off += 12;
    if (vertexFormat & kVertexTangent)
    {
        l.tangent = off;
        off += 16;
    }
    l.uv0 = off;
    off += 8;
    if (vertexFormat & kVertexColor)
    {
        l.color = off;
        off += 4;
    }
    if (vertexFormat & kVertexSkin)
    {
        l.joints = off;
        l.weights = off + 4;
        off += 8;
    }
    l.stride = off;
    return l;
}

int Skeleton::findBone(std::string_view name) const noexcept
{
    for (std::size_t i = 0; i < bones.size(); ++i)
        if (bones[i].name == name)
            return static_cast<int>(i);
    return -1;
}

std::uint32_t MeshData::index(std::uint32_t i) const noexcept
{
    if (indexSize == 2)
        return loadAt<std::uint16_t>(indices, static_cast<std::size_t>(i) * 2);
    return loadAt<std::uint32_t>(indices, static_cast<std::size_t>(i) * 4);
}

Vec3 MeshData::position(std::uint32_t v) const noexcept
{
    const VertexLayout l = layout();
    const std::size_t base = static_cast<std::size_t>(v) * l.stride;
    return loadAt<Vec3>(vertices, base + l.position);
}

Vec3 MeshData::normal(std::uint32_t v) const noexcept
{
    const VertexLayout l = layout();
    return loadAt<Vec3>(vertices, static_cast<std::size_t>(v) * l.stride + l.normal);
}

std::array<float, 4> MeshData::tangent(std::uint32_t v) const noexcept
{
    const VertexLayout l = layout();
    if (l.tangent == kAbsent)
        return {0, 0, 0, 0};
    return loadAt<std::array<float, 4>>(vertices, static_cast<std::size_t>(v) * l.stride + l.tangent);
}

std::array<float, 2> MeshData::uv0(std::uint32_t v) const noexcept
{
    const VertexLayout l = layout();
    return loadAt<std::array<float, 2>>(vertices, static_cast<std::size_t>(v) * l.stride + l.uv0);
}

std::array<std::uint8_t, 4> MeshData::color(std::uint32_t v) const noexcept
{
    const VertexLayout l = layout();
    if (l.color == kAbsent)
        return {255, 255, 255, 255};
    return loadAt<std::array<std::uint8_t, 4>>(vertices, static_cast<std::size_t>(v) * l.stride + l.color);
}

std::array<std::uint8_t, 4> MeshData::joints(std::uint32_t v) const noexcept
{
    const VertexLayout l = layout();
    if (l.joints == kAbsent)
        return {0, 0, 0, 0};
    return loadAt<std::array<std::uint8_t, 4>>(vertices, static_cast<std::size_t>(v) * l.stride + l.joints);
}

std::array<std::uint8_t, 4> MeshData::weights(std::uint32_t v) const noexcept
{
    const VertexLayout l = layout();
    if (l.weights == kAbsent)
        return {0, 0, 0, 0};
    return loadAt<std::array<std::uint8_t, 4>>(vertices, static_cast<std::size_t>(v) * l.stride + l.weights);
}

bool MeshData::isSubmeshVisible(std::size_t submesh, std::uint8_t face, std::uint8_t hair) const noexcept
{
    if (submesh >= submeshes.size())
        return false;
    const Submesh& s = submeshes[submesh];
    switch (s.variantGroup)
    {
    case VariantGroup::Always: return true;
    case VariantGroup::Hair: return s.variantIndex == hair;
    case VariantGroup::Face: return s.variantIndex == face;
    }
    return false;
}

void MeshData::computeBounds() noexcept
{
    if (vertexCount == 0)
    {
        boundsMin = boundsMax = sphereCenter = {};
        sphereRadius = 0.0f;
        return;
    }
    Vec3 lo = position(0), hi = lo;
    for (std::uint32_t v = 1; v < vertexCount; ++v)
    {
        const Vec3 p = position(v);
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    boundsMin = lo;
    boundsMax = hi;
    sphereCenter = (lo + hi) * 0.5f;
    float r2 = 0.0f;
    for (std::uint32_t v = 0; v < vertexCount; ++v)
    {
        const Vec3 d = position(v) - sphereCenter;
        r2 = std::max(r2, dot(d, d));
    }
    // Small relative slack so float rounding never puts a vertex outside.
    sphereRadius = std::sqrt(r2) * 1.0001f + 1e-6f;
}

bool parseMesh(std::span<const std::byte> file, MeshData& out, std::string& error)
{
    if (!detail::checkHeader(file, kMagicMesh, error))
        return false;
    if (file.size() < kInfoEnd)
    {
        error = "file too small for MeshInfo";
        return false;
    }
    detail::ByteReader r(file);
    const auto vertexCount = r.read<std::uint32_t>(16);
    const auto indexCount = r.read<std::uint32_t>(20);
    const auto submeshCount = r.read<std::uint32_t>(24);
    const auto boneCount = r.read<std::uint32_t>(28);
    const auto vertexFormat = r.read<std::uint32_t>(32);
    const auto vertexStride = r.read<std::uint32_t>(36);
    const auto indexSize = r.read<std::uint32_t>(40);
    out.boundsMin = readVec3(r, 44);
    out.boundsMax = readVec3(r, 56);
    out.sphereCenter = readVec3(r, 68);
    out.sphereRadius = r.read<float>(80);
    const auto vertexOffset = r.read<std::uint32_t>(84);
    const auto indexOffset = r.read<std::uint32_t>(88);
    const auto submeshOffset = r.read<std::uint32_t>(92);
    const auto boneOffset = r.read<std::uint32_t>(96);
    const auto stringOffset = r.read<std::uint32_t>(100);
    const auto stringSize = r.read<std::uint32_t>(104);
    const auto res0 = r.read<std::uint32_t>(108), res1 = r.read<std::uint32_t>(112), res2 = r.read<std::uint32_t>(116);
    if (!r.ok)
    {
        error = "truncated MeshInfo";
        return false;
    }
    if (vertexCount < 1 || vertexCount > kMaxVertices)
        return error = std::format("vertexCount {} out of range", vertexCount), false;
    if (indexCount < 3 || indexCount > kMaxIndices || indexCount % 3 != 0)
        return error = std::format("indexCount {} invalid", indexCount), false;
    if (submeshCount < 1 || submeshCount > kMaxSubmeshes)
        return error = std::format("submeshCount {} out of range", submeshCount), false;
    if (boneCount > kMaxBones)
        return error = std::format("boneCount {} out of range", boneCount), false;
    if ((vertexFormat & ~kKnownFormatBits) != 0 || (vertexFormat & kVertexNormal) == 0)
        return error = std::format("bad vertexFormat 0x{:x}", vertexFormat), false;
    if (((vertexFormat & kVertexSkin) != 0) != (boneCount > 0))
        return error = "SKIN flag must be set exactly when boneCount > 0", false;
    const VertexLayout layout = VertexLayout::fromFormat(vertexFormat);
    if (vertexStride != layout.stride)
        return error = std::format("vertexStride {} does not match format (expected {})", vertexStride, layout.stride),
               false;
    if (indexSize != 2 && indexSize != 4)
        return error = std::format("indexSize {} invalid", indexSize), false;
    if (indexSize == 2 && vertexCount > 65536)
        return error = "u16 indices with more than 65536 vertices", false;
    if (res0 != 0 || res1 != 0 || res2 != 0)
        return error = "reserved MeshInfo fields not zero", false;
    if (stringSize > kMaxStringTable)
        return error = "string table too large", false;
    if (!r.hasRange(vertexOffset, vertexCount, vertexStride) || !r.hasRange(indexOffset, indexCount, indexSize) ||
        !r.hasRange(submeshOffset, submeshCount, kSubmeshSize) || !r.hasRange(boneOffset, boneCount, kBoneSize) ||
        !r.hasRange(stringOffset, stringSize, 1))
        return error = "section outside the file", false;
    if (!finite(out.boundsMin) || !finite(out.boundsMax) || !finite(out.sphereCenter) || !finite(out.sphereRadius) ||
        out.sphereRadius < 0.0f)
        return error = "invalid bounds", false;

    const std::span<const std::byte> strings = r.bytes(stringOffset, stringSize);

    out.vertexFormat = vertexFormat;
    out.vertexCount = vertexCount;
    out.indexSize = indexSize;
    {
        const auto src = r.bytes(vertexOffset, static_cast<std::uint64_t>(vertexCount) * vertexStride);
        out.vertices.resize(src.size());
        std::memcpy(out.vertices.data(), src.data(), src.size());
        const auto isrc = r.bytes(indexOffset, static_cast<std::uint64_t>(indexCount) * indexSize);
        out.indices.resize(isrc.size());
        std::memcpy(out.indices.data(), isrc.data(), isrc.size());
    }

    // Bones (needed to validate joint indices).
    out.skeleton.bones.clear();
    out.skeleton.bones.resize(boneCount);
    std::set<std::string, std::less<>> boneNames;
    for (std::uint32_t b = 0; b < boneCount; ++b)
    {
        const std::uint64_t base = boneOffset + static_cast<std::uint64_t>(b) * kBoneSize;
        Bone& bone = out.skeleton.bones[b];
        if (!detail::readStringRef(r, base, strings, bone.name) || bone.name.empty())
            return error = std::format("bone {}: bad name", b), false;
        if (!boneNames.insert(bone.name).second)
            return error = std::format("duplicate bone name '{}'", bone.name), false;
        bone.parent = r.read<std::int32_t>(base + 8);
        if (bone.parent < -1 || bone.parent >= static_cast<std::int32_t>(b))
            return error = std::format("bone '{}': parent {} invalid", bone.name, bone.parent), false;
        for (std::size_t k = 0; k < 16; ++k)
        {
            bone.inverseBind.m[k] = r.read<float>(base + 12 + k * 4);
            if (!finite(bone.inverseBind.m[k]))
                return error = std::format("bone '{}': non-finite inverse bind", bone.name), false;
        }
        bone.bindLocal.translation = readVec3(r, base + 76);
        bone.bindLocal.rotation = {r.read<float>(base + 88), r.read<float>(base + 92), r.read<float>(base + 96),
                                   r.read<float>(base + 100)};
        bone.bindLocal.scale = readVec3(r, base + 104);
        if (r.read<std::uint32_t>(base + 116) != 0)
            return error = std::format("bone '{}': reserved field not zero", bone.name), false;
        const Quat& q = bone.bindLocal.rotation;
        if (!finite(bone.bindLocal.translation) || !finite(bone.bindLocal.scale) || !finite(q.x) || !finite(q.y) ||
            !finite(q.z) || !finite(q.w) || std::abs(dot(q, q) - 1.0f) > 1e-3f)
            return error = std::format("bone '{}': invalid bind transform", bone.name), false;
    }

    // Vertices.
    for (std::uint32_t v = 0; v < vertexCount; ++v)
    {
        if (!finite(out.position(v)) || !finite(out.normal(v)))
            return error = std::format("vertex {}: non-finite position/normal", v), false;
        if (out.isSkinned())
        {
            const auto j = out.joints(v);
            const auto w = out.weights(v);
            unsigned sum = 0;
            for (int k = 0; k < 4; ++k)
            {
                if (j[static_cast<std::size_t>(k)] >= boneCount)
                    return error = std::format("vertex {}: joint index out of range", v), false;
                sum += w[static_cast<std::size_t>(k)];
            }
            if (sum != 255)
                return error = std::format("vertex {}: weights sum to {} (must be 255)", v, sum), false;
        }
    }
    for (std::uint32_t i = 0; i < indexCount; ++i)
        if (out.index(i) >= vertexCount)
            return error = std::format("index {} out of range", i), false;

    // Submeshes.
    out.submeshes.clear();
    out.submeshes.resize(submeshCount);
    for (std::uint32_t s = 0; s < submeshCount; ++s)
    {
        const std::uint64_t base = submeshOffset + static_cast<std::uint64_t>(s) * kSubmeshSize;
        Submesh& sm = out.submeshes[s];
        sm.indexStart = r.read<std::uint32_t>(base);
        sm.indexCount = r.read<std::uint32_t>(base + 4);
        if (sm.indexStart % 3 != 0 || sm.indexCount < 3 || sm.indexCount % 3 != 0 ||
            static_cast<std::uint64_t>(sm.indexStart) + sm.indexCount > indexCount)
            return error = std::format("submesh {}: bad index range", s), false;
        if (!detail::readStringRef(r, base + 8, strings, sm.name) ||
            !detail::readStringRef(r, base + 16, strings, sm.texture))
            return error = std::format("submesh {}: bad string reference", s), false;
        if (!sm.texture.empty() && !isValidAssetPath(sm.texture))
            return error = std::format("submesh {}: invalid texture path '{}'", s, sm.texture), false;
        for (std::size_t k = 0; k < 4; ++k)
        {
            sm.baseColor[k] = r.read<float>(base + 24 + k * 4);
            if (!finite(sm.baseColor[k]))
                return error = std::format("submesh {}: non-finite base colour", s), false;
        }
        const auto group = r.read<std::uint8_t>(base + 40);
        sm.variantIndex = r.read<std::uint8_t>(base + 41);
        sm.flags = r.read<std::uint16_t>(base + 42);
        if (group > 2 || sm.variantIndex > 15 || (group == 0 && sm.variantIndex != 0))
            return error = std::format("submesh {}: bad variant {}/{}", s, group, sm.variantIndex), false;
        sm.variantGroup = static_cast<VariantGroup>(group);
        if ((sm.flags & ~static_cast<std::uint16_t>(kSubmeshDoubleSided | kSubmeshAlphaBlend)) != 0)
            return error = std::format("submesh {}: unknown flags", s), false;
    }
    if (!r.ok)
        return error = "truncated data", false;
    return true;
}

bool loadMesh(const std::filesystem::path& path, MeshData& out, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!readFileBytes(path, bytes, error))
        return false;
    if (!parseMesh(bytes, out, error))
    {
        error = std::format("{}: {}", core::pathToUtf8(path), error);
        return false;
    }
    return true;
}

bool serializeMesh(const MeshData& mesh, std::vector<std::byte>& out, std::string& error)
{
    const VertexLayout layout = mesh.layout();
    if (mesh.vertices.size() != static_cast<std::size_t>(mesh.vertexCount) * layout.stride)
        return error = "vertex buffer size does not match vertexCount * stride", false;
    if (mesh.indexSize != 2 && mesh.indexSize != 4)
        return error = "indexSize must be 2 or 4", false;
    if (mesh.indices.size() % mesh.indexSize != 0)
        return error = "index buffer size not a multiple of indexSize", false;
    if (mesh.submeshes.size() > kMaxSubmeshes || mesh.skeleton.bones.size() > kMaxBones)
        return error = "too many submeshes or bones", false;

    detail::ByteWriter w;
    detail::StringTableBuilder strings;
    w.beginFile(kMagicMesh);
    const std::size_t info = w.size();
    w.putZeros(kInfoEnd - info);
    w.patch<std::uint32_t>(16, mesh.vertexCount);
    w.patch<std::uint32_t>(20, mesh.indexCount());
    w.patch<std::uint32_t>(24, static_cast<std::uint32_t>(mesh.submeshes.size()));
    w.patch<std::uint32_t>(28, static_cast<std::uint32_t>(mesh.skeleton.bones.size()));
    w.patch<std::uint32_t>(32, mesh.vertexFormat);
    w.patch<std::uint32_t>(36, layout.stride);
    w.patch<std::uint32_t>(40, mesh.indexSize);
    w.patch<Vec3>(44, mesh.boundsMin);
    w.patch<Vec3>(56, mesh.boundsMax);
    w.patch<Vec3>(68, mesh.sphereCenter);
    w.patch<float>(80, mesh.sphereRadius);

    w.align4();
    w.patch<std::uint32_t>(84, static_cast<std::uint32_t>(w.size()));
    w.putBytes(std::as_bytes(std::span(mesh.vertices)));
    w.align4();
    w.patch<std::uint32_t>(88, static_cast<std::uint32_t>(w.size()));
    w.putBytes(std::as_bytes(std::span(mesh.indices)));
    w.align4();
    w.patch<std::uint32_t>(92, static_cast<std::uint32_t>(w.size()));
    for (const Submesh& s : mesh.submeshes)
    {
        w.put<std::uint32_t>(s.indexStart);
        w.put<std::uint32_t>(s.indexCount);
        const auto n = strings.add(s.name);
        w.put(n.offset);
        w.put(n.length);
        const auto t = strings.add(s.texture);
        w.put(t.offset);
        w.put(t.length);
        for (float c : s.baseColor)
            w.put(c);
        w.put<std::uint8_t>(static_cast<std::uint8_t>(s.variantGroup));
        w.put<std::uint8_t>(s.variantIndex);
        w.put<std::uint16_t>(s.flags);
    }
    w.align4();
    w.patch<std::uint32_t>(96, mesh.skeleton.bones.empty() ? 0u : static_cast<std::uint32_t>(w.size()));
    for (const Bone& b : mesh.skeleton.bones)
    {
        const auto n = strings.add(b.name);
        w.put(n.offset);
        w.put(n.length);
        w.put<std::int32_t>(b.parent);
        for (float f : b.inverseBind.m)
            w.put(f);
        w.put(b.bindLocal.translation);
        w.put(b.bindLocal.rotation);
        w.put(b.bindLocal.scale);
        w.put<std::uint32_t>(0);
    }
    w.align4();
    w.patch<std::uint32_t>(100, static_cast<std::uint32_t>(w.size()));
    w.patch<std::uint32_t>(104, static_cast<std::uint32_t>(strings.bytes().size()));
    w.putBytes(strings.bytes());
    w.align4();
    if (w.size() > kMaxFileSize)
        return error = "mesh too large", false;
    out = w.finishFile();

    MeshData check;
    std::string why;
    if (!parseMesh(out, check, why))
    {
        error = "serialised mesh is invalid: " + why;
        out.clear();
        return false;
    }
    return true;
}

} // namespace vo::assets
