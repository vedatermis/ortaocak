#include "client/render/RenderTarget.h"

#include "client/platform/Win32Util.h"
#include "client/render/Shader.h"

namespace vo::client::render {

bool RenderTarget::ensure(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const char* debugName,
                          std::string& error)
{
    width = width > 0 ? width : 1;
    height = height > 0 ? height : 1;
    if (valid() && width == m_width && height == m_height)
        return true;
    reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_color);
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(m_color.Get(), nullptr, &m_rtv);
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(m_color.Get(), nullptr, &m_srv);
    if (SUCCEEDED(hr))
    {
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        hr = device->CreateTexture2D(&td, nullptr, &m_depth);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateDepthStencilView(m_depth.Get(), nullptr, &m_dsv);
    if (FAILED(hr))
    {
        error = std::string("render target ") + debugName + ": " + platform::hresultToString(hr);
        reset();
        return false;
    }
    setDebugName(m_color.Get(), debugName);
    setDebugName(m_depth.Get(), debugName);
    m_width = width;
    m_height = height;
    return true;
}

void RenderTarget::reset() noexcept
{
    m_srv.Reset();
    m_rtv.Reset();
    m_color.Reset();
    m_dsv.Reset();
    m_depth.Reset();
    m_width = m_height = 0;
}

void RenderTarget::clear(ID3D11DeviceContext* context, const float color[4])
{
    if (!valid())
        return;
    context->ClearRenderTargetView(m_rtv.Get(), color);
    context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
}

} // namespace vo::client::render
