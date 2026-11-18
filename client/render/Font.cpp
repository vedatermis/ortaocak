#include "client/render/Font.h"

#include "client/core/Utf8.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"

#include <Windows.h>

#include <cmath>
#include <format>
#include <fstream>
#include <stb_rect_pack.h>
#include <stb_truetype.h>

namespace vo::client::render {

struct FontFile::Impl
{
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

namespace {

// Code points baked into every atlas (besides the direct Latin range 0x20..0x17F).
constexpr char32_t kExtraCodePoints[] = {
    0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, // hyphens and dashes
    0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, // quotes
    0x2020, 0x2021, 0x2022, 0x2026, 0x2030, 0x2039,
    0x203A, // dagger, bullet, ellipsis, per mille, angle quotes
    0x20AC, 0x20BA, 0x2122, 0x2190, 0x2191, 0x2192,
    0x2193, // euro, lira, trademark, arrows
    0x25B2, 0x25BC, 0x25CF, 0x2713, 0xFFFD,
};

std::filesystem::path windowsFontsDir()
{
    wchar_t buffer[MAX_PATH] = {};
    const UINT len = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    std::filesystem::path dir =
        (len > 0 && len < MAX_PATH) ? std::filesystem::path(buffer) : std::filesystem::path(L"C:\\Windows");
    return dir / L"Fonts";
}

} // namespace

FontFile::FontFile() : m_impl(std::make_unique<Impl>())
{
}
FontFile::~FontFile() = default;

bool FontFile::load(const std::filesystem::path& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        error = "cannot open font " + vo::core::pathToUtf8(path);
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > (std::streamoff{64} << 20))
    {
        error = "bad font file size: " + vo::core::pathToUtf8(path);
        return false;
    }
    m_impl->data.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(m_impl->data.data()), size))
    {
        error = "font read failed: " + vo::core::pathToUtf8(path);
        return false;
    }
    const int offset = stbtt_GetFontOffsetForIndex(m_impl->data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&m_impl->info, m_impl->data.data(), offset))
    {
        error = "not a usable TrueType font: " + vo::core::pathToUtf8(path);
        return false;
    }
    m_path = path;
    return true;
}

bool Font::bake(ID3D11Device* device, const FontFile& file, int pixelSize, const char* debugName,
                std::string& error)
{
    const stbtt_fontinfo& info = file.impl().info;
    m_file = &file;
    m_pixelSize = pixelSize;
    m_scale = stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(pixelSize));

    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    m_ascent = std::round(static_cast<float>(ascent) * m_scale);
    m_descent = std::round(static_cast<float>(-descent) * m_scale);
    m_lineHeight = std::round(static_cast<float>(ascent - descent + lineGap) * m_scale);
    m_hasKerning = stbtt_GetKerningTableLength(&info) > 0 || info.gpos != 0;

    // Collect the code points this face actually has.
    std::vector<int> codePoints;
    codePoints.reserve(kDirectRange + std::size(kExtraCodePoints));
    for (char32_t cp = 0x20; cp < kDirectRange; ++cp)
    {
        if ((cp >= 0x7F && cp < 0xA0))
            continue;
        if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0)
            codePoints.push_back(static_cast<int>(cp));
    }
    for (const char32_t cp : kExtraCodePoints)
    {
        if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0)
            codePoints.push_back(static_cast<int>(cp));
    }

    std::vector<stbtt_packedchar> packed(codePoints.size());
    stbtt_pack_range range{};
    range.font_size = STBTT_POINT_SIZE(static_cast<float>(pixelSize));
    range.array_of_unicode_codepoints = codePoints.data();
    range.num_chars = static_cast<int>(codePoints.size());
    range.chardata_for_range = packed.data();

    // Grow the atlas until everything fits.
    std::vector<unsigned char> pixels;
    int atlasW = 256;
    int atlasH = 256;
    bool ok = false;
    while (!ok && atlasW <= 4096)
    {
        pixels.assign(static_cast<std::size_t>(atlasW) * static_cast<std::size_t>(atlasH), 0);
        stbtt_pack_context pc{};
        if (!stbtt_PackBegin(&pc, pixels.data(), atlasW, atlasH, 0, 1, nullptr))
            break;
        stbtt_PackSetOversampling(&pc, 1, 1);
        ok = stbtt_PackFontRanges(&pc, file.impl().data.data(), 0, &range, 1) != 0;
        stbtt_PackEnd(&pc);
        if (!ok)
        {
            if (atlasH < atlasW)
                atlasH *= 2;
            else
                atlasW *= 2;
        }
    }
    if (!ok)
    {
        error = std::format("font atlas for {} px does not fit in 4096x4096", pixelSize);
        return false;
    }
    if (!m_atlas.createR8(device, static_cast<std::uint32_t>(atlasW), static_cast<std::uint32_t>(atlasH),
                          pixels.data(), debugName, error))
        return false;

    const float iw = 1.0f / static_cast<float>(atlasW);
    const float ih = 1.0f / static_cast<float>(atlasH);
    m_extra.clear();
    m_hasDirect.fill(false);
    for (std::size_t i = 0; i < codePoints.size(); ++i)
    {
        const stbtt_packedchar& p = packed[i];
        Glyph g;
        g.x0 = std::round(p.xoff);
        g.y0 = std::round(p.yoff);
        g.x1 = g.x0 + static_cast<float>(p.x1 - p.x0);
        g.y1 = g.y0 + static_cast<float>(p.y1 - p.y0);
        g.u0 = static_cast<float>(p.x0) * iw;
        g.v0 = static_cast<float>(p.y0) * ih;
        g.u1 = static_cast<float>(p.x1) * iw;
        g.v1 = static_cast<float>(p.y1) * ih;
        g.advance = p.xadvance;
        g.visible = p.x1 > p.x0 && p.y1 > p.y0;
        const auto cp = static_cast<char32_t>(codePoints[i]);
        g.glyphIndex = stbtt_FindGlyphIndex(&info, codePoints[i]);
        if (cp < kDirectRange)
        {
            m_direct[cp] = g;
            m_hasDirect[cp] = true;
        }
        else
        {
            m_extra.emplace(cp, g);
        }
    }
    m_glyphCount = codePoints.size();
    if (const auto it = m_extra.find(utf8::kReplacement); it != m_extra.end())
        m_fallback = it->second;
    else
        m_fallback = m_direct['?'];
    return true;
}

