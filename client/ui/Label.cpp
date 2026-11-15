#include "client/ui/Label.h"

#include "client/ui/DrawContext.h"
#include "client/ui/UiContext.h"

namespace vo::client::ui {

Label::Label(std::string text, TextRole role) : m_text(std::move(text)), m_role(role)
{
    setMouseTransparent(true);
}

Label& Label::setText(std::string_view text)
{
    if (m_text != text)
    {
        m_text.assign(text);
        m_linesValid = false;
    }
    return *this;
}

Label& Label::setRole(TextRole role)
{
    m_role = role;
    m_linesValid = false;
    return *this;
}

Label& Label::setFontSize(float logicalSize)
{
    m_fontSize = logicalSize;
    m_linesValid = false;
    return *this;
}

Label& Label::setWeight(render::FontWeight weight)
{
    m_weight = weight;
    m_linesValid = false;
    return *this;
}

Label& Label::setColor(const Color& color)
{
    m_color = color;
    return *this;
}

Label& Label::setAlign(render::HAlign h, render::VAlign v)
{
    m_hAlign = h;
    m_vAlign = v;
    return *this;
}

Label& Label::setWrap(bool wrap)
{
    m_wrap = wrap;
    m_linesValid = false;
    return *this;
}

Label& Label::setShadow(bool shadow)
{
    m_shadow = shadow;
    return *this;
}

Label& Label::setOutline(bool outline)
{
    m_outline = outline;
    return *this;
}

float Label::logicalFontSize(const Theme& theme) const noexcept
{
    return m_fontSize > 0.0f ? m_fontSize : theme.fontSize(m_role);
}

float Label::measureHeight(float width) const
{
    UiContext* ui = context();
    if (ui == nullptr)
        return 0.0f;
    const render::Font& font = ui->fonts().get(m_weight, logicalFontSize(ui->theme()) * ui->scale());
    std::vector<TextLine> lines;
    wrapText(font, m_text, m_wrap ? width : 0.0f, lines);
    return font.lineHeight() * static_cast<float>(lines.size());
}

void Label::drawSelf(DrawContext& ctx)
{
    if (m_text.empty())
        return;
    const render::Font& font = ctx.font(logicalFontSize(ctx.theme), m_weight);
    const Rect r = rect();
    const float wrapWidth = m_wrap ? r.w : 0.0f;
    if (!m_linesValid || m_linesWidth != wrapWidth || m_linesFontPx != font.pixelSize())
    {
        wrapText(font, m_text, wrapWidth, m_lines);
        m_linesValid = true;
        m_linesWidth = wrapWidth;
        m_linesFontPx = font.pixelSize();
    }

    render::TextStyle style;
    style.color = m_color.value_or(ctx.theme.text);
    if (!isEnabled())
        style.color = ctx.theme.textDisabled;
    style.shadow = m_shadow;
    style.shadowColor = ctx.theme.textShadow;
    style.shadowOffset = std::max(1.0f, std::round(ctx.scale * (font.pixelSize() >= 28 ? 2.0f : 1.0f)));
    style.outline = m_outline;
    render::drawTextLines(ctx.batch, font, m_text, m_lines, r, m_hAlign, m_vAlign, style);
}

} // namespace vo::client::ui
