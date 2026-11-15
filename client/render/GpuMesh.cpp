#include "client/render/GpuMesh.h"

#include "client/platform/Win32Util.h"
#include "client/render/Shader.h"

#include <cstring>

namespace vo::client::render {

void convertVertices(const assets::MeshData& mesh, std::vector<std::uint8_t>& out)
{
    const bool skinned = mesh.isSkinned();
    const bool tangents = mesh.hasTangents();
    const std::uint32_t stride = skinned ? kSkinnedVertexStride : kStaticVertexStride;
    out.assign(static_cast<std::size_t>(mesh.vertexCount) * stride, 0);
    for (std::uint32_t v = 0; v < mesh.vertexCount; ++v)
    {
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(v) * stride;
        const assets::Vec3 p = mesh.position(v);
        const assets::Vec3 n = mesh.normal(v);
        const auto uv = mesh.uv0(v);
        const auto c = mesh.color(v);
        const float pf[3] = {p.x, p.y, p.z};
        const float nf[3] = {n.x, n.y, n.z};
        std::memcpy(dst + 0, pf, 12);
        std::memcpy(dst + 12, nf, 12);
        std::memcpy(dst + 24, uv.data(), 8);
        std::memcpy(dst + 32, c.data(), 4);
        if (tangents)
        {
            const auto t = mesh.tangent(v);
            const std::int16_t s[4] = {assets::encodeSnorm16(t[0]), assets::encodeSnorm16(t[1]),
                                       assets::encodeSnorm16(t[2]),
                                       static_cast<std::int16_t>(t[3] < 0.0f ? -32767 : 32767)};
            std::memcpy(dst + 36, s, 8);
        }
        if (skinned)
        {
            const auto j = mesh.joints(v);
            const auto w = mesh.weights(v);
            std::memcpy(dst + 44, j.data(), 4);
            std::memcpy(dst + 48, w.data(), 4);
        }
    }
}

bool uploadMesh(ID3D11Device* device, std::shared_ptr<const assets::MeshData> mesh,
                const std::vector<std::uint8_t>& vertices, const char* debugName, GpuMesh& out, std::string& error)
{
    out = GpuMesh{};
    if (!mesh || vertices.empty() || mesh->indices.empty())
    {
        error = "empty mesh";
        return false;
    }
    if (mesh->isSkinned() && mesh->skeleton.size() > kMaxBones)
    {
        error = "skeleton has " + std::to_string(mesh->skeleton.size()) + " bones (max " +
                std::to_string(kMaxBones) + ")";
        return false;
    }
    D3D11_BUFFER_DESC vb{};
    vb.ByteWidth = static_cast<UINT>(vertices.size());
    vb.Usage = D3D11_USAGE_IMMUTABLE;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit{};
    vinit.pSysMem = vertices.data();
    HRESULT hr = device->CreateBuffer(&vb, &vinit, &out.vertexBuffer);
    if (SUCCEEDED(hr))
    {
        D3D11_BUFFER_DESC ib{};
        ib.ByteWidth = static_cast<UINT>(mesh->indices.size());
        ib.Usage = D3D11_USAGE_IMMUTABLE;
        ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA iinit{};
        iinit.pSysMem = mesh->indices.data();
        hr = device->CreateBuffer(&ib, &iinit, &out.indexBuffer);
    }
    if (FAILED(hr))
    {
        error = std::string("mesh buffers (") + debugName + "): " + platform::hresultToString(hr);
        out = GpuMesh{};
        return false;
    }
    setDebugName(out.vertexBuffer.Get(), debugName);
    setDebugName(out.indexBuffer.Get(), debugName);
    out.indexFormat = mesh->indexSize == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    out.skinned = mesh->isSkinned();
    out.stride = out.skinned ? kSkinnedVertexStride : kStaticVertexStride;
    out.triangles = mesh->indexCount() / 3;
    out.gpuBytes = vertices.size() + mesh->indices.size();
    out.sphereCenter = mesh->sphereCenter;
    out.sphereRadius = mesh->sphereRadius;
    out.submeshes.reserve(mesh->submeshes.size());
    for (const assets::Submesh& s : mesh->submeshes)
    {
        GpuSubmesh g;
        g.indexStart = s.indexStart;
        g.indexCount = s.indexCount;
        g.baseColor = s.baseColor;
        g.texturePath = s.texture;
        g.normalTexturePath = mesh->hasTangents() ? s.normalTexture : std::string();
        g.ormTexturePath = s.ormTexture;
        g.normalScale = s.normalScale;
        g.roughness = s.roughnessFactor;
        g.metallic = s.metallicFactor;
        g.occlusionStrength = s.occlusionStrength;
        g.flags = s.flags;
        g.alphaCutoff = (s.flags & assets::kSubmeshAlphaTest) != 0 ? s.alphaCutoff : 0.0f;
        g.emissiveTexturePath = s.emissiveTexture;
        g.emissiveFactor = s.emissiveFactor;
        out.submeshes.push_back(std::move(g));
    }
    out.cpu = std::move(mesh);
    return true;
}

} // namespace vo::client::render
