// HDR -> display: tone map + (MSAA) resolve of a float scene target into an sRGB render target view
// (client/shaders/PostProcess.hlsl). Used for the world (RenderDevice::resolveScene) and the
// character preview (RenderTarget::resolve).
#pragma once

#include "client/render/Shader.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace vo::client::render {

enum class ToneMap : std::uint8_t
{
    AcesFitted, // default
    Clamp,      // no curve: radiance clipped at 1 (debugging / comparison)
};

// Format of every HDR scene target (linear radiance x exposure).
inline constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

class PostProcess
{
public:
    PostProcess() = default;
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    bool init(ID3D11Device* device, std::string& error);
    void reset() noexcept;

    ToneMap toneMap = ToneMap::AcesFitted;

    // Tone-maps `hdr` (an SRV of a kHdrFormat texture with `samples` samples) into `dst` (an sRGB
    // RTV of the same size). Leaves no SRV bound.
    void resolve(ID3D11DeviceContext* context, ID3D11ShaderResourceView* hdr, std::uint32_t samples,
                 ID3D11RenderTargetView* dst, std::uint32_t width, std::uint32_t height);

private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psMs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depth;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
};

} // namespace vo::client::render
