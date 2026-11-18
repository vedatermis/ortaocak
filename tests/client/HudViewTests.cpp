// HUD logic (client/world/HudView.h): the bottom action bar layout at every supported screen size,
// the HUD button tooltips, the "Tuşlar" reference, the Esc order with it, window placement next to
// the HUD and the experience bar texts.
#include "client/world/HudView.h"
#include "client/world/LootView.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace world = vo::client::world;
using vo::client::Rect;
using vo::client::Vec2;

namespace {

struct Screen
{
    float w;
    float h;
    float scale;
};
// Every size the UI flows use (physical size / UI scale = logical size; 1024x640 @150 % is fitted to
// 960x600 logical by UiScale.h), plus smaller logical screens the fit never produces.
constexpr Screen kScreens[] = {{1280, 720, 1.0f},  {1920, 1080, 1.0f}, {960, 600, 1.0f},   {1024, 640, 1.0f},
                               {1600, 900, 1.0f},  {1600, 900, 1.25f}, {1920, 1080, 1.5f}, {1024, 640, 1.5f},
                               {800, 600, 1.0f}};

bool inside(const Rect& inner, const Rect& outer)
{
    return inner.x >= outer.x - 0.01f && inner.y >= outer.y - 0.01f && inner.right() <= outer.right() + 0.01f &&
           inner.bottom() <= outer.bottom() + 0.01f;
}

} // namespace

TEST_CASE("HUD bar: every piece inside the frame, the frame inside the screen, nothing overlaps", "[hud]")
{
    for (const Screen& s : kScreens)
    {
        const float w = s.w / s.scale;
        const float h = s.h / s.scale;
        INFO(s.w << "x" << s.h << " @" << s.scale);
        const world::HudBarLayout l = world::computeHudBar(w, h);
        CHECK(l.frame.x >= 0.0f);
        CHECK(l.frame.right() <= w);
        CHECK(l.frame.bottom() <= h);
        CHECK(l.frame.y > h * 0.6f); // a bottom strip, never more than 40 % of the height
        const Rect local{0.0f, 0.0f, l.frame.w, l.frame.h};
        std::vector<Rect> pieces;
        for (const Rect& r : l.actions)
            pieces.push_back(r);
        pieces.push_back(l.slots);
        for (const Rect& r : l.menus)
            pieces.push_back(r);
        pieces.push_back(l.xp);
        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
            CHECK(inside(pieces[i], local));
            for (std::size_t j = i + 1; j < pieces.size(); ++j)
                CHECK_FALSE(world::rectsOverlap(pieces[i], pieces[j], 0.0f));
        }
        // Slot row geometry matches the skill widget's (8 slots + 7 gaps).
        CHECK(l.slots.w == 8.0f * l.slot + 7.0f * l.slotGap);
        // Left to right: actions, separator, slots, separator, menu.
        CHECK(l.actions[3].right() < l.separators[0]);
        CHECK(l.separators[0] < l.slots.x);
        CHECK(l.slots.right() < l.separators[1]);
        CHECK(l.separators[1] < l.menus[0].x);
        // Centred.
        CHECK(std::abs(l.frame.centerX() - w * 0.5f) <= 1.0f);
    }
}

TEST_CASE("HUD bar: full size from 1024 logical pixels, shrunk uniformly below", "[hud]")
{
    const world::HudBarLayout wide = world::computeHudBar(1280.0f, 720.0f);
    CHECK(wide.k == 1.0f);
    CHECK(wide.slot == 48.0f);
    CHECK(wide.actions[0].w == 40.0f);
    CHECK(wide.menus[0].w == 34.0f);
    CHECK(world::computeHudBar(1024.0f, 640.0f).k == 1.0f);
    // 683 logical pixels wide (1024 at a 150 % scale, were it not fitted to 960x600 by UiScale.h).
    const world::HudBarLayout narrow = world::computeHudBar(1024.0f / 1.5f, 640.0f / 1.5f);
    CHECK(narrow.k < 1.0f);
    CHECK(narrow.k >= 0.75f);
    CHECK(narrow.frame.w <= 1024.0f / 1.5f - 16.0f + 1.0f);
    CHECK(narrow.slot >= 36.0f); // 54 physical pixels: icons stay readable
    // Never below the minimum: an absurdly narrow window overflows instead of vanishing.
    CHECK(world::computeHudBar(200.0f, 400.0f).k == world::HudBarMetrics{}.minScale);
}

