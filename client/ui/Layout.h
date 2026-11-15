// Layout math for the UI toolkit: anchoring a rect to its parent's edges and stacking children
// vertically/horizontally. Specs are in logical units (DIPs); results are physical pixels
// (spec * uiScale), with every edge snapped to whole pixels so text and borders stay crisp.
#pragma once

#include "client/core/Geometry.h"

#include <cstdint>
#include <span>

namespace vo::client::ui {

namespace anchor {
inline constexpr std::uint8_t kLeft = 1u << 0;
inline constexpr std::uint8_t kTop = 1u << 1;
inline constexpr std::uint8_t kRight = 1u << 2;
inline constexpr std::uint8_t kBottom = 1u << 3;
inline constexpr std::uint8_t kCenterX = 1u << 4;
inline constexpr std::uint8_t kCenterY = 1u << 5;

inline constexpr std::uint8_t kTopLeft = kLeft | kTop;
inline constexpr std::uint8_t kTopRight = kRight | kTop;
inline constexpr std::uint8_t kBottomLeft = kLeft | kBottom;
inline constexpr std::uint8_t kBottomRight = kRight | kBottom;
inline constexpr std::uint8_t kCenter = kCenterX | kCenterY;
inline constexpr std::uint8_t kFillX = kLeft | kRight;
inline constexpr std::uint8_t kFillY = kTop | kBottom;
inline constexpr std::uint8_t kFill = kFillX | kFillY;
} // namespace anchor

// Per axis: both edges anchored -> stretch between the margins (size ignored); one edge -> pinned to
// it at `size`; center -> centred (margins act as an offset: +left/top, -right/bottom); neither ->
// treated as left/top.
struct LayoutSpec
{
    std::uint8_t anchors = anchor::kTopLeft;
    Insets margins;
    Vec2 size;
};

[[nodiscard]] Rect computeAnchoredRect(const Rect& parent, const LayoutSpec& spec, float scale) noexcept;

enum class Orientation : std::uint8_t
{
    Vertical,
    Horizontal,
};

enum class CrossAlign : std::uint8_t
{
    Stretch,
    Start,
    Center,
    End,
};

struct StackItem
{
    Vec2 size; // logical; main-axis size <= 0 means "flexible" (shares the leftover space)
    CrossAlign align = CrossAlign::Stretch;
    bool visible = true; // invisible items take no space
};

// Lays items out along `orientation` inside `content` (physical pixels) with `spacing` (logical)
// between visible items. Flexible items split the remaining main-axis space equally. Writes one
// rect per item (invisible items get an empty rect at the current pen position).
void stackLayout(const Rect& content, Orientation orientation, float spacing, float scale,
                 std::span<const StackItem> items, std::span<Rect> out) noexcept;

} // namespace vo::client::ui
