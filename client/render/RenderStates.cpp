#include "client/render/RenderStates.h"

#include "client/platform/Win32Util.h"
#include "client/render/Shader.h"

namespace vo::client::render {

bool RenderStates::init(ID3D11Device* device, std::string& error)
{
    auto check = [&](HRESULT hr, const char* what)
    {
        if (SUCCEEDED(hr))
            return true;
        error = std::string(what) + " failed: " + platform::hresultToString(hr);
        return false;
    };

    // ---- Blend ----
    {
        D3D11_BLEND_DESC desc{};
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (!check(device->CreateBlendState(&desc, &m_blend[index(BlendMode::Opaque)]),
                   "CreateBlendState(opaque)"))
            return false;

        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        if (!check(device->CreateBlendState(&desc, &m_blend[index(BlendMode::Premultiplied)]),
                   "CreateBlendState(premultiplied)"))
            return false;

        desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        if (!check(device->CreateBlendState(&desc, &m_blend[index(BlendMode::Additive)]),
                   "CreateBlendState(additive)"))
            return false;
    }

    // ---- Samplers ----
    {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        if (!check(device->CreateSamplerState(&desc, &m_sampler[index(SamplerMode::LinearClamp)]),
                   "CreateSamplerState(linear clamp)"))
            return false;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        if (!check(device->CreateSamplerState(&desc, &m_sampler[index(SamplerMode::LinearWrap)]),
                   "CreateSamplerState(linear wrap)"))
            return false;
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (!check(device->CreateSamplerState(&desc, &m_sampler[index(SamplerMode::PointClamp)]),
                   "CreateSamplerState(point clamp)"))
            return false;
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = 8;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        if (!check(device->CreateSamplerState(&desc, &m_sampler[index(SamplerMode::AnisoWrap)]),
                   "CreateSamplerState(aniso wrap)"))
            return false;
        desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        desc.MaxAnisotropy = 1;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 1.0f;
        desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (!check(device->CreateSamplerState(&desc, &m_sampler[index(SamplerMode::ShadowCompare)]),
                   "CreateSamplerState(shadow compare)"))
            return false;
    }

    // ---- Rasterizer ----
    {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = TRUE;
        if (!check(device->CreateRasterizerState(&desc, &m_raster[index(RasterMode::CullNoneScissor)]),
                   "CreateRasterizerState(2d)"))
            return false;
        desc.ScissorEnable = FALSE;
        if (!check(device->CreateRasterizerState(&desc, &m_raster[index(RasterMode::CullNone)]),
                   "CreateRasterizerState(cull none)"))
            return false;
        desc.CullMode = D3D11_CULL_BACK;
        if (!check(device->CreateRasterizerState(&desc, &m_raster[index(RasterMode::CullBack)]),
                   "CreateRasterizerState(cull back)"))
            return false;
        desc.DepthBias = 600;
        desc.SlopeScaledDepthBias = 2.0f;
        desc.DepthBiasClamp = 0.01f;
        if (!check(device->CreateRasterizerState(&desc, &m_raster[index(RasterMode::ShadowCaster)]),
                   "CreateRasterizerState(shadow caster)"))
            return false;
    }

    // ---- Depth ----
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = FALSE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (!check(device->CreateDepthStencilState(&desc, &m_depth[index(DepthMode::None)]),
                   "CreateDepthStencilState(none)"))
            return false;
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (!check(device->CreateDepthStencilState(&desc, &m_depth[index(DepthMode::ReadWrite)]),
                   "CreateDepthStencilState(rw)"))
            return false;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (!check(device->CreateDepthStencilState(&desc, &m_depth[index(DepthMode::ReadOnly)]),
                   "CreateDepthStencilState(ro)"))
            return false;
        desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        if (!check(device->CreateDepthStencilState(&desc, &m_depth[index(DepthMode::ReverseReadWrite)]),
                   "CreateDepthStencilState(reverse rw)"))
            return false;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (!check(device->CreateDepthStencilState(&desc, &m_depth[index(DepthMode::ReverseReadOnly)]),
                   "CreateDepthStencilState(reverse ro)"))
            return false;
    }
    return true;
}

void RenderStates::shutdown() noexcept
{
    for (auto& s : m_blend)
        s.Reset();
    for (auto& s : m_sampler)
        s.Reset();
    for (auto& s : m_raster)
        s.Reset();
    for (auto& s : m_depth)
        s.Reset();
}

} // namespace vo::client::render
