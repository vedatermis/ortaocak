// Material textures -> .vtex (docs/ASSET_FORMATS.md §5): downscale to a maximum size, build the mip
// chain with filtering that suits the data, block-compress (stb_dxt) and serialise.
//
//   colour  sRGB-correct box filter (decode, average in linear light, encode); BC1 (BC3 if any alpha < 255)
//   normal  decode to vectors, average, renormalise; BC5 (X in R, Y in G; Z is reconstructed)
//   data    (ORM) plain linear box filter; BC1 (5:6:5 keeps roughness, in G, at 6 bits)
// Sources must be power-of-two sized. Output is deterministic (no threads affect the bytes).
#pragma once

#include "tools/assetc/Image.h"

#include "shared/assets/Texture.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace vo::tools::assetc {

struct TextureOptions
{
    std::uint32_t maxSize = 2048; // larger sources are halved until they fit (power of two)
    bool compress = true;         // false: RGBA8 mips (debugging / quality comparisons)
};

// An image a material references: an external file, or bytes embedded in the glTF (PNG/JPEG).
struct ImageRef
{
    std::filesystem::path file;
    std::shared_ptr<const std::vector<std::byte>> bytes;
    [[nodiscard]] bool empty() const noexcept { return file.empty() && !bytes; }
};

// One material texture of an imported mesh (GltfImport fills these, writeMeshTextures produces them).
struct TextureRequest
{
    enum class Orm : std::uint8_t
    {
        AsIs,          // `image` already is R occlusion, G roughness, B metallic
        NoOcclusion,   // `image` is glTF metallic-roughness only: R := 255
        Separate,      // G, B from `image`, R from `occlusion.r`
        OcclusionOnly, // R from `occlusion.r`, G = B = 255 (the factors alone decide)
    };
    std::string assetPath; // PNG asset path stored in the submesh; the .vtex goes next to it
    assets::TextureUsage usage = assets::TextureUsage::Color;
    ImageRef image;
    ImageRef occlusion;
    Orm orm = Orm::AsIs;
    bool writePng = false; // the PNG itself is produced by assetc (embedded or packed source)
    // UI images (inventory icons, ASSET_FORMATS.md §5.1): RGB is multiplied by alpha in linear light
    // before the mip chain is built (the sprite shader blends premultiplied), and the texture is
    // stored as RGBA8 whatever TextureOptions::compress says (BC1 has no alpha, BC3 smears small
    // icons). Colour usage only.
    bool premultiplyAlpha = false;
    bool uncompressed = false;
};

// For each request: decode/pack the sources, write `<assetPath>` (only if writePng: the processed
// top level, at most maxSize) and the compiled `<stem>.vtex` under outRoot. Appends written asset
// paths to `written` (if given).
[[nodiscard]] bool writeMeshTextures(const std::vector<TextureRequest>& requests,
                                     const std::filesystem::path& outRoot, const TextureOptions& options,
                                     std::vector<std::string>* written, std::string& error);

// `rgba` must have 4 channels.
[[nodiscard]] bool buildTexture(const Image8& rgba, assets::TextureUsage usage, const TextureOptions& options,
                                assets::TextureData& out, std::string& error);

// One 2x2 box-filter step for the given usage (exposed for tests). Odd/1-texel sides clamp.
[[nodiscard]] Image8 downsampleImage(const Image8& rgba, assets::TextureUsage usage);

} // namespace vo::tools::assetc
