// Small 2D value types shared by client render, UI and layout code: Vec2, Rect, Insets, Color.
// Pure header (no platform or D3D dependency) so it can be used by vo_client_core and its tests.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vo::client {

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, float s) noexcept { return {a.x * s, a.y * s}; }
    friend constexpr bool operator==(Vec2 a, Vec2 b) noexcept = default;
};

// Axis-aligned rectangle; x/y is the top-left corner, y grows downwards (screen convention).
struct Rect
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    [[nodiscard]] constexpr float left() const noexcept { return x; }
    [[nodiscard]] constexpr float top() const noexcept { return y; }
    [[nodiscard]] constexpr float right() const noexcept { return x + w; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + h; }
    [[nodiscard]] constexpr float centerX() const noexcept { return x + w * 0.5f; }
    [[nodiscard]] constexpr float centerY() const noexcept { return y + h * 0.5f; }
    [[nodiscard]] constexpr bool empty() const noexcept { return w <= 0.0f || h <= 0.0f; }

    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept
    {
        return px >= x && py >= y && px < x + w && py < y + h;
    }

    [[nodiscard]] static constexpr Rect fromEdges(float l, float t, float r, float b) noexcept
    {
        return {l, t, r - l, b - t};
    }

    // Shrinks by the given amounts on each side (negative values grow).
    [[nodiscard]] constexpr Rect inset(float l, float t, float r, float b) const noexcept
    {
        return {x + l, y + t, std::max(0.0f, w - l - r), std::max(0.0f, h - t - b)};
    }
    [[nodiscard]] constexpr Rect inset(float all) const noexcept { return inset(all, all, all, all); }

    [[nodiscard]] Rect intersect(const Rect& o) const noexcept
    {
        const float l = std::max(x, o.x);
        const float t = std::max(y, o.y);
        const float r = std::min(right(), o.right());
        const float b = std::min(bottom(), o.bottom());
        return {l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
    }

    // Rounds every edge to the nearest integer pixel.
    [[nodiscard]] Rect snapped() const noexcept
    {
        const float l = std::round(x);
        const float t = std::round(y);
        return {l, t, std::round(x + w) - l, std::round(y + h) - t};
    }

    friend constexpr bool operator==(const Rect& a, const Rect& b) noexcept = default;
};

struct Insets
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] static constexpr Insets all(float v) noexcept { return {v, v, v, v}; }
    [[nodiscard]] constexpr Insets scaled(float s) const noexcept
    {
        return {left * s, top * s, right * s, bottom * s};
    }
    friend constexpr bool operator==(const Insets& a, const Insets& b) noexcept = default;
};

// Straight (non-premultiplied) sRGB color, components 0..1. The sprite shader converts it to linear
// premultiplied space.
struct Color
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    // 0xRRGGBB
    [[nodiscard]] static constexpr Color hex(std::uint32_t rgb, float alpha = 1.0f) noexcept
    {
        return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                static_cast<float>((rgb >> 8) & 0xFF) / 255.0f, static_cast<float>(rgb & 0xFF) / 255.0f,
                alpha};
    }
    [[nodiscard]] constexpr Color withAlpha(float alpha) const noexcept { return {r, g, b, alpha}; }
    [[nodiscard]] constexpr Color scaledAlpha(float s) const noexcept { return {r, g, b, a * s}; }

    // RGBA8 packed little-endian (R in the lowest byte), matching DXGI_FORMAT_R8G8B8A8_UNORM.
    [[nodiscard]] std::uint32_t packed() const noexcept
    {
        auto c = [](float v)
        { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
    }

    friend constexpr bool operator==(const Color& a, const Color& b) noexcept = default;
};

[[nodiscard]] constexpr Color lerp(const Color& a, const Color& b, float t) noexcept
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

namespace colors {
inline constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
inline constexpr Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
inline constexpr Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
} // namespace colors

} // namespace vo::client
