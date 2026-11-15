#include "client/scenes/Backdrop.h"

#include <cmath>

namespace vo::client::scenes {

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

    // Thin gold rule with ornaments near the top.
    const float y = std::round(64.0f * scale) + 0.5f;
    const float margin = 40.0f * scale;
    const Color gold = theme.ornament.withAlpha(0.35f);
    batch.drawLine({margin, y}, {width * 0.5f - 14.0f * scale, y}, std::max(1.0f, scale), gold);
    batch.drawLine({width * 0.5f + 14.0f * scale, y}, {width - margin, y}, std::max(1.0f, scale), gold);
    batch.fillDiamond({width * 0.5f, y}, 5.0f * scale, theme.ornament.withAlpha(0.6f));
}

} // namespace vo::client::scenes
