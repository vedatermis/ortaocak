#include "client/ui/ImageWidget.h"

#include "client/render/Texture2D.h"
#include "client/ui/DrawContext.h"

#include <cmath>

namespace vo::client::ui {

ImageWidget::ImageWidget(const render::Texture2D* texture) : m_texture(texture)
{
    setMouseTransparent(true);
}

ImageWidget& ImageWidget::setTexture(const render::Texture2D* texture)
{
    m_texture = texture;
    return *this;
}

ImageWidget& ImageWidget::setSourceRect(std::optional<Rect> src)
{
    m_source = src;
    return *this;
}

ImageWidget& ImageWidget::setTint(const Color& tint)
{
    m_tint = tint;
    return *this;
}

ImageWidget& ImageWidget::setFit(Fit fit)
{
    m_fit = fit;
    return *this;
}

void ImageWidget::drawSelf(DrawContext& ctx)
{
    if (m_texture == nullptr || !m_texture->valid())
        return;
    const Rect r = rect();
    Rect src = m_source.value_or(
        Rect{0.0f, 0.0f, static_cast<float>(m_texture->width()), static_cast<float>(m_texture->height())});
    if (src.w <= 0.0f || src.h <= 0.0f || r.empty())
        return;

    Rect dst = r;
    const float srcAspect = src.w / src.h;
    const float dstAspect = r.w / r.h;
    if (m_fit == Fit::Contain)
    {
        if (srcAspect > dstAspect)
        {
            const float h = r.w / srcAspect;
            dst = Rect{r.x, r.y + (r.h - h) * 0.5f, r.w, h}.snapped();
        }
        else
        {
            const float w = r.h * srcAspect;
            dst = Rect{r.x + (r.w - w) * 0.5f, r.y, w, r.h}.snapped();
        }
    }
    else if (m_fit == Fit::Cover)
    {
        if (srcAspect > dstAspect)
        {
            const float w = src.h * dstAspect;
            src = Rect{src.x + (src.w - w) * 0.5f, src.y, w, src.h};
        }
        else
        {
            const float h = src.w / dstAspect;
            src = Rect{src.x, src.y + (src.h - h) * 0.5f, src.w, h};
        }
    }
    const Color tint = isEnabled() ? m_tint : m_tint.scaledAlpha(0.5f);
    ctx.batch.drawImageRegion(*m_texture, dst, src, tint);
}

} // namespace vo::client::ui
