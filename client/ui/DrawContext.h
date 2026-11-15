// Everything a widget needs to draw itself: the sprite batch, fonts, theme, UI scale and time.
#pragma once

#include "client/render/Font.h"
#include "client/render/SpriteBatch.h"
#include "client/render/TextRenderer.h"
#include "client/ui/Theme.h"

#include <cmath>

namespace vo::client::ui {

struct DrawContext
{
    render::SpriteBatch& batch;
    render::FontLibrary& fonts;
    const Theme& theme;
    float scale = 1.0f;
    double time = 0.0;

    // Font for a logical size at the current scale.
    [[nodiscard]] const render::Font& font(float logicalSize,
                                           render::FontWeight weight = render::FontWeight::Regular) const
    {
        return fonts.get(weight, logicalSize * scale);
    }
    [[nodiscard]] const render::Font& font(TextRole role,
                                           render::FontWeight weight = render::FontWeight::Regular) const
    {
        return font(theme.fontSize(role), weight);
    }
    // Logical -> physical, rounded to whole pixels (minimum 1 for non-zero input).
    [[nodiscard]] float px(float logical) const noexcept
    {
        const float v = std::round(logical * scale);
        return (logical > 0.0f && v < 1.0f) ? 1.0f : v;
    }
    // Logical -> physical without rounding (stroke widths, radii).
    [[nodiscard]] float pxf(float logical) const noexcept { return logical * scale; }
};

} // namespace vo::client::ui
