#include "tools/assetc/TextureCompiler.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Paths.h"

#include <stb_dxt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <thread>
#include <vector>

namespace vo::tools::assetc {

using assets::TextureData;
using assets::TextureFormat;
using assets::TextureUsage;

namespace {

struct SrgbTables
{
    std::array<float, 256> toLinear{};
    SrgbTables()
    {
        for (std::size_t i = 0; i < 256; ++i)
        {
            const float c = static_cast<float>(i) / 255.0f;
            toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
    }
};

const SrgbTables& srgb()
{
    static const SrgbTables t;
    return t;
}

std::uint8_t linearToSrgb8(float c) noexcept
{
    c = std::clamp(c, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

std::uint8_t unorm8(float c) noexcept
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
}

bool isPow2(int v) noexcept
{
    return v > 0 && std::has_single_bit(static_cast<unsigned>(v));
}

// Runs fn(rowBegin, rowEnd) over [0, rows) on a few threads; each row's result is independent, so
// the output does not depend on the split.
template <class Fn>
void parallelRows(std::uint32_t rows, Fn fn)
{
    const unsigned hw = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    if (rows < 64 || hw == 1)
    {
        fn(0u, rows);
        return;
    }
    const std::uint32_t per = (rows + hw - 1) / hw;
    std::vector<std::thread> threads;
    for (std::uint32_t r = 0; r < rows; r += per)
        threads.emplace_back([&fn, r, per, rows]() { fn(r, std::min(rows, r + per)); });
    for (std::thread& t : threads)
        t.join();
}

// Gathers the 4x4 block at (bx, by) as RGBA, clamping at the image edge (mips smaller than 4).
void gatherBlock(const Image8& img, std::uint32_t bx, std::uint32_t by, std::uint8_t out[64])
{
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
        {
            const std::uint32_t sx = std::min<std::uint32_t>(bx * 4 + x, static_cast<std::uint32_t>(img.width) - 1);
            const std::uint32_t sy = std::min<std::uint32_t>(by * 4 + y, static_cast<std::uint32_t>(img.height) - 1);
            std::memcpy(out + (y * 4 + x) * 4,
                        img.pixels.data() + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(img.width) + sx) * 4, 4);
        }
}

void compressMip(const Image8& img, TextureFormat format, std::vector<std::uint8_t>& out)
{
    const std::uint32_t bw = std::max(1u, (static_cast<std::uint32_t>(img.width) + 3) / 4);
    const std::uint32_t bh = std::max(1u, (static_cast<std::uint32_t>(img.height) + 3) / 4);
    const std::uint32_t blockBytes = assets::textureBlockBytes(format);
    out.assign(static_cast<std::size_t>(bw) * bh * blockBytes, 0);
    parallelRows(bh, [&](std::uint32_t y0, std::uint32_t y1) {
        std::uint8_t block[64];
        for (std::uint32_t by = y0; by < y1; ++by)
            for (std::uint32_t bx = 0; bx < bw; ++bx)
            {
                gatherBlock(img, bx, by, block);
                std::uint8_t* dst = out.data() + (static_cast<std::size_t>(by) * bw + bx) * blockBytes;
                switch (format)
                {
                case TextureFormat::Bc1: stb_compress_dxt_block(dst, block, 0, STB_DXT_HIGHQUAL); break;
                case TextureFormat::Bc3: stb_compress_dxt_block(dst, block, 1, STB_DXT_HIGHQUAL); break;
                case TextureFormat::Bc4:
                {
                    std::uint8_t r[16];
                    for (int i = 0; i < 16; ++i)
                        r[i] = block[i * 4];
                    stb_compress_bc4_block(dst, r);
                    break;
                }
                case TextureFormat::Bc5:
                {
                    std::uint8_t rg[32];
                    for (int i = 0; i < 16; ++i)
                    {
                        rg[i * 2] = block[i * 4];
                        rg[i * 2 + 1] = block[i * 4 + 1];
                    }
                    stb_compress_bc5_block(dst, rg);
                    break;
                }
                case TextureFormat::Rgba8: break;
                }
            }
    });
}

} // namespace

Image8 downsampleImage(const Image8& src, TextureUsage usage)
{
    Image8 dst;
    dst.width = std::max(1, src.width / 2);
    dst.height = std::max(1, src.height / 2);
    dst.channels = 4;
    dst.pixels.assign(static_cast<std::size_t>(dst.width) * static_cast<std::size_t>(dst.height) * 4, 0);
    const auto& lin = srgb().toLinear;
    const auto at = [&](int x, int y) {
        x = std::min(x, src.width - 1);
        y = std::min(y, src.height - 1);
        return src.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(src.width) + static_cast<std::size_t>(x)) * 4;
    };
    parallelRows(static_cast<std::uint32_t>(dst.height), [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t uy = y0; uy < y1; ++uy)
        {
            const int y = static_cast<int>(uy);
            for (int x = 0; x < dst.width; ++x)
            {
                const std::uint8_t* p[4] = {at(2 * x, 2 * y), at(2 * x + 1, 2 * y), at(2 * x, 2 * y + 1),
                                            at(2 * x + 1, 2 * y + 1)};
                std::uint8_t* o = dst.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst.width) + static_cast<std::size_t>(x)) * 4;
                float a = 0.0f;
                for (const std::uint8_t* s : p)
                    a += static_cast<float>(s[3]);
                o[3] = static_cast<std::uint8_t>(std::lround(a / 4.0f));
                switch (usage)
                {
                case TextureUsage::Color:
                    for (int c = 0; c < 3; ++c)
                    {
                        float sum = 0.0f;
                        for (const std::uint8_t* s : p)
                            sum += lin[s[c]];
                        o[c] = linearToSrgb8(sum / 4.0f);
                    }
                    break;
                case TextureUsage::Normal:
                {
                    float v[3] = {0, 0, 0};
                    for (const std::uint8_t* s : p)
                    {
                        float n[3];
                        for (int c = 0; c < 3; ++c)
                            n[c] = static_cast<float>(s[c]) / 127.5f - 1.0f;
                        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        for (int c = 0; c < 3; ++c)
                            v[c] += len > 1e-6f ? n[c] / len : (c == 2 ? 1.0f : 0.0f);
                    }
                    float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (!(len > 1e-6f))
                    {
                        v[0] = v[1] = 0.0f;
                        v[2] = len = 1.0f;
                    }
                    for (int c = 0; c < 3; ++c)
                        o[c] = unorm8((v[c] / len) * 0.5f + 0.5f);
                    break;
                }
                case TextureUsage::Data:
                    for (int c = 0; c < 3; ++c)
                    {
                        float sum = 0.0f;
                        for (const std::uint8_t* s : p)
                            sum += static_cast<float>(s[c]);
                        o[c] = static_cast<std::uint8_t>(std::lround(sum / 4.0f));
                    }
                    break;
                }
            }
        }
    });
    return dst;
}