TEST_CASE("HUD bar: the transient lines stack above it", "[hud]")
{
    const world::HudBarLayout l = world::computeHudBar(1280.0f, 720.0f);
    const world::HudBottomLines b = world::hudBottomLines(l, 24.0f, 30.0f);
    CHECK(b.castBar + 18.0f < l.top());
    CHECK(b.message + 24.0f < b.castBar);
    CHECK(b.gateHint + 30.0f < b.message);
    CHECK(b.pickupBottom < l.top());
}

TEST_CASE("HUD buttons: names, keys, widgets and icons", "[hud]")
{
    CHECK(world::hudButtonInfo(world::HudButton::Attack).key == "F");
    CHECK(world::hudButtonInfo(world::HudButton::Sit).key == "X");
    CHECK(world::hudButtonInfo(world::HudButton::Loot).key == "E");
    CHECK(world::hudButtonInfo(world::HudButton::Run).key == "R");
    CHECK(world::hudButtonInfo(world::HudButton::Inventory).key == "I");
    CHECK(world::hudButtonInfo(world::HudButton::Kunye).key == "C");
    CHECK(world::hudButtonInfo(world::HudButton::Skills).key == "K");
    CHECK(world::hudButtonInfo(world::HudButton::Controls).key == "H");
    CHECK(world::hudButtonInfo(world::HudButton::Menu).key == "Esc");
    // The widget names the older flows click stay.
    CHECK(world::hudButtonInfo(world::HudButton::Inventory).widget == "world.inventory");
    CHECK(world::hudButtonInfo(world::HudButton::Kunye).widget == "world.kunye");
    CHECK(world::hudButtonInfo(world::HudButton::Skills).widget == "world.skills");
    std::vector<std::string_view> widgets;
    for (const world::HudButton b : world::kHudActions)
        widgets.push_back(world::hudButtonInfo(b).widget);
    for (const world::HudButton b : world::kHudMenus)
        widgets.push_back(world::hudButtonInfo(b).widget);
    for (std::size_t i = 0; i < widgets.size(); ++i)
        for (std::size_t j = i + 1; j < widgets.size(); ++j)
            CHECK(widgets[i] != widgets[j]);
    world::HudButtonState s;
    CHECK(world::hudButtonIcon(world::HudButton::Run, s) == "ui/hud/run.png");
    s.running = false;
    CHECK(world::hudButtonIcon(world::HudButton::Run, s) == "ui/hud/walk.png");
    CHECK(world::hudButtonIcon(world::HudButton::Menu, s) == "ui/hud/menu.png");
}

TEST_CASE("HUD buttons: tooltips carry name, key, a line and the state", "[hud]")
{
    world::HudButtonState s;
    auto text = [&](world::HudButton b) { return world::hudTipPlainText(world::hudButtonTipLines(b, s)); };
    for (const world::HudButton b : world::kHudActions)
    {
        const auto lines = world::hudButtonTipLines(b, s);
        REQUIRE(lines.size() >= 2);
        CHECK(lines[0].tone == world::HudTipLine::Tone::Title);
        CHECK(lines[0].text.find(std::string(world::hudButtonInfo(b).key)) != std::string::npos);
        CHECK_FALSE(lines[1].text.empty());
    }
    CHECK(text(world::HudButton::Skills).find("Yetenekler (K)") != std::string::npos);
    CHECK(text(world::HudButton::Menu).find("Menü (Esc)") != std::string::npos);
    CHECK(text(world::HudButton::Sit).find("ayaktasın") != std::string::npos);
    s.seated = true;
    CHECK(text(world::HudButton::Sit).find("oturuyorsun") != std::string::npos);
    CHECK(text(world::HudButton::Run).find("Şu an: koşu") != std::string::npos);
    s.running = false;
    CHECK(text(world::HudButton::Run).find("Şu an: yürüyüş") != std::string::npos);
    CHECK(text(world::HudButton::Attack).find("Önce bir canavar seç") != std::string::npos);
    s.hasTarget = true;
    s.attacking = true;
    CHECK(text(world::HudButton::Attack).find("saldırıyorsun") != std::string::npos);
    s.points = 3;
    CHECK(text(world::HudButton::Kunye).find("nitelik puanı: 3") != std::string::npos);
    CHECK(text(world::HudButton::Skills).find("3") != std::string::npos);
    s.lootInRange = true;
    CHECK(text(world::HudButton::Loot).find("ganimet var") != std::string::npos);
}

