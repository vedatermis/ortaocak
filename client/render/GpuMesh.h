// GPU copy of a .vmesh (docs/ASSET_FORMATS.md §2). Every mesh is converted to one of two canonical
// interleaved vertex layouts so the shaders need no permutations:
//   static  (36 B): position f32x3, normal f32x3, uv0 f32x2, colour u8x4 (sRGB, 255s if absent)
//   skinned (44 B): static + joints u8x4 + weights u8x4 (unorm, sum 255)
// Tangents are dropped (no normal maps yet). Index buffers keep the file's 16/32-bit size.
#pragma once

#include "shared/assets/Mesh.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vo::client::render {

class Texture2D;

inline constexpr std::uint32_t kStaticVertexStride = 36;
inline constexpr std::uint32_t kSkinnedVertexStride = 44;
inline constexpr std::size_t kMaxBones = 64; // bone palette size of Mesh.hlsl

struct GpuSubmesh
{
    std::uint32_t indexStart = 0;
    std::uint32_t indexCount = 0;
    std::array<float, 4> baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    std::string texturePath;            // empty = untextured
    const Texture2D* texture = nullptr; // resolved when the texture is loaded
    std::uint16_t flags = 0;            // assets::SubmeshFlags
};

struct GpuMesh
{
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
    std::uint32_t stride = kStaticVertexStride;
    std::uint32_t triangles = 0;
    bool skinned = false;
    std::vector<GpuSubmesh> submeshes;
    // CPU data kept for skinned meshes (skeleton, appearance variants) and bounds.
    std::shared_ptr<const assets::MeshData> cpu;
    assets::Vec3 sphereCenter{};
    float sphereRadius = 0.0f;
};

// Converts the file's vertices to the canonical layout (any thread).
void convertVertices(const assets::MeshData& mesh, std::vector<std::uint8_t>& out);
// Creates the GPU buffers (render thread). `vertices` from convertVertices().
bool uploadMesh(ID3D11Device* device, std::shared_ptr<const assets::MeshData> mesh,
                const std::vector<std::uint8_t>& vertices, const char* debugName, GpuMesh& out, std::string& error);

} // namespace vo::client::render
