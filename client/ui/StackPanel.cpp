#include "client/ui/StackPanel.h"

namespace vo::client::ui {

StackPanel::StackPanel(Orientation orientation, float spacing)
    : m_orientation(orientation), m_spacing(spacing)
{
    setMouseTransparent(true);
}

StackPanel& StackPanel::setOrientation(Orientation orientation)
{
    m_orientation = orientation;
    markLayoutDirty();
    return *this;
}

StackPanel& StackPanel::setSpacing(float spacing)
{
    m_spacing = spacing;
    markLayoutDirty();
    return *this;
}

void StackPanel::layoutChildren(float scale)
{
    recordLayoutScale(scale);
    const auto& kids = children();
    m_items.resize(kids.size());
    m_rects.resize(kids.size());
    const bool vertical = m_orientation == Orientation::Vertical;
    for (std::size_t i = 0; i < kids.size(); ++i)
    {
        const LayoutSpec& spec = kids[i]->layoutSpec();
        StackItem& item = m_items[i];
        item.size = spec.size;
        item.visible = kids[i]->isVisible();
        const float cross = vertical ? spec.size.x : spec.size.y;
        const std::uint8_t a = spec.anchors;
        const bool center = (a & (vertical ? anchor::kCenterX : anchor::kCenterY)) != 0;
        const bool startEdge = (a & (vertical ? anchor::kLeft : anchor::kTop)) != 0;
        const bool endEdge = (a & (vertical ? anchor::kRight : anchor::kBottom)) != 0;
        if (cross <= 0.0f)
            item.align = CrossAlign::Stretch;
        else if (center)
            item.align = CrossAlign::Center;
        else if (endEdge && !startEdge)
            item.align = CrossAlign::End;
        else
            item.align = CrossAlign::Start;
    }
    stackLayout(contentRect(), m_orientation, m_spacing, scale, m_items, m_rects);
    for (std::size_t i = 0; i < kids.size(); ++i)
    {
        if (kids[i]->isVisible())
            placeChild(*kids[i], m_rects[i], scale);
    }
}

} // namespace vo::client::ui
