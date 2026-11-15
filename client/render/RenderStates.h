// Cache of the fixed-function state objects the renderer uses (blend, sampler, rasterizer,
// depth-stencil). Created once at init; look-ups are array indexing, no per-frame allocation.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>

namespace vo::client::render {

enum class BlendMode : std::uint8_t
{
    Opaque,
    Premultiplied, // ONE, INV_SRC_ALPHA
    Additive,      // ONE, ONE
    Count,
};

enum class SamplerMode : std::uint8_t
{
    LinearClamp, // trilinear
    LinearWrap,
    PointClamp,
    AnisoWrap,     // 8x anisotropic, wrap (terrain layers)
    ShadowCompare, // comparison LESS_EQUAL, linear, border = lit (shadow map PCF)
    AnisoClamp,    // 8x anisotropic, clamp (mesh material textures: UV atlases)
    Count,
};

enum class RasterMode : std::uint8_t
{
    CullNoneScissor, // 2D: no culling, scissor test on
    CullNone,
    CullBack,      // front faces are clockwise (D3D default; docs/ASSET_FORMATS.md §1)
    ShadowCaster,  // cull back, slope-scaled depth bias (shadow map pass)
    Count,
};

enum class DepthMode : std::uint8_t
{
    None,      // no depth test or write
    ReadWrite, // LESS_EQUAL, write (standard depth: shadow map)
    ReadOnly,  // LESS_EQUAL, no write
    ReverseReadWrite, // GREATER_EQUAL, write (reverse-Z main view)
    ReverseReadOnly,  // GREATER_EQUAL, no write (decals, sky)
    Count,
};

class RenderStates
{
public:
    RenderStates() = default;
    RenderStates(const RenderStates&) = delete;
    RenderStates& operator=(const RenderStates&) = delete;

    bool init(ID3D11Device* device, std::string& error);
    void shutdown() noexcept;

    [[nodiscard]] ID3D11BlendState* blend(BlendMode m) const noexcept { return m_blend[index(m)].Get(); }
    [[nodiscard]] ID3D11SamplerState* sampler(SamplerMode m) const noexcept
    {
        return m_sampler[index(m)].Get();
    }
    [[nodiscard]] ID3D11RasterizerState* raster(RasterMode m) const noexcept
    {
        return m_raster[index(m)].Get();
    }
    [[nodiscard]] ID3D11DepthStencilState* depth(DepthMode m) const noexcept
    {
        return m_depth[index(m)].Get();
    }

private:
    template <typename E>
    static constexpr std::size_t index(E e) noexcept
    {
        return static_cast<std::size_t>(e);
    }

    std::array<Microsoft::WRL::ComPtr<ID3D11BlendState>, index(BlendMode::Count)> m_blend;
    std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>, index(SamplerMode::Count)> m_sampler;
    std::array<Microsoft::WRL::ComPtr<ID3D11RasterizerState>, index(RasterMode::Count)> m_raster;
    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilState>, index(DepthMode::Count)> m_depth;
};

} // namespace vo::client::render
