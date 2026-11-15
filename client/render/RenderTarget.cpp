#include "client/render/RenderTarget.h"

#include "client/platform/Win32Util.h"
#include "client/render/PostProcess.h"
#include "client/render/Shader.h"

namespace vo::client::render {

bool RenderTarget::ensure(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const char* debugName,
                          std::string& error, std::uint32_t samples)
{
    width = width > 0 ? width : 1;
    height = height > 0 ? height : 1;
    samples = samples > 1 ? samples : 1;
    if (samples > 1)
    {
        UINT quality = 0;
        if (FAILED(device->CheckMultisampleQualityLevels(kHdrFormat, samples, &quality)) || quality == 0)
            samples = 1;
    }
    if (valid() && width == m_width && height == m_height && samples == m_samples)
        return true;
    reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = kHdrFormat;
    td.SampleDesc.Count = samples;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_hdr);
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(m_hdr.Get(), nullptr, &m_hdrRtv);
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(m_hdr.Get(), nullptr, &m_hdrSrv);
    if (SUCCEEDED(hr))
    {
        D3D11_TEXTURE2D_DESC dd = td;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        hr = device->CreateTexture2D(&dd, nullptr, &m_depth);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateDepthStencilView(m_depth.Get(), nullptr, &m_dsv);
    if (SUCCEEDED(hr))
    {
        D3D11_TEXTURE2D_DESC cd = td;
        cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        cd.SampleDesc.Count = 1;
        hr = device->CreateTexture2D(&cd, nullptr, &m_color);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(m_color.Get(), nullptr, &m_rtv);
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(m_color.Get(), nullptr, &m_srv);
    if (FAILED(hr))
    {
        error = std::string("render target ") + debugName + ": " + platform::hresultToString(hr);
        reset();
        return false;
    }
    setDebugName(m_hdr.Get(), debugName);
    setDebugName(m_color.Get(), debugName);
    setDebugName(m_depth.Get(), debugName);
    m_width = width;
    m_height = height;
    m_samples = samples;
    return true;
}

void RenderTarget::reset() noexcept
{
    m_srv.Reset();
    m_rtv.Reset();
    m_color.Reset();
    m_hdrSrv.Reset();
    m_hdrRtv.Reset();
    m_hdr.Reset();
    m_dsv.Reset();
    m_depth.Reset();
    m_width = m_height = 0;
    m_samples = 1;
}

void RenderTarget::clear(ID3D11DeviceContext* context, const float color[4])
{
    if (!valid())
        return;
    context->ClearRenderTargetView(m_hdrRtv.Get(), color);
    context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
}

void RenderTarget::resolve(ID3D11DeviceContext* context, PostProcess& post)
{
    if (!valid())
        return;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    post.resolve(context, m_hdrSrv.Get(), m_samples, m_rtv.Get(), m_width, m_height);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}

} // namespace vo::client::render