TEST_CASE("Controls window (Tuslar): every control of the old help lines is listed, grouped", "[hud]")
{
    const auto& groups = world::controlsReference();
    std::vector<std::string_view> titles;
    for (const auto& g : groups)
        titles.push_back(g.title);
    for (const char* t : {"Hareket", "Kamera", "Dövüş", "Pencereler", "Diğer"})
        CHECK(std::find(titles.begin(), titles.end(), std::string_view(t)) != titles.end());
    const std::string all = world::controlsPlainText();
    // The two help lines of M6-M13, piece by piece.
    for (const char* needle :
         {"W / A / S / D", "Basılı tut: oraya git", "Sağ tık — Basılı tut, sürükle: kamerayı çevir","PgUp / PgDn", "Tekerlek", "Shift",
          "R — Koşu / yürüyüş", "I — Envanter", "C — Künye", "F3", "Esc — Menü", "Sol tık — Hedef seç",
          "Sağ tık / Çift tık / F — Saldır", "Tab — Sıradaki hedef", "1 – 8", "X — Otur / kalk", "E — Ganimet",
          "Esc — Hedefi bırak", "K — Yetenekler", "H — Tuşlar", "Boşluk — Zıpla"})
    {
        INFO(needle);
        CHECK(all.find(needle) != std::string::npos);
    }
    // Two columns, both used.
    bool left = false;
    bool right = false;
    for (const auto& g : groups)
    {
        CHECK_FALSE(g.rows.empty());
        left = left || g.column == 0;
        right = right || g.column == 1;
        for (const auto& r : g.rows)
        {
            CHECK_FALSE(r.keys.empty());
            CHECK_FALSE(r.text.empty());
        }
    }
    CHECK(left);
    CHECK(right);
}

TEST_CASE("Esc order with the controls window (Tuslar)", "[hud]")
{
    using A = world::EscapeAction;
    CHECK(world::escapeActionHud(true, true, true, true, true, true, false) == A::CloseLoot);
    CHECK(world::escapeActionHud(false, true, true, true, true, true, false) == A::CloseControls);
    CHECK(world::escapeActionHud(false, false, true, true, true, true, false) == A::CloseSkills);
    CHECK(world::escapeActionHud(false, false, false, true, true, true, false) == A::CloseKunye);
    CHECK(world::escapeActionHud(false, false, false, false, true, true, false) == A::CloseInventory);
    CHECK(world::escapeActionHud(false, false, false, false, false, true, false) == A::ClearTarget);
    CHECK(world::escapeActionHud(false, false, false, false, false, false, false) == A::OpenMenu);
    CHECK(world::escapeActionHud(true, true, true, true, true, true, true) == A::None);
}

