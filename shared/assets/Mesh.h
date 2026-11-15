// .vmesh: static or skinned mesh in engine space (docs/ASSET_FORMATS.md §2). CPU-side data only;
// the client converts `vertices` (interleaved, layout from `layout()`) to its canonical layouts.
// Version 2 adds the packed snorm16 tangent stream and PBR material fields per submesh (normal map,
// ORM map, factors); version 1 files still load (their submeshes get the default material).
#pragma once

#include "shared/assets/AssetMath.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::assets {

// serializeMesh writes version 2, or version 3 when a submesh uses alpha test or emission (so meshes
// without them stay byte-identical); versions 1..kMeshVersionMax are read.
inline constexpr std::uint16_t kMeshVersion = 2;
inline constexpr std::uint16_t kMeshVersionMax = 3;

enum VertexFormatBits : std::uint32_t
{
    kVertexNormal = 0x01,         // always set
    kVertexTangent = 0x02,        // f32x4 (v1 layout, still accepted)
    kVertexColor = 0x04,          // u8x4 sRGB
    kVertexSkin = 0x08,           // u8x4 joints + u8x4 unorm weights (sum 255)
    kVertexTangentSnorm16 = 0x20, // v2: snorm16x4, xyz unit, w = +-1 handedness (what assetc writes)
};

inline constexpr std::uint32_t kAbsent = 0xFFFFFFFFu;

// Byte offsets of each stream inside one interleaved vertex (kAbsent if not present).
struct VertexLayout
{
    std::uint32_t stride = 0;
    std::uint32_t position = 0;
    std::uint32_t normal = kAbsent;
    std::uint32_t tangent = kAbsent;
    std::uint32_t uv0 = kAbsent;
    std::uint32_t color = kAbsent;
    std::uint32_t joints = kAbsent;
    std::uint32_t weights = kAbsent;
    bool tangentSnorm16 = false; // tangent stream is snorm16x4 (8 B) instead of f32x4 (16 B)

    [[nodiscard]] static VertexLayout fromFormat(std::uint32_t vertexFormat) noexcept;
};

// snorm16 <-> float as D3D's R16G16B16A16_SNORM (v / 32767, -32768 clamps to -1; writers never use it).
[[nodiscard]] std::int16_t encodeSnorm16(float v) noexcept;
[[nodiscard]] float decodeSnorm16(std::int16_t v) noexcept;

enum class VariantGroup : std::uint8_t
{
    Always = 0,
    Hair = 1,
    Face = 2,
};

enum SubmeshFlags : std::uint16_t
{
    kSubmeshDoubleSided = 0x1,
    kSubmeshAlphaBlend = 0x2,
    kSubmeshAlphaTest = 0x4, // v3: alpha-tested (glTF alphaMode MASK), cutoff in Submesh::alphaCutoff
};

inline constexpr float kMaxEmissive = 1000.0f; // upper bound of each emissiveFactor component

struct Submesh
{
    std::uint32_t indexStart = 0;
    std::uint32_t indexCount = 0;
    std::string name;
    std::string texture; // asset path of the base-colour PNG, empty = untextured
    std::array<float, 4> baseColor{1.0f, 1.0f, 1.0f, 1.0f}; // linear
    VariantGroup variantGroup = VariantGroup::Always;
    std::uint8_t variantIndex = 0;
    std::uint16_t flags = 0;
    // ---- v2 material (version 1 files read these defaults) ----
    std::string normalTexture; // tangent-space normal map PNG (OpenGL convention, +Y = image up), empty = none
    std::string ormTexture;    // R occlusion, G roughness, B metallic (linear), empty = factors only
    float normalScale = 1.0f;       // XY of the decoded normal are scaled by this (glTF normalTexture.scale)
    float roughnessFactor = 1.0f;   // multiplies ORM.g (perceptual roughness)
    float metallicFactor = 0.0f;    // multiplies ORM.b
    float occlusionStrength = 1.0f; // occlusion = lerp(1, ORM.r, strength)
    // ---- v3 (version 1 and 2 files read these defaults) ----
    float alphaCutoff = 0.0f;       // kSubmeshAlphaTest: texel kept where base colour alpha >= cutoff, (0, 1)
    std::string emissiveTexture;    // emitted colour PNG (sRGB), empty = none (then emissiveFactor alone)
    std::array<float, 3> emissiveFactor{0.0f, 0.0f, 0.0f}; // linear, x texture; includes the glTF emissive strength

    // True if anything beyond the version 1 submesh record is used.
    [[nodiscard]] bool usesV2Material() const noexcept;
    // True if anything beyond the version 2 submesh record is used (alpha test, emission).
    [[nodiscard]] bool usesV3Material() const noexcept;
};

