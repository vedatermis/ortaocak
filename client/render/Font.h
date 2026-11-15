// TrueType fonts baked into glyph atlases with stb_truetype.
//
// FontFile holds the TTF bytes; Font is one face at one pixel size (em size in pixels) with an R8
// coverage atlas covering ASCII, Latin-1, Latin Extended-A (all Turkish letters) and common
// punctuation. Unknown code points render as U+FFFD (or '?' if the face lacks it). FontLibrary
// finds the system UI font (Segoe UI, fallback Arial/Tahoma) and caches Fonts per weight + size.
// Glyphs are rasterised without oversampling and positioned on whole pixels (crisp, no blur).
#pragma once

#include "client/render/Texture2D.h"
#include "client/ui/TextLayout.h"

#include <d3d11.h>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vo::client::render {

class FontFile
{
public:
    FontFile();
    ~FontFile();
    FontFile(const FontFile&) = delete;
    FontFile& operator=(const FontFile&) = delete;

    bool load(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    struct Impl;
    [[nodiscard]] const Impl& impl() const noexcept { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
    std::filesystem::path m_path;
};

struct Glyph
{
    // Quad relative to the pen position on the baseline (whole pixels).
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    float advance = 0.0f;
    int glyphIndex = 0;
    bool visible = false; // has pixels (space does not)
};

class Font final : public ui::GlyphMetrics
{
public:
    Font() = default;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    bool bake(ID3D11Device* device, const FontFile& file, int pixelSize, const char* debugName,
              std::string& error);

    // Never fails: unknown code points return the replacement glyph.
    [[nodiscard]] const Glyph& glyph(char32_t cp) const noexcept;
    [[nodiscard]] bool hasGlyph(char32_t cp) const noexcept;

    [[nodiscard]] float advance(char32_t cp) const override { return glyph(cp).advance; }
    [[nodiscard]] float kerning(char32_t left, char32_t right) const override;
    [[nodiscard]] float lineHeight() const override { return m_lineHeight; }

    [[nodiscard]] float ascent() const noexcept { return m_ascent; }   // whole pixels
    [[nodiscard]] float descent() const noexcept { return m_descent; } // whole pixels, positive
    [[nodiscard]] int pixelSize() const noexcept { return m_pixelSize; }
    [[nodiscard]] const Texture2D& atlas() const noexcept { return m_atlas; }
    [[nodiscard]] std::size_t glyphCount() const noexcept { return m_glyphCount; }

private:
    static constexpr char32_t kDirectRange = 0x180; // ASCII + Latin-1 + Latin Extended-A

    const FontFile* m_file = nullptr;
    float m_scale = 0.0f;
    float m_ascent = 0.0f;
    float m_descent = 0.0f;
    float m_lineHeight = 0.0f;
    int m_pixelSize = 0;
    bool m_hasKerning = false;
    std::size_t m_glyphCount = 0;
    Texture2D m_atlas;
    std::array<Glyph, kDirectRange> m_direct{};
    std::array<bool, kDirectRange> m_hasDirect{};
    std::unordered_map<char32_t, Glyph> m_extra;
    Glyph m_fallback;
};

enum class FontWeight : std::uint8_t
{
    Regular,
    Bold,
};

class FontLibrary
{
public:
    FontLibrary() = default;
    FontLibrary(const FontLibrary&) = delete;
    FontLibrary& operator=(const FontLibrary&) = delete;

    // Locates the font files in %WINDIR%\Fonts and bakes a default size. Fails only if no usable
    // font file exists at all.
    bool init(ID3D11Device* device, std::string& error);
    void shutdown() noexcept;

    // Bakes on first use (logged); later calls are a map look-up. Sizes are clamped to [6, 160].
    // If baking fails, the default font is returned (and the error logged once).
    [[nodiscard]] const Font& get(FontWeight weight, int pixelSize);
    [[nodiscard]] const Font& get(FontWeight weight, float pixelSize)
    {
        return get(weight, static_cast<int>(pixelSize + 0.5f));
    }

    [[nodiscard]] std::size_t bakedCount() const noexcept { return m_fonts.size(); }

private:
    ID3D11Device* m_device = nullptr;
    std::array<std::unique_ptr<FontFile>, 2> m_files;
    std::map<std::pair<int, int>, std::unique_ptr<Font>> m_fonts;
    const Font* m_default = nullptr;
};

} // namespace vo::client::render