TEST_CASE("Windows next to the HUD: free band first, then over the top HUD, then over the bar", "[hud]")
{
    // 1280x720: the Künye (580x388) fits between the player frame and the bar.
    const world::HudBarLayout bar = world::computeHudBar(1280.0f, 720.0f);
    const auto k = world::placeWindow(1280.0f, 720.0f, Vec2{580.0f, 388.0f}, true, false, 14.0f, 140.0f, bar.top());
    CHECK(k.fit == world::WindowFit::Free);
    CHECK(k.rect.x == 14.0f);
    CHECK(k.rect.y >= 144.0f);
    CHECK(k.rect.bottom() <= bar.top() - 4.0f);
    CHECK_FALSE(world::rectsOverlap(k.rect, bar.frame));
    // Right side.
    const auto inv = world::placeWindow(1280.0f, 720.0f, Vec2{652.0f, 334.0f}, false, false, 24.0f, 70.0f, bar.top());
    CHECK(inv.fit == world::WindowFit::Free);
    CHECK(inv.rect.right() == 1280.0f - 24.0f);
    // 1024x640 @150 %: the UI scale is fitted to a 960x600 logical canvas (UiScale.h fitUiScale). The
    // Künye (580x388) still fits between the player frame (and its effect row: 110) and the bar; the
    // Yetenekler window (656x424) goes over the player frame, never over the bar; the inventory fits.
    {
        const world::HudBarLayout b = world::computeHudBar(960.0f, 600.0f);
        CHECK(b.k == 1.0f);
        const auto kunye = world::placeWindow(960.0f, 600.0f, Vec2{580.0f, 388.0f}, true, false, 14.0f, 110.0f, b.top());
        CHECK(kunye.fit == world::WindowFit::Free);
        const auto skills = world::placeWindow(960.0f, 600.0f, Vec2{656.0f, 424.0f}, true, false, 14.0f, 110.0f, b.top());
        CHECK(skills.fit == world::WindowFit::OverTop);
        CHECK_FALSE(world::rectsOverlap(skills.rect, b.frame));
        const auto inv960 = world::placeWindow(960.0f, 600.0f, Vec2{652.0f, 334.0f}, false, false, 24.0f, 64.0f, b.top());
        CHECK(inv960.fit == world::WindowFit::Free);
        CHECK_FALSE(world::rectsOverlap(inv960.rect, b.frame));
    }
    // A smaller logical screen (683x427, e.g. a UI scale forced past the fit): the Yetenekler window only
    // fits on the whole screen; the inventory fits over the top HUD, above the bar.
    const float w = 1024.0f / 1.5f;
    const float h = 640.0f / 1.5f;
    const world::HudBarLayout small = world::computeHudBar(w, h);
    const auto sk = world::placeWindow(w, h, Vec2{656.0f, 424.0f}, true, false, 14.0f, 140.0f, small.top());
    CHECK(sk.fit == world::WindowFit::OverBar);
    CHECK(sk.rect.y >= 0.0f);
    CHECK(sk.rect.bottom() <= h + 0.5f);
    const auto inv2 = world::placeWindow(w, h, Vec2{652.0f, 334.0f}, false, false, 14.0f, 70.0f, small.top());
    CHECK(inv2.fit == world::WindowFit::OverTop);
    CHECK(inv2.rect.bottom() <= small.top());
    CHECK_FALSE(world::rectsOverlap(inv2.rect, small.frame));
    // Centred windows.
    const auto c = world::placeWindow(1280.0f, 720.0f, Vec2{600.0f, 300.0f}, false, true, 14.0f, 100.0f, bar.top());
    CHECK(c.rect.x == 340.0f);
    // What is covered.
    const Rect covers[] = {sk.rect};
    CHECK(world::coveredByAny(small.frame, covers));
    CHECK_FALSE(world::coveredByAny(Rect{700.0f, 0.0f, 10.0f, 10.0f}, covers));
    CHECK_FALSE(world::rectsOverlap(Rect{0, 0, 10, 10}, Rect{10, 0, 10, 10}));
}

TEST_CASE("Experience bar texts", "[hud]")
{
    CHECK(world::xpBarText(5, 34, 80) == "Seviye 5  ·  34 / 80  ·  %42");
    CHECK(world::xpBarText(30, 0, 0) == "Seviye 30  ·  en yüksek seviye");
    CHECK(world::xpBarShortText(5, 34, 80) == "Sv. 5  ·  %42");
    CHECK(world::xpBarText(2, 79, 80).find("%98") != std::string::npos);
    CHECK(world::xpBarText(2, 80, 80).find("%99") != std::string::npos); // never "%100" before the level-up
    CHECK(world::xpFraction(40, 80) == 0.5f);
    CHECK(world::xpFraction(5, 0) == 1.0f);
    CHECK(world::xpFraction(100, 80) == 1.0f);
    const auto tip = world::xpTipLines(5, 34, 80);
    REQUIRE(tip.size() == 3);
    CHECK(tip[0].text == "Deneyim (Seviye 5)");
    CHECK(tip[1].text == "34 / 80 (%42)");
    CHECK(tip[2].text == "Sonraki seviyeye 46");
}