struct Bone
{
    std::string name;
    std::int32_t parent = -1; // < own index
    Mat4 inverseBind;         // model -> bone space at bind pose
    Transform bindLocal;      // relative to parent (model space for roots)
};

struct Skeleton
{
    std::vector<Bone> bones;

    [[nodiscard]] bool empty() const noexcept { return bones.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return bones.size(); }
    // Index of the bone with this name, or -1.
    [[nodiscard]] int findBone(std::string_view name) const noexcept;
};

struct MeshData
{
    std::uint32_t vertexFormat = kVertexNormal;
    std::uint32_t vertexCount = 0;
    std::vector<std::uint8_t> vertices; // vertexCount * layout().stride bytes, interleaved
    std::uint32_t indexSize = 2;        // 2 or 4
    std::vector<std::uint8_t> indices;  // indexCount() * indexSize bytes
    std::vector<Submesh> submeshes;
    Skeleton skeleton; // empty for static meshes
    Vec3 boundsMin{};
    Vec3 boundsMax{};
    Vec3 sphereCenter{};
    float sphereRadius = 0.0f;

    [[nodiscard]] VertexLayout layout() const noexcept { return VertexLayout::fromFormat(vertexFormat); }
    [[nodiscard]] bool isSkinned() const noexcept { return (vertexFormat & kVertexSkin) != 0; }
    [[nodiscard]] bool hasTangents() const noexcept
    {
        return (vertexFormat & (kVertexTangent | kVertexTangentSnorm16)) != 0;
    }
    [[nodiscard]] bool hasColors() const noexcept { return (vertexFormat & kVertexColor) != 0; }
    [[nodiscard]] std::uint32_t indexCount() const noexcept
    {
        return indexSize == 0 ? 0u : static_cast<std::uint32_t>(indices.size() / indexSize);
    }

    // Accessors (no bounds checks beyond VO-style debug asserts: callers pass valid indices).
    [[nodiscard]] std::uint32_t index(std::uint32_t i) const noexcept;
    [[nodiscard]] Vec3 position(std::uint32_t v) const noexcept;
    [[nodiscard]] Vec3 normal(std::uint32_t v) const noexcept;
    [[nodiscard]] std::array<float, 4> tangent(std::uint32_t v) const noexcept; // (0,0,0,0) if absent
    // Writes a tangent into vertex v in the mesh's tangent encoding (no-op without a tangent stream).
    void setTangent(std::uint32_t v, const std::array<float, 4>& t) noexcept;
    [[nodiscard]] std::array<float, 2> uv0(std::uint32_t v) const noexcept;
    [[nodiscard]] std::array<std::uint8_t, 4> color(std::uint32_t v) const noexcept;   // 255s if absent
    [[nodiscard]] std::array<std::uint8_t, 4> joints(std::uint32_t v) const noexcept;  // 0s if absent
    [[nodiscard]] std::array<std::uint8_t, 4> weights(std::uint32_t v) const noexcept; // 0s if absent

    // Appearance selection (docs/ASSET_FORMATS.md §2): group 0 always, hair group by `hair`,
    // face group by `face`.
    [[nodiscard]] bool isSubmeshVisible(std::size_t submesh, std::uint8_t face, std::uint8_t hair) const noexcept;

    // Recomputes the box and sphere from the positions.
    void computeBounds() noexcept;
};

// Parses a whole .vmesh file (version 1 or 2). On failure returns false, sets `error`, `out` is
// unspecified.
[[nodiscard]] bool parseMesh(std::span<const std::byte> file, MeshData& out, std::string& error);
[[nodiscard]] bool loadMesh(const std::filesystem::path& path, MeshData& out, std::string& error);
// Serialises `mesh` as version kMeshVersion, or 3 if a submesh uses v3 data (deterministic bytes).
// Fails if the data would not pass parseMesh.
[[nodiscard]] bool serializeMesh(const MeshData& mesh, std::vector<std::byte>& out, std::string& error);
// Serialises as a specific version (1..3). Version 1 fails if the mesh uses v2-only data (the
// snorm16 tangent stream, normal/ORM maps or non-default material factors); versions 1 and 2 fail
// if it uses v3 data (alpha test, emission).
[[nodiscard]] bool serializeMeshAs(const MeshData& mesh, std::uint16_t version, std::vector<std::byte>& out,
                                   std::string& error);
// Version field of a .vmesh header (0 if too short); no other validation.
[[nodiscard]] std::uint16_t meshFileVersion(std::span<const std::byte> file) noexcept;

} // namespace vo::assets
