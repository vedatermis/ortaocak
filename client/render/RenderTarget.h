// Offscreen target, e.g. for the character preview: an HDR colour (R16G16B16A16_FLOAT, optionally
// multisampled) + D32 depth that WorldRenderer renders into, and an LDR R8G8B8A8_UNORM_SRGB texture
// that resolve() tone-maps into and SpriteBatch draws as an image (opaque, so premultiplied alpha is
// trivially satisfied).
#pragma once

#include "client/render/WorldRenderer.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace vo::client::render {

class PostProcess;

class RenderTarget
{
public:
    RenderTarget() = default;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    // (Re)creates the textures if the size or sample count changed.
    bool ensure(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const char* debugName,
                std::string& error, std::uint32_t samples = 1);
    void reset() noexcept;
    // Clears the HDR colour (linear RGBA) and depth (0, reverse-Z).
    void clear(ID3D11DeviceContext* context, const float color[4]);
    // Tone-maps (and resolves) the HDR colour into the LDR texture.
    void resolve(ID3D11DeviceContext* context, PostProcess& post);

    // Where WorldRenderer draws (HDR).
    [[nodiscard]] RenderTargetViews views() const noexcept
    {
        return {m_hdrRtv.Get(), m_dsv.Get(), m_width, m_height};
    }
    // The tone-mapped image (valid after resolve()).
    [[nodiscard]] ID3D11ShaderResourceView* srv() const noexcept { return m_srv.Get(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] std::uint32_t samples() const noexcept { return m_samples; }
    [[nodiscard]] bool valid() const noexcept { return m_hdrRtv != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hdr;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_hdrRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_hdrSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_color;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depth;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_samples = 1;
};

} // namespace vo::client::render