// ---- fix1: HUD scale with the resolution, the window placement manager ----

TEST_CASE("HUD scale grows with the screen height in quarter steps (fix1)", "[hud]")
{
    CHECK(world::hudResolutionScale(720.0f, 1.0f) == 1.0f);
    CHECK(world::hudResolutionScale(640.0f, 1.0f) == 1.0f); // never below the player's own scale
    CHECK(world::hudResolutionScale(900.0f, 1.0f) == 1.25f);
    CHECK(world::hudResolutionScale(1080.0f, 1.0f) == 1.5f);
    CHECK(world::hudResolutionScale(1079.9f, 1.0f) == 1.5f);
    CHECK(world::hudResolutionScale(1200.0f, 1.0f) == 1.5f);
    CHECK(world::hudResolutionScale(1440.0f, 1.0f) == 2.0f);
    CHECK(world::hudResolutionScale(2160.0f, 1.0f) == 3.0f);
    CHECK(world::hudResolutionScale(1080.0f, 1.75f) == 1.75f); // a larger player scale wins
    CHECK(world::hudResolutionScale(0.0f, 1.25f) == 1.25f);
    // The bar keeps its share of the screen: 1280x720, 1920x1080, 2560x1440 are all 1280x720 logical.
    for (const float h : {720.0f, 1080.0f, 1440.0f})
    {
        const float k = world::hudResolutionScale(h, 1.0f);
        const world::HudBarLayout bar = world::computeHudBar(h * 16.0f / 9.0f / k, h / k);
        INFO(h);
        CHECK(std::abs(bar.frame.h * k / h - 86.0f / 720.0f) < 0.002f);
        CHECK(bar.k == 1.0f);
    }
}

namespace {

using Specs = std::array<world::WindowSpec, world::kHudWindowCount>;
constexpr Vec2 kKunye{580.0f, 388.0f};
constexpr Vec2 kSkills{656.0f, 424.0f};
constexpr Vec2 kInv{652.0f, 334.0f};
constexpr Vec2 kControls{652.0f, 475.0f};
constexpr Vec2 kShop{300.0f, 344.0f};

world::WindowScreen screenOf(float w, float h)
{
    world::WindowScreen s;
    s.width = w;
    s.height = h;
    s.barTop = world::computeHudBar(w, h).top();
    s.leftTop = 110.0f;
    s.rightTop = 64.0f;
    s.leftColumn = world::kHudPlayerFrame.right() + 8.0f;
    return s;
}

world::WindowSpec& open(Specs& specs, world::HudWindow w, Vec2 size, world::WindowSide side, float margin,
                        std::uint64_t use)
{
    world::WindowSpec& s = specs[static_cast<std::size_t>(w)];
    s.open = true;
    s.size = size;
    s.side = side;
    s.margin = margin;
    s.lastUse = use;
    return s;
}

const world::ArrangedWindow& at(const world::WindowArranger::Result& r, world::HudWindow w)
{
    return r.windows[static_cast<std::size_t>(w)];
}

// Every shown window on the screen, off the bar and off the others.
void checkClear(const world::WindowScreen& s, const world::WindowArranger::Result& r)
{
    const world::HudBarLayout bar = world::computeHudBar(s.width, s.height);
    for (std::size_t i = 0; i < world::kHudWindowCount; ++i)
    {
        const world::ArrangedWindow& a = r.windows[i];
        if (!a.shown)
            continue;
        INFO(world::hudWindowName(static_cast<world::HudWindow>(i)) << " at " << a.rect.x << "," << a.rect.y);
        CHECK(a.rect.x >= 0.0f);
        CHECK(a.rect.y >= 0.0f);
        CHECK(a.rect.right() <= s.width + 0.01f);
        CHECK_FALSE(world::rectsOverlap(a.rect, bar.frame));
        for (std::size_t j = 0; j < i; ++j)
            if (r.windows[j].shown)
                CHECK_FALSE(world::rectsOverlap(a.rect, r.windows[j].rect));
    }
}

} // namespace