bool Font::hasGlyph(char32_t cp) const noexcept
{
    if (cp < kDirectRange)
        return m_hasDirect[cp];
    return m_extra.find(cp) != m_extra.end();
}

const Glyph& Font::glyph(char32_t cp) const noexcept
{
    if (cp < kDirectRange)
        return m_hasDirect[cp] ? m_direct[cp] : m_fallback;
    const auto it = m_extra.find(cp);
    return it != m_extra.end() ? it->second : m_fallback;
}

float Font::kerning(char32_t left, char32_t right) const
{
    if (!m_hasKerning || m_file == nullptr)
        return 0.0f;
    const int a = glyph(left).glyphIndex;
    const int b = glyph(right).glyphIndex;
    if (a == 0 || b == 0)
        return 0.0f;
    return static_cast<float>(stbtt_GetGlyphKernAdvance(&m_file->impl().info, a, b)) * m_scale;
}

bool FontLibrary::init(ID3D11Device* device, std::string& error)
{
    m_device = device;
    const std::filesystem::path dir = windowsFontsDir();
    const wchar_t* regular[] = {L"segoeui.ttf", L"arial.ttf", L"tahoma.ttf"};
    const wchar_t* bold[] = {L"segoeuib.ttf", L"arialbd.ttf", L"tahomabd.ttf"};
    const wchar_t* italic[] = {L"segoeuii.ttf", L"ariali.ttf"}; // (Tahoma has none: drawn slanted then)

    auto loadFirst = [&](const auto& names) -> std::unique_ptr<FontFile>
    {
        for (const wchar_t* name : names)
        {
            auto file = std::make_unique<FontFile>();
            std::string err;
            if (file->load(dir / name, err))
                return file;
            VO_LOG_DEBUG("font candidate skipped: {}", err);
        }
        return nullptr;
    };
    m_files[0] = loadFirst(regular);
    if (!m_files[0])
    {
        error = "no usable UI font (Segoe UI, Arial, Tahoma) in " + vo::core::pathToUtf8(dir);
        return false;
    }
    m_files[1] = loadFirst(bold);
    m_files[2] = loadFirst(italic);
    VO_LOG_INFO("fonts: regular {}, bold {}, italic {}", vo::core::pathToUtf8(m_files[0]->path()),
                m_files[1] ? vo::core::pathToUtf8(m_files[1]->path()) : std::string("(regular)"),
                m_files[2] ? vo::core::pathToUtf8(m_files[2]->path()) : std::string("(regular, slanted)"));

    auto font = std::make_unique<Font>();
    if (!font->bake(device, *m_files[0], 16, "font.regular.16", error))
        return false;
    m_default = font.get();
    m_fonts.emplace(std::make_pair(0, 16), std::move(font));
    return true;
}

void FontLibrary::shutdown() noexcept
{
    m_default = nullptr;
    m_fonts.clear();
    for (auto& f : m_files)
        f.reset();
    m_device = nullptr;
}

const Font& FontLibrary::get(FontWeight weight, int pixelSize)
{
    pixelSize = pixelSize < 6 ? 6 : (pixelSize > 160 ? 160 : pixelSize);
    int w = static_cast<int>(weight);
    if (!m_files[static_cast<std::size_t>(w)])
        w = 0;
    const auto key = std::make_pair(w, pixelSize);
    if (const auto it = m_fonts.find(key); it != m_fonts.end())
        return *it->second;

    auto font = std::make_unique<Font>();
    std::string error;
    const std::string name = std::format("font.{}.{}", w == 0 ? "regular" : (w == 1 ? "bold" : "italic"), pixelSize);
    if (!font->bake(m_device, *m_files[static_cast<std::size_t>(w)], pixelSize, name.c_str(), error))
    {
        VO_LOG_ERROR("font bake failed ({}): {}", name, error);
        return *m_default;
    }
    VO_LOG_DEBUG("baked {}: {} glyphs, atlas {}x{}", name, font->glyphCount(), font->atlas().width(),
                 font->atlas().height());
    const Font& ref = *font;
    m_fonts.emplace(key, std::move(font));
    return ref;
}

} // namespace vo::client::render
