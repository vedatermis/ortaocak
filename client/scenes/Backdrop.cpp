#include "client/scenes/Backdrop.h"

#include "client/render/Texture2D.h"

#include <algorithm>
#include <cmath>

namespace vo::client::scenes {

namespace {

void drawTopRule(render::SpriteBatch& batch, const ui::Theme& theme, float width, float scale)
{
    // Thin gold rule with ornaments near the top.
    const float y = std::round(64.0f * scale) + 0.5f;
    const float margin = 40.0f * scale;
    const Color gold = theme.ornament.withAlpha(0.35f);
    batch.drawLine({margin, y}, {width * 0.5f - 14.0f * scale, y}, std::max(1.0f, scale), gold);
    batch.drawLine({width * 0.5f + 14.0f * scale, y}, {width - margin, y}, std::max(1.0f, scale), gold);
    batch.fillDiamond({width * 0.5f, y}, 5.0f * scale, theme.ornament.withAlpha(0.6f));
}

} // namespace

void drawBackdrop(render::SpriteBatch& batch, const ui::Theme& theme, float width, float height, float scale)
{
    const Rect full{0.0f, 0.0f, width, height};
    batch.fillRectGradient(full, theme.backgroundTop, theme.backgroundBottom);

    // Faint warm horizon band behind the middle of the screen.
    const float glowTop = std::round(height * 0.25f);
    const float glowMid = std::round(height * 0.5f);
    const float glowBottom = std::round(height * 0.75f);
    batch.fillRectGradient({0.0f, glowTop, width, glowMid - glowTop}, Color::hex(0x6B5226, 0.0f),
                           Color::hex(0x6B5226, 0.10f));
    batch.fillRectGradient({0.0f, glowMid, width, glowBottom - glowMid}, Color::hex(0x6B5226, 0.10f),
                           Color::hex(0x6B5226, 0.0f));

    // Top and bottom vignette bands.
    const float band = std::round(height * 0.18f);
    batch.fillRectGradient({0.0f, 0.0f, width, band}, Color::hex(0x000000, 0.45f),
                           Color::hex(0x000000, 0.0f));
    batch.fillRectGradient({0.0f, height - band, width, band}, Color::hex(0x000000, 0.0f),
                           Color::hex(0x000000, 0.55f));

    drawTopRule(batch, theme, width, scale);
}

void drawSceneBackground(render::SpriteBatch& batch, const ui::Theme& theme, const render::Texture2D* image,
                         float width, float height, float scale)
{
    if (image == nullptr || !image->valid() || width <= 0.0f || height <= 0.0f)
    {
        drawBackdrop(batch, theme, width, height, scale);
        return;
    }

    // Cover-fit: keep the aspect ratio, fill the screen, crop the overflow (centred).
    const float iw = static_cast<float>(image->width());
    const float ih = static_cast<float>(image->height());
    Rect src{0.0f, 0.0f, iw, ih};
    const float screenAspect = width / height;
    if (iw / ih > screenAspect)
    {
        const float w = ih * screenAspect;
        src = {(iw - w) * 0.5f, 0.0f, w, ih};
    }
    else
    {
        const float h = iw / screenAspect;
        src = {0.0f, (ih - h) * 0.5f, iw, h};
    }
    const Rect full{0.0f, 0.0f, width, height};
    batch.fillRect(full, colors::kBlack);
    batch.drawImageRegion(*image, full, src);

    // Dark vignette: an overall dim plus darker bands towards every edge (horizontal bands are built
    // from strips, the batch only has vertical gradients).
    batch.fillRect(full, Color::hex(0x000000, 0.30f));
    const float band = std::round(height * 0.22f);
    batch.fillRectGradient({0.0f, 0.0f, width, band}, Color::hex(0x000000, 0.55f), Color::hex(0x000000, 0.0f));
    batch.fillRectGradient({0.0f, height - band, width, band}, Color::hex(0x000000, 0.0f),
                           Color::hex(0x000000, 0.65f));
    constexpr int kStrips = 12;
    const float side = std::round(width * 0.16f);
    const float strip = std::ceil(side / static_cast<float>(kStrips));
    for (int i = 0; i < kStrips; ++i)
    {
        const float t = 1.0f - static_cast<float>(i) / static_cast<float>(kStrips);
        const Color c = Color::hex(0x000000, 0.45f * t * t);
        const float x = static_cast<float>(i) * strip;
        batch.fillRect({x, 0.0f, strip, height}, c);
        batch.fillRect({width - x - strip, 0.0f, strip, height}, c);
    }

    drawTopRule(batch, theme, width, scale);
}

} // namespace vo::client::scenes