bool buildTexture(const Image8& rgba, TextureUsage usage, const TextureOptions& options, TextureData& out,
                  std::string& error)
{
    if (rgba.channels != 4 || rgba.pixels.size() != static_cast<std::size_t>(rgba.width) * static_cast<std::size_t>(rgba.height) * 4)
        return error = "buildTexture: expected an RGBA image", false;
    if (!isPow2(rgba.width) || !isPow2(rgba.height))
        return error = std::format("texture is {}x{}; material textures must be power-of-two sized", rgba.width,
                                   rgba.height),
               false;
    if (options.maxSize < 4 || !std::has_single_bit(options.maxSize))
        return error = "max texture size must be a power of two >= 4", false;

    Image8 level = rgba;
    while (static_cast<std::uint32_t>(std::max(level.width, level.height)) > options.maxSize)
        level = downsampleImage(level, usage);
    if (options.compress && (level.width < 4 || level.height < 4))
        return error = "block-compressed textures must be at least 4x4", false;

    // Renormalise the top level of normal maps too (sources are not always unit length).
    if (usage == TextureUsage::Normal)
    {
        for (std::size_t i = 0; i + 3 < level.pixels.size(); i += 4)
        {
            float n[3];
            for (int c = 0; c < 3; ++c)
                n[c] = static_cast<float>(level.pixels[i + static_cast<std::size_t>(c)]) / 127.5f - 1.0f;
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-6f)
                for (int c = 0; c < 3; ++c)
                    level.pixels[i + static_cast<std::size_t>(c)] = unorm8((n[c] / len) * 0.5f + 0.5f);
        }
    }

    TextureFormat format = TextureFormat::Rgba8;
    if (options.compress)
    {
        switch (usage)
        {
        case TextureUsage::Color:
        {
            bool alpha = false;
            for (std::size_t i = 3; i < level.pixels.size(); i += 4)
                alpha = alpha || level.pixels[i] != 255;
            format = alpha ? TextureFormat::Bc3 : TextureFormat::Bc1;
            break;
        }
        case TextureUsage::Normal: format = TextureFormat::Bc5; break;
        case TextureUsage::Data: format = TextureFormat::Bc1; break;
        }
    }

    out = TextureData{};
    out.format = format;
    out.usage = usage;
    for (;;)
    {
        assets::TextureMip mip;
        mip.width = static_cast<std::uint32_t>(level.width);
        mip.height = static_cast<std::uint32_t>(level.height);
        if (format == TextureFormat::Rgba8)
            mip.data = level.pixels;
        else
            compressMip(level, format, mip.data);
        out.mips.push_back(std::move(mip));
        if (level.width == 1 && level.height == 1)
            break;
        level = downsampleImage(level, usage);
    }
    return true;
}