TEST_CASE("Window manager: preferred sides, the next free place, never on top of each other (fix1)", "[hud]")
{
    using W = world::HudWindow;
    using S = world::WindowSide;
    const world::WindowScreen scr = screenOf(1280.0f, 720.0f);
    world::WindowArranger arr;
    Specs specs{};
    // The inventory alone: right edge.
    open(specs, W::Inventory, kInv, S::Right, 24.0f, 1);
    auto r = arr.arrange(scr, specs);
    CHECK(r.close.empty());
    CHECK(at(r, W::Inventory).rect.right() == 1280.0f - 24.0f);
    // The Künye next: the left edge is free.
    open(specs, W::Kunye, kKunye, S::Left, 14.0f, 2);
    r = arr.arrange(scr, specs);
    CHECK(r.close.empty());
    CHECK(at(r, W::Kunye).rect.x == 14.0f);
    CHECK(at(r, W::Inventory).rect.right() == 1280.0f - 24.0f); // the open one stays where it was
    checkClear(scr, r);
    // Yetenekler (left, 656 wide): its side is taken and there is no room for three: the least recently
    // used window in the way (the inventory) closes; Yetenekler goes beside the Künye.
    open(specs, W::Skills, kSkills, S::Left, 14.0f, 3);
    r = arr.arrange(scr, specs);
    REQUIRE(r.close.size() == 1);
    CHECK(r.close[0] == W::Inventory);
    CHECK_FALSE(at(r, W::Inventory).shown);
    CHECK(at(r, W::Kunye).rect.x == 14.0f);
    CHECK(at(r, W::Skills).shown);
    CHECK(at(r, W::Skills).rect.x == 14.0f + kKunye.x + world::WindowArranger::kGap);
    checkClear(scr, r);
    // Stable: the same request again moves nothing and closes nothing.
    specs[static_cast<std::size_t>(W::Inventory)].open = false;
    const auto again = arr.arrange(scr, specs);
    CHECK(again.close.empty());
    CHECK(at(again, W::Skills).rect.x == at(r, W::Skills).rect.x);
    CHECK(at(again, W::Kunye).rect.x == at(r, W::Kunye).rect.x);
}

TEST_CASE("Window manager: Tuslar beside the Kunye, a resize re-places, small screens close the older (fix1)", "[hud]")
{
    using W = world::HudWindow;
    using S = world::WindowSide;
    world::WindowArranger arr;
    Specs specs{};
    open(specs, W::Kunye, kKunye, S::Left, 14.0f, 1);
    open(specs, W::Controls, kControls, S::Center, 0.0f, 2);
    const world::WindowScreen wide = screenOf(1280.0f, 720.0f);
    auto r = arr.arrange(wide, specs);
    CHECK(r.close.empty());
    CHECK(at(r, W::Controls).rect.x >= at(r, W::Kunye).rect.right());
    checkClear(wide, r);
    // 960x600 (1024x640 @150 %): the two do not fit side by side: the older (Künye) closes.
    const world::WindowScreen small = screenOf(960.0f, 600.0f);
    r = arr.arrange(small, specs);
    REQUIRE(r.close.size() == 1);
    CHECK(r.close[0] == W::Kunye);
    CHECK(at(r, W::Controls).shown);
    checkClear(small, r);
    // The Künye and the inventory at 960x600: the newer one stays.
    Specs two{};
    open(two, W::Kunye, kKunye, S::Left, 14.0f, 5);
    open(two, W::Inventory, kInv, S::Right, 24.0f, 6);
    world::WindowArranger a2;
    r = a2.arrange(small, two);
    REQUIRE(r.close.size() == 1);
    CHECK(r.close[0] == W::Kunye);
    CHECK(at(r, W::Inventory).shown);
    CHECK(at(r, W::Inventory).rect.right() == 960.0f - 24.0f);
    checkClear(small, r);
    // Yetenekler at 960x600 alone: above the bar (over the top HUD), never over the bar.
    Specs one{};
    open(one, W::Skills, kSkills, S::Left, 14.0f, 1);
    world::WindowArranger a3;
    r = a3.arrange(small, one);
    CHECK(r.close.empty());
    CHECK(at(r, W::Skills).fit == world::WindowFit::OverTop);
    checkClear(small, r);
}

