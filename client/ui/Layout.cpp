#include "client/ui/Layout.h"

#include <cmath>

namespace vo::client::ui {
namespace {

struct Span1D
{
    float begin;
    float end;
};

// One axis of computeAnchoredRect. Edges are rounded independently so adjacent widgets share edges.
Span1D anchorAxis(float parentBegin, float parentEnd, bool anchorBegin, bool anchorEnd, bool center,
                  float marginBegin, float marginEnd, float size, float scale) noexcept
{
    const float s = std::round(size * scale);
    if (anchorBegin && anchorEnd)
    {
        const float b = std::round(parentBegin + marginBegin * scale);
        const float e = std::round(parentEnd - marginEnd * scale);
        return {b, e < b ? b : e};
    }
    if (anchorEnd)
    {
        const float e = std::round(parentEnd - marginEnd * scale);
        return {e - s, e};
    }
    if (center)
    {
        const float mid = (parentBegin + parentEnd) * 0.5f + (marginBegin - marginEnd) * scale * 0.5f;
        const float b = std::round(mid - s * 0.5f);
        return {b, b + s};
    }
    const float b = std::round(parentBegin + marginBegin * scale);
    return {b, b + s};
}

} // namespace

Rect computeAnchoredRect(const Rect& parent, const LayoutSpec& spec, float scale) noexcept
{
    const std::uint8_t a = spec.anchors;
    const Span1D h =
        anchorAxis(parent.left(), parent.right(), (a & anchor::kLeft) != 0, (a & anchor::kRight) != 0,
                   (a & anchor::kCenterX) != 0, spec.margins.left, spec.margins.right, spec.size.x, scale);
    const Span1D v =
        anchorAxis(parent.top(), parent.bottom(), (a & anchor::kTop) != 0, (a & anchor::kBottom) != 0,
                   (a & anchor::kCenterY) != 0, spec.margins.top, spec.margins.bottom, spec.size.y, scale);
    return Rect::fromEdges(h.begin, v.begin, h.end, v.end);
}

void stackLayout(const Rect& content, Orientation orientation, float spacing, float scale,
                 std::span<const StackItem> items, std::span<Rect> out) noexcept
{
    const bool vertical = orientation == Orientation::Vertical;
    const float mainBegin = vertical ? content.top() : content.left();
    const float mainExtent = vertical ? content.h : content.w;
    const float crossBegin = vertical ? content.left() : content.top();
    const float crossExtent = vertical ? content.w : content.h;
    const float gap = spacing * scale;

    const std::size_t n = items.size() < out.size() ? items.size() : out.size();
    float fixed = 0.0f;
    std::size_t visibleCount = 0;
    std::size_t flexCount = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (!items[i].visible)
            continue;
        ++visibleCount;
        const float m = vertical ? items[i].size.y : items[i].size.x;
        if (m <= 0.0f)
            ++flexCount;
        else
            fixed += m * scale;
    }
    const float gaps = visibleCount > 1 ? gap * static_cast<float>(visibleCount - 1) : 0.0f;
    float flexEach = 0.0f;
    if (flexCount > 0)
    {
        const float leftover = mainExtent - fixed - gaps;
        flexEach = leftover > 0.0f ? leftover / static_cast<float>(flexCount) : 0.0f;
    }

    float pen = mainBegin;
    bool first = true;
    for (std::size_t i = 0; i < n; ++i)
    {
        const StackItem& item = items[i];
        if (!item.visible)
        {
            const float p = std::round(pen);
            out[i] = vertical ? Rect{std::round(crossBegin), p, 0.0f, 0.0f}
                              : Rect{p, std::round(crossBegin), 0.0f, 0.0f};
            continue;
        }
        if (!first)
            pen += gap;
        first = false;

        const float m = vertical ? item.size.y : item.size.x;
        const float mainSize = m <= 0.0f ? flexEach : m * scale;
        const float mb = std::round(pen);
        const float me = std::round(pen + mainSize);
        pen += mainSize;

        const float c = vertical ? item.size.x : item.size.y;
        const float crossSize = std::round(c * scale);
        float cb = crossBegin;
        float ce = crossBegin + crossExtent;
        switch (item.align)
        {
        case CrossAlign::Stretch:
            break;
        case CrossAlign::Start:
            ce = cb + crossSize;
            break;
        case CrossAlign::Center:
            cb = crossBegin + (crossExtent - crossSize) * 0.5f;
            ce = cb + crossSize;
            break;
        case CrossAlign::End:
            cb = ce - crossSize;
            break;
        }
        cb = std::round(cb);
        ce = std::round(ce);
        out[i] = vertical ? Rect::fromEdges(cb, mb, ce, me) : Rect::fromEdges(mb, cb, me, ce);
    }
}

} // namespace vo::client::ui
