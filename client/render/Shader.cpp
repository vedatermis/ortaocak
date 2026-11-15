#include "client/render/Shader.h"

#include "client/platform/Win32Util.h"

#include <cstring>
#include <string>

namespace vo::client::render {

void setDebugName(ID3D11DeviceChild* object, const char* name) noexcept
{
    if (object != nullptr && name != nullptr)
        object->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
}

bool Pipeline::create(ID3D11Device* device, ShaderBytecode vs, ShaderBytecode ps,
                      std::span<const D3D11_INPUT_ELEMENT_DESC> layout, const char* debugName,
                      std::string& error)
{
    reset();
    HRESULT hr = device->CreateVertexShader(vs.data, vs.size, nullptr, &m_vs);
    if (FAILED(hr))
    {
        error = std::string("CreateVertexShader(") + debugName + ") failed: " + platform::hresultToString(hr);
        return false;
    }
    hr = device->CreatePixelShader(ps.data, ps.size, nullptr, &m_ps);
    if (FAILED(hr))
    {
        error = std::string("CreatePixelShader(") + debugName + ") failed: " + platform::hresultToString(hr);
        reset();
        return false;
    }
    if (!layout.empty())
    {
        hr = device->CreateInputLayout(layout.data(), static_cast<UINT>(layout.size()), vs.data, vs.size,
                                       &m_layout);
        if (FAILED(hr))
        {
            error =
                std::string("CreateInputLayout(") + debugName + ") failed: " + platform::hresultToString(hr);
            reset();
            return false;
        }
    }
    const std::string base = debugName;
    setDebugName(m_vs.Get(), (base + ".vs").c_str());
    setDebugName(m_ps.Get(), (base + ".ps").c_str());
    setDebugName(m_layout.Get(), (base + ".layout").c_str());
    return true;
}

void Pipeline::reset() noexcept
{
    m_vs.Reset();
    m_ps.Reset();
    m_layout.Reset();
}

void Pipeline::bind(ID3D11DeviceContext* context, D3D11_PRIMITIVE_TOPOLOGY topology) const
{
    context->IASetInputLayout(m_layout.Get());
    context->IASetPrimitiveTopology(topology);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);
}

} // namespace vo::client::render
