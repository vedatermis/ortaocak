// Batched 2D renderer: quads in pixel coordinates (top-left origin, y down) for images, atlas
// sub-rects, glyphs, solid/gradient/rounded rects, 9-slice panels and anti-aliased lines, with a
// scissor clip stack. Everything is premultiplied-alpha blended into the sRGB back buffer.
//
// Quads are recorded between begin() and end(); end() uploads them with one WRITE_DISCARD map and
// issues one DrawIndexed per run of quads sharing texture + clip rect. Shape quads (rects, lines)
// do not sample their texture, so they join whatever run is current and never break a batch.
// CPU arrays and GPU buffers only grow; the steady state does not allocate.
#pragma once

#include "client/core/Geometry.h"
#include "client/render/Shader.h"
#include "client/render/Texture2D.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vo::client::render {

class RenderStates;

struct SpriteVertex
{
    float x, y;
    float u, v;
    std::uint32_t color; // straight sRGB RGBA8
    float shapeX, shapeY, halfW, halfH;
    float radius, border, mode, unused;
};
static_assert(sizeof(SpriteVertex) == 52);

class SpriteBatch
{
public:
    struct Stats
    {
        std::uint32_t drawCalls = 0;
        std::uint32_t quads = 0;
    };

    SpriteBatch() = default;
    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    bool init(ID3D11Device* device, ID3D11DeviceContext* context, const RenderStates& states,
              std::string& error);
    void shutdown() noexcept;

    // Binds the sprite pipeline for a render target of the given size. The clip stack starts with
    // the full target.
    void begin(std::uint32_t targetWidth, std::uint32_t targetHeight);
    void end();

    // ---- Images ----
    void drawImage(const Texture2D& texture, const Rect& dst, const Color& tint = colors::kWhite);
    // `src` in texture pixels (atlas sub-rect).
    void drawImageRegion(const Texture2D& texture, const Rect& dst, const Rect& src,
                         const Color& tint = colors::kWhite);
    // 9-slice: `srcBorder` in texture pixels, `dstBorder` in screen pixels; centre and edges stretch.
    void drawNineSlice(const Texture2D& texture, const Rect& dst, const Insets& srcBorder,
                       const Insets& dstBorder, const Color& tint = colors::kWhite);
    // Low level: textured quad with explicit UVs; `glyph` = sample the R channel as coverage.
    void drawQuad(ID3D11ShaderResourceView* srv, const Rect& dst, Vec2 uv0, Vec2 uv1, const Color& color,
                  bool glyph);
    // As drawQuad with the top edge moved right by `topShift` and the bottom edge by `bottomShift`
    // pixels (a sheared quad: synthetic italics).
    void drawQuadSkewed(ID3D11ShaderResourceView* srv, const Rect& dst, Vec2 uv0, Vec2 uv1, const Color& color,
                        bool glyph, float topShift, float bottomShift);

    // ---- Shapes (anti-aliased signed-distance quads) ----
    void fillRect(const Rect& rect, const Color& color);
    void fillRectGradient(const Rect& rect, const Color& top, const Color& bottom);
    void fillRoundedRect(const Rect& rect, float radius, const Color& color);
    void fillRoundedRectGradient(const Rect& rect, float radius, const Color& top, const Color& bottom);
    // Border drawn inside `rect`.
    void strokeRect(const Rect& rect, float thickness, const Color& color);
    void strokeRoundedRect(const Rect& rect, float radius, float thickness, const Color& color);
    void drawLine(Vec2 a, Vec2 b, float thickness, const Color& color, bool roundCaps = false);
    // Square rotated 45 degrees (ornaments).
    void fillDiamond(Vec2 center, float halfDiagonal, const Color& color);

    // ---- Clipping ----
    void pushClip(const Rect& rect); // intersected with the current clip
    void popClip();
    [[nodiscard]] Rect currentClip() const noexcept;

    [[nodiscard]] const Texture2D& whiteTexture() const noexcept { return m_white; }
    // Totals since the last call.
    Stats takeStats() noexcept;

private:
    struct Run
    {
        ID3D11ShaderResourceView* srv; // nullptr = only shapes so far (any texture will do)
        D3D11_RECT scissor;
        std::uint32_t firstQuad;
        std::uint32_t quadCount;
    };

    bool ensureCapacity(std::size_t quads);
    // Returns a pointer to 4 vertices to fill, or nullptr if the quad is fully clipped.
    SpriteVertex* allocQuad(ID3D11ShaderResourceView* srv, float minX, float minY, float maxX, float maxY);
    void addShape(const Rect& rect, float radius, float border, const Color& top, const Color& bottom);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    const RenderStates* m_states = nullptr;
    Pipeline m_pipeline;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Texture2D m_white;
    std::size_t m_gpuQuadCapacity = 0;

    std::vector<SpriteVertex> m_vertices;
    std::vector<Run> m_runs;
    std::vector<D3D11_RECT> m_clipStack;
    std::uint32_t m_targetWidth = 0;
    std::uint32_t m_targetHeight = 0;
    std::uint32_t m_constantsWidth = 0;
    std::uint32_t m_constantsHeight = 0;
    bool m_inBatch = false;
    Stats m_stats;
};

} // namespace vo::client::render
