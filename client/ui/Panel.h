// Container with a background: a framed window (optional title bar + gold ornaments), an inset
// well, or a plain flat rect with border. Uses Theme::panelSkin (9-slice) for the Window style if
// the theme provides a texture, otherwise draws procedurally.
#pragma once

#include "client/ui/Widget.h"

#include <string>

namespace vo::client::ui {

class Panel : public Widget
{
public:
    enum class Style : std::uint8_t
    {
        Window, // gradient slate, bronze border, title bar if titled, corner ornaments
        Inset,  // dark recessed well
        Plain,  // flat fill + 1px border
        None,   // invisible container (layout only)
    };

    explicit Panel(Style style = Style::Window, std::string title = {});

    Panel& setTitle(std::string title);
    Panel& setStyle(Style style);
    [[nodiscard]] const std::string& title() const noexcept { return m_title; }

    [[nodiscard]] Rect contentRect() const override;
    void drawSelf(DrawContext& ctx) override;

private:
    [[nodiscard]] float titleBarHeight() const noexcept;

    Style m_style;
    std::string m_title;
};

} // namespace vo::client::ui
