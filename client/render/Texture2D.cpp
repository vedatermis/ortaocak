#include "client/render/Texture2D.h"

#include "client/platform/Win32Util.h"
#include "client/render/Shader.h"
#include "shared/core/Paths.h"

#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <memory>
#include <stb_image.h>
#include <vector>

namespace vo::client::render {
namespace {

float srgbToLinear(float c) noexcept
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c) noexcept
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

std::uint8_t toByte(float v) noexcept
{
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<std::uint8_t>(c * 255.0f + 0.5f);
}

// Premultiplies RGB by alpha; for sRGB data the multiplication happens in linear space.
void premultiply(std::vector<std::uint8_t>& rgba, bool srgb)
{
    std::array<float, 256> toLinear{};
    for (std::size_t i = 0; i < toLinear.size(); ++i)
        toLinear[i] = srgb ? srgbToLinear(static_cast<float>(i) / 255.0f) : static_cast<float>(i) / 255.0f;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        const std::uint8_t a = rgba[i + 3];
        if (a == 255)
            continue;
        const float af = static_cast<float>(a) / 255.0f;
        for (std::size_t c = 0; c < 3; ++c)
        {
            const float lin = toLinear[rgba[i + c]] * af;
            rgba[i + c] = toByte(srgb ? linearToSrgb(lin) : lin);
        }
    }
}

std::uint32_t fullMipCount(std::uint32_t w, std::uint32_t h) noexcept
{
    const std::uint32_t largest = w > h ? w : h;
    return static_cast<std::uint32_t>(std::bit_width(largest));
}

} // namespace

bool Texture2D::createFromRgba(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t width,
                               std::uint32_t height, const std::uint8_t* rgba, const Options& options,
                               std::string& error)
{
    reset();
    if (width == 0 || height == 0 || rgba == nullptr || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    {
        error = "invalid texture dimensions";
        return false;
    }

    std::vector<std::uint8_t> pixels(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
    if (options.premultiply)
        premultiply(pixels, options.srgb);

    const DXGI_FORMAT format = options.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    bool mips = options.generateMips && context != nullptr;
    if (mips)
    {
        UINT support = 0;
        if (FAILED(device->CheckFormatSupport(format, &support)) ||
            (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) == 0)
            mips = false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mips ? fullMipCount(width, height) : 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mips ? D3D11_BIND_RENDER_TARGET : 0u);
    desc.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0u;

    HRESULT hr = S_OK;
    if (mips)
    {
        hr = device->CreateTexture2D(&desc, nullptr, &m_texture);
    }
    else
    {
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = pixels.data();
        init.SysMemPitch = width * 4;
        hr = device->CreateTexture2D(&desc, &init, &m_texture);
    }
    if (FAILED(hr))
    {
        error = std::string("CreateTexture2D(") + options.debugName +
                ") failed: " + platform::hresultToString(hr);
        reset();
        return false;
    }
    hr = device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv);
    if (FAILED(hr))
    {
        error = "CreateShaderResourceView failed: " + platform::hresultToString(hr);
        reset();
        return false;
    }
    if (mips)
    {
        context->UpdateSubresource(m_texture.Get(), 0, nullptr, pixels.data(), width * 4, 0);
        context->GenerateMips(m_srv.Get());
    }
    setDebugName(m_texture.Get(), options.debugName);
    setDebugName(m_srv.Get(), options.debugName);
    m_width = width;
    m_height = height;
    m_mipLevels = desc.MipLevels;
    return true;
}

bool Texture2D::loadFromFile(ID3D11Device* device, ID3D11DeviceContext* context,
                             const std::filesystem::path& path, const Options& options, std::string& error)
{
    DecodedImage image;
    if (!decodeFile(path, image, error))
        return false;
    return createFromRgba(device, context, image.width, image.height, image.rgba.data(), options, error);
}

bool Texture2D::decodeFile(const std::filesystem::path& path, DecodedImage& out, std::string& error)
{
    // Read via std::filesystem so non-ASCII paths work (stbi_load uses narrow fopen).
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        error = "cannot open " + vo::core::pathToUtf8(path);
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > (std::streamoff{1} << 30))
    {
        error = "bad image file size: " + vo::core::pathToUtf8(path);
        return false;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        error = "read failed: " + vo::core::pathToUtf8(path);
        return false;
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    using StbiPtr = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;
    StbiPtr pixels(stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4),
                   &stbi_image_free);
    if (!pixels)
    {
        error = "image decode failed (" + vo::core::pathToUtf8(path) + "): " + stbi_failure_reason();
        return false;
    }
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.rgba.assign(pixels.get(), pixels.get() + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    return true;
}

bool Texture2D::createR8(ID3D11Device* device, std::uint32_t width, std::uint32_t height,
                         const std::uint8_t* data, const char* debugName, std::string& error)
{
    reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    init.SysMemPitch = width;
    HRESULT hr = device->CreateTexture2D(&desc, &init, &m_texture);
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(m_texture.Get(), nullptr, &m_srv);
    if (FAILED(hr))
    {
        error =
            std::string("R8 texture (") + debugName + ") creation failed: " + platform::hresultToString(hr);
        reset();
        return false;
    }
    setDebugName(m_texture.Get(), debugName);
    setDebugName(m_srv.Get(), debugName);
    m_width = width;
    m_height = height;
    m_mipLevels = 1;
    return true;
}

void Texture2D::reset() noexcept
{
    m_srv.Reset();
    m_texture.Reset();
    m_width = m_height = m_mipLevels = 0;
}

} // namespace vo::client::render
