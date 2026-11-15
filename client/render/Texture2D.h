// 2D texture with a shader resource view. Color textures are stored as R8G8B8A8_UNORM_SRGB with
// premultiplied alpha (premultiplied in linear space at load), optionally with a full mip chain
// generated on the GPU. Single-channel (R8) textures are used for font coverage atlases.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vo::assets {
struct TextureData;
}

namespace vo::client::render {

// RGBA8 pixels decoded on any thread (asset loader worker); uploaded later on the render thread.
struct DecodedImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

class Texture2D
{
public:
    // PNG / JPG / TGA / BMP via stb_image into RGBA8. Thread-safe (no D3D).
    static bool decodeFile(const std::filesystem::path& path, DecodedImage& out, std::string& error);

    struct Options
    {
        bool srgb = true; // data is sRGB-encoded color (false: linear data, e.g. masks)
        bool generateMips = true;
        bool premultiply = true; // input is straight alpha; convert to premultiplied
        const char* debugName = "texture";
    };

    Texture2D() = default;
    Texture2D(Texture2D&&) noexcept = default;
    Texture2D& operator=(Texture2D&&) noexcept = default;
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    // `rgba` = width*height*4 bytes, rows tightly packed, top row first.
    bool createFromRgba(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t width,
                        std::uint32_t height, const std::uint8_t* rgba, const Options& options,
                        std::string& error);
    // PNG / JPG / TGA / BMP via stb_image.
    bool loadFromFile(ID3D11Device* device, ID3D11DeviceContext* context, const std::filesystem::path& path,
                      const Options& options, std::string& error);
    // Single-channel 8-bit texture without mips (font atlases).
    bool createR8(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const std::uint8_t* data,
                  const char* debugName, std::string& error);
    // Immutable texture with every mip of a .vtex (block-compressed payloads upload as they are). The
    // view is sRGB for colour textures, linear for normal and data maps.
    bool createFromTextureData(ID3D11Device* device, const assets::TextureData& data, const char* debugName,
                               std::string& error);
    // GPU memory of the texels (all mips; estimated for GPU-generated mips).
    [[nodiscard]] std::uint64_t gpuBytes() const noexcept { return m_bytes; }

    void reset() noexcept;

    [[nodiscard]] ID3D11ShaderResourceView* srv() const noexcept { return m_srv.Get(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] std::uint32_t mipLevels() const noexcept { return m_mipLevels; }
    [[nodiscard]] bool valid() const noexcept { return m_srv != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_mipLevels = 0;
    std::uint64_t m_bytes = 0;
};

} // namespace vo::client::render
