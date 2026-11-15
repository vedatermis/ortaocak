#include "client/ui/Layout.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using vo::client::Rect;
using namespace vo::client::ui;

TEST_CASE("Anchored rect: pinned to edges", "[client][layout]")
{
    const Rect parent{100.0f, 50.0f, 400.0f, 300.0f};
    LayoutSpec spec;
    spec.size = {80.0f, 20.0f};
    spec.margins = {10.0f, 5.0f, 15.0f, 7.0f};

    spec.anchors = anchor::kTopLeft;
    REQUIRE(computeAnchoredRect(parent, spec, 1.0f) == Rect{110.0f, 55.0f, 80.0f, 20.0f});

    spec.anchors = anchor::kBottomRight;
    REQUIRE(computeAnchoredRect(parent, spec, 1.0f) == Rect{405.0f, 323.0f, 80.0f, 20.0f});

    spec.anchors = 0; // treated as top-left
    REQUIRE(computeAnchoredRect(parent, spec, 1.0f) == Rect{110.0f, 55.0f, 80.0f, 20.0f});
}

TEST_CASE("Anchored rect: stretch and centre", "[client][layout]")
{
    const Rect parent{0.0f, 0.0f, 1280.0f, 720.0f};
    LayoutSpec spec;
    spec.anchors = anchor::kFillX | anchor::kBottom;
    spec.margins = {60.0f, 0.0f, 60.0f, 28.0f};
    spec.size = {0.0f, 104.0f};
    REQUIRE(computeAnchoredRect(parent, spec, 1.0f) == Rect{60.0f, 588.0f, 1160.0f, 104.0f});

    spec.anchors = anchor::kCenter;
    spec.margins = {};
    spec.size = {300.0f, 200.0f};
    REQUIRE(computeAnchoredRect(parent, spec, 1.0f) == Rect{490.0f, 260.0f, 300.0f, 200.0f});

    // Centre with margins acts as an offset (+left, -right).
    spec.margins = {0.0f, 0.0f, 100.0f, 0.0f};
    REQUIRE(computeAnchoredRect(parent, spec, 1.0f).x == 440.0f);

    // Stretch never goes negative.
    spec.anchors = anchor::kFill;
    spec.margins = {700.0f, 0.0f, 700.0f, 0.0f};
    REQUIRE(computeAnchoredRect(parent, spec, 1.0f).w == 0.0f);
}

TEST_CASE("Anchored rect at 150% scale snaps to whole pixels", "[client][layout]")
{
    const Rect parent{0.0f, 0.0f, 1920.0f, 1080.0f};
    LayoutSpec spec;
    spec.anchors = anchor::kTopRight;
    spec.margins = {0.0f, 15.0f, 15.0f, 0.0f};
    spec.size = {101.0f, 33.0f};
    const Rect r = computeAnchoredRect(parent, spec, 1.5f);
    REQUIRE(r.right() == 1898.0f); // round(1920 - 22.5) = round(1897.5) = 1898
    REQUIRE(r.w == 152.0f);        // 151.5 -> 152
    REQUIRE(r.y == 23.0f);
    REQUIRE(r.h == 50.0f); // 49.5 -> 50
    REQUIRE(r.x == std::round(r.x));
}

TEST_CASE("Vertical stack with fixed and flexible items", "[client][layout]")
{
    const Rect content{10.0f, 20.0f, 200.0f, 300.0f};
    std::array<StackItem, 4> items{};
    items[0].size = {0.0f, 30.0f};
    items[1].size = {0.0f, 0.0f}; // flexible
    items[2].size = {50.0f, 40.0f};
    items[2].align = CrossAlign::Center;
    items[3].size = {80.0f, 20.0f};
    items[3].align = CrossAlign::End;
    std::array<Rect, 4> out{};
    stackLayout(content, Orientation::Vertical, 10.0f, 1.0f, items, out);

    REQUIRE(out[0] == Rect{10.0f, 20.0f, 200.0f, 30.0f});
    // leftover = 300 - (30 + 40 + 20) - 3*10 = 180
    REQUIRE(out[1] == Rect{10.0f, 60.0f, 200.0f, 180.0f});
    REQUIRE(out[2] == Rect{85.0f, 250.0f, 50.0f, 40.0f});
    REQUIRE(out[3] == Rect{130.0f, 300.0f, 80.0f, 20.0f});
}

TEST_CASE("Horizontal stack splits flexible space and skips hidden items", "[client][layout]")
{
    const Rect content{0.0f, 0.0f, 310.0f, 36.0f};
    std::array<StackItem, 3> items{};
    items[0].size = {0.0f, 0.0f};
    items[1].size = {100.0f, 0.0f};
    items[1].visible = false;
    items[2].size = {0.0f, 0.0f};
    std::array<Rect, 3> out{};
    stackLayout(content, Orientation::Horizontal, 10.0f, 1.0f, items, out);
    REQUIRE(out[0] == Rect{0.0f, 0.0f, 150.0f, 36.0f});
    REQUIRE(out[1].w == 0.0f);
    REQUIRE(out[2] == Rect{160.0f, 0.0f, 150.0f, 36.0f});
}

TEST_CASE("Stack at fractional scale keeps edges contiguous", "[client][layout]")
{
    const Rect content{0.0f, 0.0f, 100.0f, 1000.0f};
    std::array<StackItem, 3> items{};
    for (auto& it : items)
        it.size = {0.0f, 33.0f};
    std::array<Rect, 3> out{};
    stackLayout(content, Orientation::Vertical, 0.0f, 1.25f, items, out);
    REQUIRE(out[0].y == 0.0f);
    REQUIRE(out[0].bottom() == out[1].y);
    REQUIRE(out[1].bottom() == out[2].y);
    for (const Rect& r : out)
        REQUIRE(r.h >= 41.0f);
}