namespace {

bool loadRef(const ImageRef& ref, Image8& out, std::string& error)
{
    if (ref.bytes)
        return decodeImage8(*ref.bytes, 4, out, error);
    return loadImage8(ref.file, 4, out, error);
}

} // namespace

bool writeMeshTextures(const std::vector<TextureRequest>& requests, const std::filesystem::path& outRoot,
                       const TextureOptions& options, std::vector<std::string>* written, std::string& error)
{
    for (const TextureRequest& req : requests)
    {
        Image8 img;
        if (req.usage == TextureUsage::Data && req.orm == TextureRequest::Orm::OcclusionOnly)
        {
            if (!loadRef(req.occlusion, img, error))
                return error = req.assetPath + ": " + error, false;
            for (std::size_t i = 0; i < img.pixels.size(); i += 4)
            {
                img.pixels[i + 1] = 255;
                img.pixels[i + 2] = 255;
                img.pixels[i + 3] = 255;
            }
        }
        else
        {
            if (!loadRef(req.image, img, error))
                return error = req.assetPath + ": " + error, false;
            if (req.usage == TextureUsage::Data && req.orm == TextureRequest::Orm::NoOcclusion)
                for (std::size_t i = 0; i < img.pixels.size(); i += 4)
                    img.pixels[i] = 255;
            if (req.usage == TextureUsage::Data && req.orm == TextureRequest::Orm::Separate)
            {
                Image8 occ;
                if (!loadRef(req.occlusion, occ, error))
                    return error = req.assetPath + ": " + error, false;
                if (occ.width != img.width || occ.height != img.height)
                    return error = req.assetPath + ": occlusion and metallic-roughness images differ in size", false;
                for (std::size_t i = 0; i < img.pixels.size(); i += 4)
                    img.pixels[i] = occ.pixels[i];
            }
            if (req.usage != TextureUsage::Color)
                for (std::size_t i = 3; i < img.pixels.size(); i += 4)
                    img.pixels[i] = 255; // data maps carry no alpha
        }
        if (req.premultiplyAlpha)
        {
            if (req.usage != TextureUsage::Color)
                return error = req.assetPath + ": premultiplied alpha needs a colour texture", false;
            const auto& lin = srgb().toLinear;
            for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4)
            {
                const std::uint8_t a = img.pixels[i + 3];
                if (a == 255)
                    continue;
                const float af = static_cast<float>(a) / 255.0f;
                for (std::size_t c = 0; c < 3; ++c)
                    img.pixels[i + c] = linearToSrgb8(lin[img.pixels[i + c]] * af);
            }
        }
        if (!isPow2(img.width) || !isPow2(img.height))
            return error = std::format("{}: {}x{} is not power-of-two sized", req.assetPath, img.width, img.height),
                   false;
        while (static_cast<std::uint32_t>(std::max(img.width, img.height)) > options.maxSize)
            img = downsampleImage(img, req.usage);
        if (req.writePng)
        {
            if (!writePng8(outRoot / core::pathFromUtf8(req.assetPath),
                           img, error))
                return error = req.assetPath + ": " + error, false;
            if (written != nullptr)
                written->push_back(req.assetPath);
        }
        TextureOptions texOptions = options;
        if (req.uncompressed)
            texOptions.compress = false;
        TextureData tex;
        if (!buildTexture(img, req.usage, texOptions, tex, error))
            return error = req.assetPath + ": " + error, false;
        std::vector<std::byte> bytes;
        if (!assets::serializeTexture(tex, bytes, error))
            return error = req.assetPath + ": " + error, false;
        const std::string vtex = assets::compiledTexturePath(req.assetPath);
        if (!assets::writeFileBytes(outRoot / core::pathFromUtf8(vtex), bytes,
                                    error))
            return false;
        if (written != nullptr)
            written->push_back(vtex);
    }
    return true;
}

} // namespace vo::tools::assetc
