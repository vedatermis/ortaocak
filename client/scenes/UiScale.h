// UI scale fitting for the full-screen 2D scenes. The login/character screens are laid out for a
// logical canvas of at least kMinLogicalWidth x kMinLogicalHeight; if the window (in physical
// pixels) divided by the requested UI scale would be smaller than that (e.g. 1024x640 at 150%),
// the scale is reduced just enough to fit, so nothing overlaps or gets clipped. It never goes
// below kMinFitScale, and never above the requested scale.
#pragma once

#include <algorithm>

namespace vo::client::scenes {

inline constexpr float kMinLogicalWidth = 960.0f;
inline constexpr float kMinLogicalHeight = 600.0f;
inline constexpr float kMinFitScale = 0.5f;

[[nodiscard]] constexpr float fitUiScale(float widthPx, float heightPx, float requestedScale) noexcept
{
    if (widthPx <= 0.0f || heightPx <= 0.0f || requestedScale <= 0.0f)
        return requestedScale;
    const float fit = std::min(widthPx / kMinLogicalWidth, heightPx / kMinLogicalHeight);
    return std::max(kMinFitScale, std::min(requestedScale, fit));
}

} // namespace vo::client::scenes
