#include "shared/assets/Texture.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Paths.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace vo::assets {

namespace {

constexpr std::size_t kInfoEnd = 48;
constexpr std::uint32_t kMaxDimension = 16384;
constexpr std::uint32_t kMaxMips = 15;

bool validFormat(std::uint32_t f) noexcept
{
    return f >= static_cast<std::uint32_t>(TextureFormat::Rgba8) && f <= static_cast<std::uint32_t>(TextureFormat::Bc5);
}

// Which formats each usage may be stored in (sRGB colour needs a format with an _SRGB view; normal
// maps need two channels).
bool formatAllowed(TextureUsage u, TextureFormat f) noexcept
{
    switch (u)
    {
    case TextureUsage::Color: return f == TextureFormat::Rgba8 || f == TextureFormat::Bc1 || f == TextureFormat::Bc3;
    case TextureUsage::Normal: return f == TextureFormat::Rgba8 || f == TextureFormat::Bc5;
    case TextureUsage::Data: return true;
    }
    return false;
}

std::uint32_t fullMipCount(std::uint32_t w, std::uint32_t h) noexcept
{
    return static_cast<std::uint32_t>(std::bit_width(std::max(w, h)));
}

// ---- block decoders ----

void decode565(std::uint16_t c, std::uint8_t out[3]) noexcept
{
    const unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    out[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    out[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
}

// BC1 colour block -> 16 RGBA texels (alpha written only if `writeAlpha`).
void decodeBc1(const std::uint8_t* block, std::uint8_t out[16][4], bool forceFourColor, bool writeAlpha) noexcept
{
    std::uint16_t c0 = 0, c1 = 0;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    std::uint8_t pal[4][4] = {};
    decode565(c0, pal[0]);
    decode565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    if (c0 > c1 || forceFourColor)
    {
        for (int k = 0; k < 3; ++k)
        {
            pal[2][k] = static_cast<std::uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<std::uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
    }
    else
    {
        for (int k = 0; k < 3; ++k)
            pal[2][k] = static_cast<std::uint8_t>((pal[0][k] + pal[1][k]) / 2);
        pal[3][3] = 0;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, block + 4, 4);
    for (int i = 0; i < 16; ++i)
    {
        const std::uint8_t* p = pal[(bits >> (2 * i)) & 3u];
        out[i][0] = p[0];
        out[i][1] = p[1];
        out[i][2] = p[2];
        if (writeAlpha)
            out[i][3] = p[3];
    }
}

// BC4 block (8 bytes) -> 16 values.
void decodeBc4(const std::uint8_t* block, std::uint8_t out[16]) noexcept
{
    const unsigned a0 = block[0], a1 = block[1];
    unsigned pal[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
    if (a0 > a1)
    {
        for (unsigned i = 1; i < 7; ++i)
            pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    }
    else
    {
        for (unsigned i = 1; i < 5; ++i)
            pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<std::uint8_t>(pal[(bits >> (3 * i)) & 7u]);
}

} // namespace

std::uint64_t TextureData::byteSize() const noexcept
{
    std::uint64_t total = 0;
    for (const TextureMip& m : mips)
        total += m.data.size();
    return total;
}

bool isBlockCompressed(TextureFormat f) noexcept
{
    return f != TextureFormat::Rgba8;
}

std::uint32_t textureBlockBytes(TextureFormat f) noexcept
{
    switch (f)
    {
    case TextureFormat::Bc1:
    case TextureFormat::Bc4: return 8;
    case TextureFormat::Bc3:
    case TextureFormat::Bc5: return 16;
    case TextureFormat::Rgba8: return 0;
    }
    return 0;
}

std::uint64_t textureMipSize(TextureFormat f, std::uint32_t width, std::uint32_t height) noexcept
{
    if (!isBlockCompressed(f))
        return static_cast<std::uint64_t>(width) * height * 4;
    const std::uint64_t bw = std::max<std::uint64_t>(1, (static_cast<std::uint64_t>(width) + 3) / 4);
    const std::uint64_t bh = std::max<std::uint64_t>(1, (static_cast<std::uint64_t>(height) + 3) / 4);
    return bw * bh * textureBlockBytes(f);
}

std::string_view textureFormatName(TextureFormat f) noexcept
{
    switch (f)
    {
    case TextureFormat::Rgba8: return "RGBA8";
    case TextureFormat::Bc1: return "BC1";
    case TextureFormat::Bc3: return "BC3";
    case TextureFormat::Bc4: return "BC4";
    case TextureFormat::Bc5: return "BC5";
    }
    return "?";
}

std::string_view textureUsageName(TextureUsage u) noexcept
{
    switch (u)
    {
    case TextureUsage::Color: return "colour (sRGB)";
    case TextureUsage::Normal: return "normal map (linear, RG = XY)";
    case TextureUsage::Data: return "data (linear)";
    }
    return "?";
}

bool parseTexture(std::span<const std::byte> file, TextureData& out, std::string& error)
{
    if (!detail::checkHeader(file, kMagicTexture, error))
        return false;
    if (file.size() < kInfoEnd)
        return error = "file too small for TexInfo", false;
    detail::ByteReader r(file);
    const auto width = r.read<std::uint32_t>(16);
    const auto height = r.read<std::uint32_t>(20);
    const auto mipCount = r.read<std::uint32_t>(24);
    const auto format = r.read<std::uint32_t>(28);
    const auto usage = r.read<std::uint32_t>(32);
    const auto tableOffset = r.read<std::uint32_t>(36);
    const auto res0 = r.read<std::uint32_t>(40);
    const auto res1 = r.read<std::uint32_t>(44);
    if (!r.ok)
        return error = "truncated TexInfo", false;
    if (width < 1 || height < 1 || width > kMaxDimension || height > kMaxDimension)
        return error = std::format("size {}x{} out of range", width, height), false;
    if (!validFormat(format))
        return error = std::format("unknown format {}", format), false;
    if (usage > static_cast<std::uint32_t>(TextureUsage::Data))
        return error = std::format("unknown usage {}", usage), false;
    const auto fmt = static_cast<TextureFormat>(format);
    const auto use = static_cast<TextureUsage>(usage);
    if (!formatAllowed(use, fmt))
        return error = std::format("format {} not allowed for {}", textureFormatName(fmt), textureUsageName(use)),
               false;
    if (isBlockCompressed(fmt) && (width % 4 != 0 || height % 4 != 0))
        return error = "block-compressed size must be a multiple of 4", false;
    if (mipCount < 1 || mipCount > kMaxMips || mipCount > fullMipCount(width, height))
        return error = std::format("mipCount {} invalid", mipCount), false;
    if (res0 != 0 || res1 != 0)
        return error = "reserved TexInfo fields not zero", false;
    if (tableOffset < kInfoEnd || !r.hasRange(tableOffset, mipCount, 8))
        return error = "mip table outside the file", false;

    out.format = fmt;
    out.usage = use;
    out.mips.clear();
    out.mips.resize(mipCount);
    std::uint64_t minOffset = static_cast<std::uint64_t>(tableOffset) + 8ull * mipCount;
    for (std::uint32_t i = 0; i < mipCount; ++i)
    {
        TextureMip& m = out.mips[i];
        m.width = std::max(1u, width >> i);
        m.height = std::max(1u, height >> i);
        const auto offset = r.read<std::uint32_t>(tableOffset + 8ull * i);
        const auto size = r.read<std::uint32_t>(tableOffset + 8ull * i + 4);
        if (size != textureMipSize(fmt, m.width, m.height))
            return error = std::format("mip {}: size {} does not match {}x{}", i, size, m.width, m.height), false;
        if (offset < minOffset || !r.hasRange(offset, size, 1))
            return error = std::format("mip {}: data outside the file or overlapping", i), false;
        minOffset = static_cast<std::uint64_t>(offset) + size;
        const auto src = r.bytes(offset, size);
        m.data.resize(src.size());
        std::memcpy(m.data.data(), src.data(), src.size());
    }
    if (!r.ok)
        return error = "truncated data", false;
    return true;
}

bool loadTexture(const std::filesystem::path& path, TextureData& out, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!readFileBytes(path, bytes, error))
        return false;
    if (!parseTexture(bytes, out, error))
    {
        error = std::format("{}: {}", core::pathToUtf8(path), error);
        return false;
    }
    return true;
}

bool serializeTexture(const TextureData& t, std::vector<std::byte>& out, std::string& error)
{
    if (t.mips.empty() || t.mips.size() > kMaxMips)
        return error = "texture needs 1..15 mips", false;
    detail::ByteWriter w;
    w.beginFile(kMagicTexture);
    w.putZeros(kInfoEnd - w.size());
    w.patch<std::uint32_t>(16, t.width());
    w.patch<std::uint32_t>(20, t.height());
    w.patch<std::uint32_t>(24, static_cast<std::uint32_t>(t.mips.size()));
    w.patch<std::uint32_t>(28, static_cast<std::uint32_t>(t.format));
    w.patch<std::uint32_t>(32, static_cast<std::uint32_t>(t.usage));
    w.patch<std::uint32_t>(36, static_cast<std::uint32_t>(w.size()));
    const std::size_t table = w.size();
    w.putZeros(t.mips.size() * 8);
    for (std::size_t i = 0; i < t.mips.size(); ++i)
    {
        w.align(16); // block rows start aligned (not required by readers)
        w.patch<std::uint32_t>(table + i * 8, static_cast<std::uint32_t>(w.size()));
        w.patch<std::uint32_t>(table + i * 8 + 4, static_cast<std::uint32_t>(t.mips[i].data.size()));
        w.putBytes(std::as_bytes(std::span(t.mips[i].data)));
    }
    w.align4();
    if (w.size() > kMaxFileSize)
        return error = "texture too large", false;
    out = w.finishFile();
    TextureData check;
    std::string why;
    if (!parseTexture(out, check, why))
    {
        error = "serialised texture is invalid: " + why;
        out.clear();
        return false;
    }
    return true;
}

std::string compiledTexturePath(std::string_view assetPath)
{
    std::string s(assetPath);
    const std::size_t slash = s.find_last_of('/');
    const std::size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        s.resize(dot);
    return s + ".vtex";
}

bool decodeTextureMip(const TextureData& t, std::size_t level, std::vector<std::uint8_t>& rgba, std::string& error)
{
    if (level >= t.mips.size())
        return error = "no such mip", false;
    const TextureMip& m = t.mips[level];
    rgba.assign(static_cast<std::size_t>(m.width) * m.height * 4, 255);
    if (t.format == TextureFormat::Rgba8)
    {
        rgba.assign(m.data.begin(), m.data.end());
        return true;
    }
    const std::uint32_t bw = std::max(1u, (m.width + 3) / 4);
    const std::uint32_t bh = std::max(1u, (m.height + 3) / 4);
    const std::uint32_t blockBytes = textureBlockBytes(t.format);
    for (std::uint32_t by = 0; by < bh; ++by)
    {
        for (std::uint32_t bx = 0; bx < bw; ++bx)
        {
            const std::uint8_t* block = m.data.data() + (static_cast<std::size_t>(by) * bw + bx) * blockBytes;
            std::uint8_t texels[16][4] = {};
            for (auto& tx : texels)
                tx[3] = 255;
            switch (t.format)
            {
            case TextureFormat::Bc1: decodeBc1(block, texels, false, true); break;
            case TextureFormat::Bc3:
            {
                std::uint8_t a[16];
                decodeBc4(block, a);
                decodeBc1(block + 8, texels, true, false);
                for (int i = 0; i < 16; ++i)
                    texels[i][3] = a[i];
                break;
            }
            case TextureFormat::Bc4:
            {
                std::uint8_t v[16];
                decodeBc4(block, v);
                for (int i = 0; i < 16; ++i)
                    texels[i][0] = texels[i][1] = texels[i][2] = v[i];
                break;
            }
            case TextureFormat::Bc5:
            {
                std::uint8_t x[16], y[16];
                decodeBc4(block, x);
                decodeBc4(block + 8, y);
                for (int i = 0; i < 16; ++i)
                {
                    texels[i][0] = x[i];
                    texels[i][1] = y[i];
                    texels[i][2] = 0;
                }
                break;
            }
            case TextureFormat::Rgba8: break;
            }
            for (std::uint32_t i = 0; i < 16; ++i)
            {
                const std::uint32_t x = bx * 4 + (i % 4), y = by * 4 + (i / 4);
                if (x >= m.width || y >= m.height)
                    continue;
                std::memcpy(rgba.data() + (static_cast<std::size_t>(y) * m.width + x) * 4, texels[i], 4);
            }
        }
    }
    return true;
}

} // namespace vo::assets
