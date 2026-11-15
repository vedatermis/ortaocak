// .vmesh: static or skinned mesh in engine space (docs/ASSET_FORMATS.md §2). CPU-side data only;
// the client uploads `vertices`/`indices` as-is (interleaved, layout from `layout()`).
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

enum VertexFormatBits : std::uint32_t
{
    kVertexNormal = 0x01,  // always set
    kVertexTangent = 0x02, // f32x4
    kVertexColor = 0x04,   // u8x4 sRGB
    kVertexSkin = 0x08,    // u8x4 joints + u8x4 unorm weights (sum 255)
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

    [[nodiscard]] static VertexLayout fromFormat(std::uint32_t vertexFormat) noexcept;
};

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
};

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
    [[nodiscard]] bool hasTangents() const noexcept { return (vertexFormat & kVertexTangent) != 0; }
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

// Parses a whole .vmesh file. On failure returns false, sets `error`, `out` is unspecified.
[[nodiscard]] bool parseMesh(std::span<const std::byte> file, MeshData& out, std::string& error);
[[nodiscard]] bool loadMesh(const std::filesystem::path& path, MeshData& out, std::string& error);
// Serialises `mesh` (deterministic bytes). Fails if the data would not pass parseMesh.
[[nodiscard]] bool serializeMesh(const MeshData& mesh, std::vector<std::byte>& out, std::string& error);

} // namespace vo::assets
