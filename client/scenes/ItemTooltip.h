// Item tooltips shared by the inventory window and the loot window (M10): the lines come from
// world::itemTipLines (pure, unit-tested); this file colours and draws them in a framed box next to
// an anchor rect, kept inside the screen.
#pragma once

#include "client/core/Geometry.h"
#include "client/world/LootView.h"

#include <string>
#include <vector>

namespace vo::client::ui {
struct DrawContext;
struct Theme;
} // namespace vo::client::ui

namespace vo::client::scenes {

struct ShownTipLine
{
    std::string text;
    Color color;
    bool title = false;
    bool wrap = false;
    bool unmet = false; // a requirement the character does not meet (automation text marks it)
};

// Colours of the tones (rarity colour for the title).
[[nodiscard]] std::vector<ShownTipLine> shownTipLines(const std::vector<world::TipLine>& lines, const ui::Theme& t);
// The text of a tooltip for automation: lines joined by '\n', unmet ones with " (karşılanmıyor)".
[[nodiscard]] std::string tooltipPlainText(const std::vector<ShownTipLine>& lines);
// Draws the box right of `anchor` (left of it if it does not fit), clamped to the viewport; M12:
// `above` centres it over the anchor instead (the skill bar at the bottom of the screen).
void drawTooltipBox(ui::DrawContext& ctx, const std::vector<ShownTipLine>& lines, const Rect& anchor, float viewportW,
                    float viewportH, bool above = false);

} // namespace vo::client::scenes
