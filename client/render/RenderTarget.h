// Offscreen colour (R8G8B8A8_UNORM_SRGB, rendered linear, sampled as sRGB) + D32 depth target, e.g.
// for the character preview: rendered with WorldRenderer, then drawn by SpriteBatch as an image
// (opaque, so premultiplied alpha is trivially satisfied).
#pragma once

#include "client/render/WorldRenderer.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace vo::client::render {

class RenderTarget
{
public:
    RenderTarget() = default;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    // (Re)creates the textures if the size changed.
    bool ensure(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const char* debugName,
                std::string& error);
    void reset() noexcept;
    // Clears colour (linear RGBA) and depth (0, reverse-Z).
    void clear(ID3D11DeviceContext* context, const float color[4]);

    [[nodiscard]] RenderTargetViews views() const noexcept
    {
        return {m_rtv.Get(), m_dsv.Get(), m_width, m_height};
    }
    [[nodiscard]] ID3D11ShaderResourceView* srv() const noexcept { return m_srv.Get(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] bool valid() const noexcept { return m_rtv != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_color;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depth;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};

} // namespace vo::client::render