TEST_CASE("Window manager: the shop pair leaves a gap for the merchant, low on narrow screens (fix1)", "[hud]")
{
    using W = world::HudWindow;
    using S = world::WindowSide;
    {
        const world::WindowScreen scr = screenOf(1280.0f, 720.0f);
        world::WindowArranger arr;
        Specs specs{};
        open(specs, W::Shop, kShop, S::Left, 8.0f, 1);
        open(specs, W::Inventory, kInv, S::Right, 8.0f, 2);
        const auto r = arr.arrange(scr, specs);
        CHECK(r.close.empty());
        CHECK(at(r, W::Shop).rect.x == 8.0f);
        CHECK(at(r, W::Inventory).rect.right() == 1272.0f);
        CHECK(at(r, W::Inventory).rect.x - at(r, W::Shop).rect.right() >= 160.0f); // the merchant's gap
        checkClear(scr, r);
    }
    {
        // 960x600: no gap; both touch, low (just above the bar), the merchant is framed above them.
        const world::WindowScreen scr = screenOf(960.0f, 600.0f);
        world::WindowArranger arr;
        Specs specs{};
        open(specs, W::Shop, kShop, S::Left, 4.0f, 1).bottom = true;
        open(specs, W::Inventory, kInv, S::Right, 4.0f, 2).bottom = true;
        const auto r = arr.arrange(scr, specs);
        CHECK(r.close.empty());
        CHECK(at(r, W::Shop).rect.x == 4.0f);
        CHECK(at(r, W::Inventory).rect.x == at(r, W::Shop).rect.right());
        CHECK(at(r, W::Shop).rect.bottom() == scr.barTop - 4.0f);
        CHECK(at(r, W::Inventory).rect.bottom() == scr.barTop - 4.0f);
        checkClear(scr, r);
    }
}

TEST_CASE("Window manager: dragged windows stay until they are in the way (fix1)", "[hud]")
{
    using W = world::HudWindow;
    using S = world::WindowSide;
    const world::WindowScreen scr = screenOf(1280.0f, 720.0f);
    world::WindowArranger arr;
    Specs specs{};
    open(specs, W::Kunye, kKunye, S::Left, 14.0f, 1);
    auto r = arr.arrange(scr, specs);
    arr.setDragged(W::Kunye, {400.0f, 150.0f});
    r = arr.arrange(scr, specs);
    CHECK(at(r, W::Kunye).rect.x == 400.0f);
    CHECK(at(r, W::Kunye).rect.y == 150.0f);
    CHECK(arr.dragged(W::Kunye));
    // Dragged off the screen / onto the bar: clamped above the bar.
    arr.setDragged(W::Kunye, {2000.0f, 900.0f});
    r = arr.arrange(scr, specs);
    CHECK(at(r, W::Kunye).rect.right() == 1280.0f);
    CHECK(at(r, W::Kunye).rect.bottom() <= scr.barTop - 4.0f + 0.01f);
    arr.setDragged(W::Kunye, {400.0f, 150.0f});
    r = arr.arrange(scr, specs);
    // The inventory opens and needs that room: the dragged Künye gives up its spot (placed again, left);
    // nothing closes.
    open(specs, W::Inventory, kInv, S::Right, 24.0f, 2);
    r = arr.arrange(scr, specs);
    CHECK(r.close.empty());
    CHECK_FALSE(arr.dragged(W::Kunye));
    CHECK(at(r, W::Kunye).rect.x == 14.0f);
    CHECK(at(r, W::Inventory).rect.right() == 1256.0f);
    checkClear(scr, r);
    // While dragging (live) a window may pass over another; released there, it goes back to a free place.
    specs[static_cast<std::size_t>(W::Inventory)].lastUse = 3;
    arr.setDragged(W::Inventory, {100.0f, 200.0f}, true);
    r = arr.arrange(scr, specs);
    CHECK(at(r, W::Inventory).rect.x == 100.0f);
    arr.setDragged(W::Inventory, {100.0f, 200.0f}, false);
    r = arr.arrange(scr, specs);
    CHECK(r.close.empty());
    CHECK_FALSE(arr.dragged(W::Inventory));
    CHECK(at(r, W::Inventory).rect.right() == 1256.0f);
    checkClear(scr, r);
}
