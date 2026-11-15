// Displays a texture (or an atlas sub-rect of it), stretched or aspect-fitted into its rect.
#pragma once

#include "client/ui/Widget.h"

#include <optional>

namespace vo::client::render {
class Texture2D;
}

namespace vo::client::ui {

class ImageWidget : public Widget
{
public:
    enum class Fit : std::uint8_t
    {
        Stretch,
        Contain, // keep aspect, letterbox inside the rect
        Cover,   // keep aspect, fill the rect (cropped via UVs)
    };

    explicit ImageWidget(const render::Texture2D* texture = nullptr);

    ImageWidget& setTexture(const render::Texture2D* texture);
    ImageWidget& setSourceRect(std::optional<Rect> src); // texture pixels
    ImageWidget& setTint(const Color& tint);
    ImageWidget& setFit(Fit fit);

    void drawSelf(DrawContext& ctx) override;

private:
    const render::Texture2D* m_texture;
    std::optional<Rect> m_source;
    Color m_tint = colors::kWhite;
    Fit m_fit = Fit::Contain;
};

} // namespace vo::client::ui
