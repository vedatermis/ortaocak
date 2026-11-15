// .vtex: pre-mipped, optionally block-compressed 2D texture (docs/ASSET_FORMATS.md §5). Written by
// assetc next to the PNG a mesh material references (`characters/kargan_0_n.png` ->
// `characters/kargan_0_n.vtex`); the client uploads the mips as they are (D3D11-native BC payload).
// Also: CPU decoders for the block formats (tests, `assetc info`).
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::assets {

enum class TextureFormat : std::uint32_t
{
    Rgba8 = 1, // 4 bytes per texel, R G B A
    Bc1 = 2,   // 8 bytes per 4x4 block (DXT1), RGB (+1-bit alpha unused by assetc)
    Bc3 = 3,   // 16 bytes per block (DXT5), RGBA
    Bc4 = 4,   // 8 bytes per block, R
    Bc5 = 5,   // 16 bytes per block, RG
};

// What the texels mean; decides the colour space of the GPU view.
enum class TextureUsage : std::uint32_t
{
    Color = 0,  // sRGB-encoded colour (base colour): *_UNORM_SRGB view
    Normal = 1, // tangent-space normal, X in R, Y in G (OpenGL: +Y = image up), Z reconstructed; linear
    Data = 2,   // linear data (ORM: R occlusion, G roughness, B metallic)
};

struct TextureMip
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> data; // exactly textureMipSize(format, width, height) bytes, rows top first
};

struct TextureData
{
    TextureFormat format = TextureFormat::Rgba8;
    TextureUsage usage = TextureUsage::Color;
    std::vector<TextureMip> mips; // mip 0 = full size; each next level max(1, size / 2)

    [[nodiscard]] std::uint32_t width() const noexcept { return mips.empty() ? 0 : mips.front().width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return mips.empty() ? 0 : mips.front().height; }
    [[nodiscard]] std::uint64_t byteSize() const noexcept;
};

[[nodiscard]] bool isBlockCompressed(TextureFormat f) noexcept;
[[nodiscard]] std::uint32_t textureBlockBytes(TextureFormat f) noexcept; // 0 for Rgba8
[[nodiscard]] std::uint64_t textureMipSize(TextureFormat f, std::uint32_t width, std::uint32_t height) noexcept;
[[nodiscard]] std::string_view textureFormatName(TextureFormat f) noexcept;
[[nodiscard]] std::string_view textureUsageName(TextureUsage u) noexcept;

[[nodiscard]] bool parseTexture(std::span<const std::byte> file, TextureData& out, std::string& error);
[[nodiscard]] bool loadTexture(const std::filesystem::path& path, TextureData& out, std::string& error);
[[nodiscard]] bool serializeTexture(const TextureData& texture, std::vector<std::byte>& out, std::string& error);

// The compiled sibling of a PNG asset path: "a/b.png" -> "a/b.vtex" (any extension replaced).
[[nodiscard]] std::string compiledTexturePath(std::string_view assetPath);

// Decodes mip `level` to RGBA8 (BC4: R in all of RGB, BC5: R, G, 0; alpha 255 unless the format has it).
[[nodiscard]] bool decodeTextureMip(const TextureData& texture, std::size_t level, std::vector<std::uint8_t>& rgba,
                                    std::string& error);

} // namespace vo::assets
