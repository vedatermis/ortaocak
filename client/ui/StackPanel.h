// Layout container that stacks its visible children vertically or horizontally.
//
// Main axis: a child's LayoutSpec size (logical) on that axis; <= 0 means flexible (shares the
// leftover space with the other flexible children). Cross axis: size <= 0 stretches; otherwise the
// child's anchors pick the alignment (centre flag -> centred, only the far edge -> end, else start).
#pragma once

#include "client/ui/Layout.h"
#include "client/ui/Widget.h"

#include <vector>

namespace vo::client::ui {

class StackPanel : public Widget
{
public:
    explicit StackPanel(Orientation orientation = Orientation::Vertical, float spacing = 8.0f);

    StackPanel& setOrientation(Orientation orientation);
    StackPanel& setSpacing(float spacing);

    void layoutChildren(float scale) override;

private:
    Orientation m_orientation;
    float m_spacing;
    std::vector<StackItem> m_items;
    std::vector<Rect> m_rects;
};

} // namespace vo::client::ui
