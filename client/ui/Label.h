// Static text: single line or word-wrapped paragraph, aligned inside its rect, optional shadow or
// outline. Wrapped lines are cached and recomputed only when text, width or font size change.
#pragma once

#include "client/render/Font.h"
#include "client/render/TextRenderer.h"
#include "client/ui/TextLayout.h"
#include "client/ui/Theme.h"
#include "client/ui/Widget.h"

#include <optional>
#include <string>
#include <vector>

namespace vo::client::ui {

class Label : public Widget
{
public:
    explicit Label(std::string text = {}, TextRole role = TextRole::Normal);

    Label& setText(std::string_view text);
    Label& setRole(TextRole role);
    Label& setFontSize(float logicalSize); // overrides the role's size
    Label& setWeight(render::FontWeight weight);
    Label& setColor(const Color& color); // default: theme text (dimmed when disabled)
    Label& setAlign(render::HAlign h, render::VAlign v = render::VAlign::Middle);
    Label& setWrap(bool wrap);
    Label& setShadow(bool shadow);
    Label& setOutline(bool outline);
    // Italic (fix1): the italic face when the system has one, else the regular face slanted. Not
    // combined with the bold weight.
    Label& setItalic(bool italic);
    [[nodiscard]] bool italic() const noexcept { return m_italic; }

    [[nodiscard]] const std::string& text() const noexcept { return m_text; }

    // Height needed to show all lines at `width` physical pixels (for layouts that size to text).
    [[nodiscard]] float measureHeight(float width) const;

    void drawSelf(DrawContext& ctx) override;

private:
    [[nodiscard]] float logicalFontSize(const Theme& theme) const noexcept;

    std::string m_text;
    TextRole m_role;
    float m_fontSize = 0.0f;
    render::FontWeight m_weight = render::FontWeight::Regular;
    std::optional<Color> m_color;
    render::HAlign m_hAlign = render::HAlign::Left;
    render::VAlign m_vAlign = render::VAlign::Middle;
    bool m_wrap = false;
    bool m_shadow = false;
    bool m_outline = false;
    bool m_italic = false;

    [[nodiscard]] render::FontWeight drawWeight(const render::FontLibrary& fonts) const noexcept;

    // Line cache.
    std::vector<TextLine> m_lines;
    bool m_linesValid = false;
    float m_linesWidth = -1.0f;
    int m_linesFontPx = 0;
};

} // namespace vo::client::ui
