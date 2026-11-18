// WorldScene, cleanup round "fix1" (docs/DEVELOPMENT_PLAN.md §32): the placement manager of the side
// windows (shop, inventory, Künye, Yetenekler, Tuşlar: world::WindowArranger in HudView.h), dragging them
// by the title bar, the HUD scale with the resolution, the merchant framed beside the shop windows (a lens
// shift of the projection), and the automation of these.
#include "client/scenes/WorldScene.h"

#include "client/platform/Input.h"
#include "client/scenes/InventoryWindow.h"
#include "client/scenes/KunyeWindow.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/ShopWindow.h"
#include "client/scenes/SkillWindow.h"
#include "client/ui/Ui.h"
#include "client/world/ItemData.h"
#include "shared/core/Log.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
using platform::EventType;
using platform::MouseButton;
using world::HudWindow;
using world::Vec3;

namespace {

constexpr float kTitleGrab = 34.0f;   // logical: the title bar strip that drags a window
constexpr float kCloseButton = 44.0f; // logical: the right end of the title bar (the X) does not drag
constexpr float kMerchantRoom = 160.0f; // logical: the gap between the shop pair the merchant is framed in

std::optional<HudWindow> parseWindow(std::string_view name)
{
    for (std::size_t i = 0; i < world::kHudWindowCount; ++i)
        if (name == world::hudWindowName(static_cast<HudWindow>(i)))
            return static_cast<HudWindow>(i);
    return std::nullopt;
}

std::size_t idx(HudWindow w) noexcept
{
    return static_cast<std::size_t>(w);
}

} // namespace

float WorldScene::sceneUiScale(float width, float height, float uiScale) const
{
    (void)width;
    return world::hudResolutionScale(height, uiScale);
}

// ---------------------------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------------------------

ui::Widget* WorldScene::hudWindowWidget(HudWindow w) const
{
    switch (w)
    {
    case HudWindow::Shop: return m_shopWindow;
    case HudWindow::Inventory: return m_invWindow;
    case HudWindow::Kunye: return m_kunyeWindow;
    case HudWindow::Skills: return m_skillWindow;
    case HudWindow::Controls: return m_controlsWindow;
    }
    return nullptr;
}

bool WorldScene::hudWindowOpen(HudWindow w) const
{
    switch (w)
    {
    case HudWindow::Shop: return m_shopWindow != nullptr && m_shop.isOpen() && m_shopWindow->isVisible();
    case HudWindow::Inventory: return m_invWindow != nullptr && m_invOpen;
    case HudWindow::Kunye: return m_kunyeWindow != nullptr && m_kunyeOpen;
    case HudWindow::Skills: return m_skillWindow != nullptr && m_skillsOpen;
    case HudWindow::Controls: return m_controlsWindow != nullptr && m_controlsOpen;
    }
    return false;
}

void WorldScene::closeHudWindow(HudWindow w)
{
    switch (w)
    {
    case HudWindow::Shop: closeShop(true, {}); break;
    case HudWindow::Inventory: setInventoryOpen(false); break;
    case HudWindow::Kunye: setKunyeOpen(false); break;
    case HudWindow::Skills: setSkillsOpen(false); break;
    case HudWindow::Controls: setControlsOpen(false); break;
    }
}

void WorldScene::touchHudWindow(HudWindow w)
{
    m_winUse[idx(w)] = ++m_winClock;
}

void WorldScene::arrangeWindows(float w, float h, float s, float leftTop, float rightTop)
{
    if (m_arranging)
        return; // a window closed below calls layoutHud again
    m_arranging = true;
    const float barTop = m_barLayout.frame.y;
    for (int pass = 0; pass < 4; ++pass)
    {
        // Recency: a window opened since the last arrangement is the most recently used (in HudWindow
        // order when several opened at once: the shop before the inventory it opens).
        for (std::size_t i = 0; i < world::kHudWindowCount; ++i)
        {
            const bool open = hudWindowOpen(static_cast<HudWindow>(i));
            if (open && !m_winWasOpen[i])
                m_winUse[i] = ++m_winClock;
            m_winWasOpen[i] = open;
        }
        const bool shopShown = hudWindowOpen(HudWindow::Shop);
        // The shop's stock scrolls beyond the band under the player frame.
        if (m_shopWindow != nullptr)
            m_shopWindow->setMaxHeight(std::max(200.0f, barTop - leftTop - 8.0f));
        const Vec2 invSize = m_invWindow != nullptr ? m_invWindow->layoutSpec().size : Vec2{652.0f, 334.0f};
        // The shop pair (§8.2): shop left, inventory right, the merchant framed in the gap between them; on a
        // screen too narrow for a gap both sit low (just above the bar) and the merchant is framed above.
        const bool pair = shopShown && m_invOpen;
        const float pairW = ShopWindow::kWidth + invSize.x;
        const bool widePair = !pair || w >= pairW + 16.0f + kMerchantRoom;
        const float pairMargin = widePair ? 8.0f : std::max(0.0f, std::floor((w - pairW) * 0.5f));

        std::array<world::WindowSpec, world::kHudWindowCount> specs{};
        const auto spec = [&](HudWindow id, Vec2 size, world::WindowSide side, float margin, bool bottom) {
            world::WindowSpec& sp = specs[idx(id)];
            sp.open = m_winWasOpen[idx(id)];
            sp.size = size;
            sp.side = side;
            sp.margin = margin;
            sp.bottom = bottom;
            sp.lastUse = m_winUse[idx(id)];
        };
        spec(HudWindow::Shop, {ShopWindow::kWidth, m_shopWindow != nullptr ? m_shopWindow->preferredHeight() : 300.0f},
             world::WindowSide::Left, pairMargin, !widePair);
        spec(HudWindow::Inventory, invSize, world::WindowSide::Right, shopShown ? pairMargin : 24.0f, pair && !widePair);
        spec(HudWindow::Kunye, {KunyeWindow::kWidth, KunyeWindow::kHeight}, world::WindowSide::Left, 14.0f, false);
        spec(HudWindow::Skills, {SkillWindow::kWidth, SkillWindow::kHeight}, world::WindowSide::Left, 14.0f, false);
        Vec2 controls{600.0f, 400.0f};
        if (m_controlsWindow != nullptr && m_controlsOpen)
            controls = m_controlsWindow->fit(w - 16.0f, h - 12.0f);
        spec(HudWindow::Controls, controls, world::WindowSide::Center, 0.0f, false);

        world::WindowScreen screen;
        screen.width = w;
        screen.height = h;
        screen.barTop = barTop;
        screen.leftTop = leftTop;
        screen.rightTop = rightTop;
        screen.leftColumn = world::kHudPlayerFrame.right() + 8.0f;
        const world::WindowArranger::Result r = m_arranger.arrange(screen, specs);

        m_windowCovers.clear();
        for (std::size_t i = 0; i < world::kHudWindowCount; ++i)
        {
            m_winRects[i] = {};
            const world::ArrangedWindow& a = r.windows[i];
            ui::Widget* win = hudWindowWidget(static_cast<HudWindow>(i));
            if (win == nullptr || !a.shown || !specs[i].open)
                continue;
            const Insets& m = win->layoutSpec().margins;
            if (win->layoutSpec().anchors != anchor::kTopLeft)
                win->setAnchors(anchor::kTopLeft);
            if (m.left != a.rect.x || m.top != a.rect.y)
                win->setMargins({a.rect.x, a.rect.y, 0.0f, 0.0f});
            m_winRects[i] = Rect{a.rect.x * s, a.rect.y * s, a.rect.w * s, a.rect.h * s};
            m_windowCovers.push_back(m_winRects[i]);
        }
        if (r.close.empty())
            break;
        for (const HudWindow c : r.close)
        {
            ++m_windowsClosedForRoom;
            VO_LOG_INFO("world: window '{}' closed: no room beside the newer ones ({:.0f}x{:.0f} logical)",
                        world::hudWindowName(c), w, h);
            closeHudWindow(c);
        }
    }
    m_arranging = false;
}

bool WorldScene::windowDragEvent(const platform::InputEvent& e)
{
    const bool usable = m_phase == Phase::Ready && !m_zoneChanging && !m_menuOpen && !ui().hasModal();
    if (m_winDrag.active)
    {
        const HudWindow id = m_winDrag.window;
        const float s = std::max(ui().scale(), 0.01f);
        if (!usable || !hudWindowOpen(id))
        {
            m_winDrag = {};
            return false;
        }
        if (e.type == EventType::MouseMove || e.type == EventType::MouseUp)
        {
            const bool release = e.type == EventType::MouseUp;
            if (release && e.button != MouseButton::Left)
                return false;
            const Vec2 pos{(static_cast<float>(e.x) - m_winDrag.grabX) / s, (static_cast<float>(e.y) - m_winDrag.grabY) / s};
            m_arranger.setDragged(id, pos, !release);
            touchHudWindow(id);
            if (release)
            {
                m_winDrag = {};
                VO_LOG_INFO("world: window '{}' dragged to ({:.0f}, {:.0f})", world::hudWindowName(id), pos.x, pos.y);
            }
            layoutHud();
            return true;
        }
        if (e.type == EventType::FocusLost)
            m_winDrag = {};
        return false;
    }
    if (!usable || e.type != EventType::MouseDown)
        return false;
    const float s = std::max(ui().scale(), 0.01f);
    const auto x = static_cast<float>(e.x);
    const auto y = static_cast<float>(e.y);
    for (std::size_t i = 0; i < world::kHudWindowCount; ++i)
    {
        const Rect& r = m_winRects[i];
        if (r.empty() || !r.contains(x, y))
            continue;
        touchHudWindow(static_cast<HudWindow>(i)); // a click in a window: the most recently used
        const Rect title{r.x, r.y, r.w - kCloseButton * s, kTitleGrab * s};
        if (e.button != MouseButton::Left || !title.contains(x, y))
            return false; // the window's own widgets get it
        m_winDrag.active = true;
        m_winDrag.window = static_cast<HudWindow>(i);
        m_winDrag.grabX = x - r.x;
        m_winDrag.grabY = y - r.y;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// The merchant beside the shop windows
// ---------------------------------------------------------------------------------------------

void WorldScene::updateShopFraming(float dt)
{
    world::Vec2 target{};
    m_merchantVisible = false;
    const float W = ctx().viewportWidth;
    const float H = std::max(ctx().viewportHeight, 1.0f);
    const float s = std::max(ui().scale(), 0.01f);
    const world::NpcEntity* npc = m_shop.isOpen() ? m_npcs.find(m_shop.npc()) : nullptr;
    const Rect shop = m_winRects[idx(HudWindow::Shop)];
    const Rect inv = m_winRects[idx(HudWindow::Inventory)];
    if (npc != nullptr && !shop.empty() && !m_winDrag.active)
    {
        // Where the merchant is without a shift.
        const world::Mat4 vp0 = m_camera.projection(W / H) * m_camera.view();
        world::Vec2 feet{};
        world::Vec2 chest{};
        if (world::projectToScreen(vp0, npc->position, W, H, feet) &&
            world::projectToScreen(vp0, npc->position + Vec3{0.0f, 1.0f, 0.0f}, W, H, chest))
        {
            float tx = chest.x;
            float ty = chest.y;
            if (!inv.empty())
            {
                const Rect& a = shop.x <= inv.x ? shop : inv;
                const Rect& b = shop.x <= inv.x ? inv : shop;
                const float gapL = a.right();
                const float gapR = b.x;
                if (gapR - gapL >= 0.75f * kMerchantRoom * s)
                {
                    tx = std::round((gapL + gapR) * 0.5f); // in the gap between the two windows
                }
                else
                {
                    // No gap: centred above the two (they sit low on such a screen), feet just over them.
                    tx = std::round(W * 0.5f);
                    ty = chest.y + (std::min(shop.y, inv.y) - 12.0f * s - feet.y);
                }
            }
            else if (chest.x < shop.right() + 60.0f * s && chest.x > shop.x - 60.0f * s)
            {
                tx = std::round((shop.right() + W) * 0.5f); // the shop alone: right of it
            }
            target.x = std::clamp(2.0f * (tx - chest.x) / W, -0.8f, 0.8f);
            target.y = std::clamp(-2.0f * (ty - chest.y) / H, -0.8f, 0.8f);
        }
    }
    const float k = world::smoothingFactor(10.0f, dt);
    m_lensShift.x += (target.x - m_lensShift.x) * k;
    m_lensShift.y += (target.y - m_lensShift.y) * k;
    if (target.x == 0.0f && target.y == 0.0f && std::fabs(m_lensShift.x) < 1e-4f && std::fabs(m_lensShift.y) < 1e-4f)
        m_lensShift = {};

    // Automation: the merchant (chest and head) on screen, outside every window and the bar.
    if (npc != nullptr)
    {
        const world::Mat4 vp = currentViewProj();
        world::Vec2 chest{};
        world::Vec2 head{};
        if (world::projectToScreen(vp, npc->position + Vec3{0.0f, 1.0f, 0.0f}, W, H, chest) &&
            world::projectToScreen(vp, npcHeadPoint(*npc), W, H, head))
        {
            const Rect screen{0.0f, 0.0f, W, H};
            const Rect bar{m_barLayout.frame.x * s, m_barLayout.frame.y * s, m_barLayout.frame.w * s, m_barLayout.frame.h * s};
            bool clear = screen.contains(chest.x, chest.y) && screen.contains(head.x, head.y) &&
                         !bar.contains(chest.x, chest.y);
            for (const Rect& r : m_winRects)
                clear = clear && !(r.contains(chest.x, chest.y) || r.contains(head.x, head.y));
            m_merchantVisible = clear;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

void WorldScene::fix1States(std::vector<std::string>& out) const
{
    if (m_shopWindow != nullptr && m_shop.isOpen())
    {
        const world::ShopListLayout& l = m_shopWindow->listLayout();
        out.emplace_back(l.scrolls ? "shopscrolls" : "shopnoscroll");
        out.push_back(std::format("shopfirst{}", l.first));
        if (m_shopWindow->greetingItalic())
            out.emplace_back("greetingitalic");
        if (m_merchantVisible)
            out.emplace_back("merchantvisible");
    }
    out.push_back(std::format("drinks{}", m_shopSeen.drinkGestures));
    out.push_back(std::format("drinknogesture{}", m_shopSeen.drinkNoGesture));
    out.push_back(std::format("winclosed{}", m_windowsClosedForRoom));
    if (m_localAnim.action() == world::AnimAction::Drink)
        out.emplace_back("drinking");
}

bool WorldScene::fix1Automation(std::string_view command, bool& handled, std::string& error)
{
    std::istringstream in{std::string(command)};
    std::string verb;
    in >> verb;
    handled = true;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    const float s = std::max(ui().scale(), 0.01f);
    if (verb == "expectnooverlap")
    {
        // Every shown side window inside the screen, off the bar and off each other.
        const Rect screen{0.0f, 0.0f, ctx().viewportWidth, ctx().viewportHeight};
        const Rect bar{m_barLayout.frame.x * s, m_barLayout.frame.y * s, m_barLayout.frame.w * s, m_barLayout.frame.h * s};
        std::string shown;
        for (std::size_t i = 0; i < world::kHudWindowCount; ++i)
        {
            const Rect& a = m_winRects[i];
            if (a.empty())
                continue;
            const char* name = world::hudWindowName(static_cast<HudWindow>(i));
            shown += std::format(" {}({:.0f},{:.0f} {:.0f}x{:.0f})", name, a.x, a.y, a.w, a.h);
            if (a.x < -0.5f || a.y < -0.5f || a.right() > screen.right() + 0.5f || a.bottom() > screen.bottom() + 0.5f)
                return fail(std::format("window {} is off the screen:{}", name, shown));
            if (world::rectsOverlap(a, bar, 0.5f))
                return fail(std::format("window {} covers the bar:{}", name, shown));
            for (std::size_t j = 0; j < i; ++j)
                if (!m_winRects[j].empty() && world::rectsOverlap(a, m_winRects[j], 0.5f))
                    return fail(std::format("windows {} and {} overlap:{}", world::hudWindowName(static_cast<HudWindow>(j)),
                                            name, shown));
        }
        VO_LOG_INFO("world: windows clear of each other and the bar:{}", shown.empty() ? std::string(" (none)") : shown);
        return true;
    }
    if (verb == "expectwindow")
    {
        // expectwindow <name> open|closed|left|right|middle|dragged|notdragged
        std::string name;
        std::string what;
        if (!(in >> name >> what))
            return fail("usage: expectwindow <shop|inv|kunye|skills|controls> open|closed|left|right|middle|dragged|notdragged");
        const auto id = parseWindow(name);
        if (!id)
            return fail("unknown window " + name);
        const Rect& r = m_winRects[idx(*id)];
        const bool open = hudWindowOpen(*id) && !r.empty();
        const float third = ctx().viewportWidth / 3.0f;
        const char* side = r.centerX() < third ? "left" : (r.centerX() > 2.0f * third ? "right" : "middle");
        bool ok = false;
        if (what == "open")
            ok = open;
        else if (what == "closed")
            ok = !hudWindowOpen(*id);
        else if (what == "left" || what == "right" || what == "middle")
            ok = open && what == side;
        else if (what == "dragged")
            ok = m_arranger.dragged(*id);
        else if (what == "notdragged")
            ok = !m_arranger.dragged(*id);
        else
            return fail("expectwindow: unknown condition " + what);
        if (!ok)
            return fail(std::format("window {}: expected {}, it is {} ({}, at {:.0f},{:.0f} {:.0f}x{:.0f}, {})", name, what,
                                    open ? "open" : "closed", side, r.x, r.y, r.w, r.h,
                                    m_arranger.dragged(*id) ? "dragged" : "placed"));
        return true;
    }
    if (verb == "dragwindow")
    {
        // dragwindow <name> <dx> <dy>: as a title-bar drag by (dx, dy) logical pixels.
        std::string name;
        float dx = 0.0f;
        float dy = 0.0f;
        if (!(in >> name >> dx >> dy))
            return fail("usage: dragwindow <name> <dx> <dy>");
        const auto id = parseWindow(name);
        if (!id || m_winRects[idx(*id)].empty())
            return fail("window not shown: " + name);
        const Rect& r = m_winRects[idx(*id)];
        m_arranger.setDragged(*id, {r.x / s + dx, r.y / s + dy}, false);
        touchHudWindow(*id);
        layoutHud();
        return true;
    }
    if (verb == "shopfakestock")
    {
        // shopfakestock <n>: n more entries in the open shop's list (client only; buying one is refused).
        int n = 0;
        if (!(in >> n) || n <= 0)
            return fail("usage: shopfakestock <n>");
        if (!m_shop.isOpen())
            return fail("no shop is open");
        const world::ItemData* items = ctx().resources.itemData();
        if (items == nullptr || !items->catalogLoaded)
            return fail("no item catalog");
        std::vector<world::ShopOffer> extra;
        for (const game::ItemTemplate& t : items->catalog.templates())
        {
            if (static_cast<int>(extra.size()) >= n)
                break;
            if (m_shop.offer(t.id) == nullptr)
                extra.push_back({t.id, std::max<std::uint32_t>(t.value, 1)});
        }
        m_shop.appendOffersForTesting(extra);
        VO_LOG_INFO("world: shop list padded with {} test entr(y/ies) ({} in all)", extra.size(), m_shop.offers().size());
        return true;
    }
    if (verb == "shopscroll")
    {
        std::size_t first = 0;
        if (!(in >> first) || m_shopWindow == nullptr || !m_shop.isOpen())
            return fail("usage: shopscroll <first row> (with a shop open)");
        m_shopWindow->scrollTo(first);
        return true;
    }
    if (verb == "expectpickup")
    {
        // expectpickup <text>: a pickup line on screen contains <text>.
        std::string text;
        std::getline(in >> std::ws, text);
        for (const Pickup& p : m_pickups)
            if (p.text.find(text) != std::string::npos)
                return true;
        std::string shown;
        for (const Pickup& p : m_pickups)
            shown += " '" + p.text + "'";
        return fail(std::format("no pickup line contains '{}' (shown:{})", text, shown.empty() ? " none" : shown));
    }
    if (verb == "expectlabellift" || verb == "expectremotelabellift")
    {
        // expectlabellift <min> <max> / expectremotelabellift <name> <min> <max>: the name label's height
        // over the feet (metres; a seated character's is lower).
        std::string name;
        if (verb == "expectremotelabellift" && !(in >> name))
            return fail("usage: expectremotelabellift <name> <min> <max>");
        float lo = 0.0f;
        float hi = 0.0f;
        if (!(in >> lo >> hi))
            return fail("usage: " + verb + (name.empty() ? "" : " <name>") + " <min> <max>");
        float lift = m_localLabelLift;
        if (!name.empty())
        {
            std::replace(name.begin(), name.end(), '_', ' ');
            const world::RemoteEntity* e = nullptr;
            for (const world::RemoteEntity& r : m_model.entities())
                if (r.active && r.name == name)
                    e = &r;
            if (e == nullptr)
                return fail("no remote named " + name);
            lift = headPoint(*e).y - e->position.y - seatedHeadDrop(e->animator);
        }
        if (lift < lo || lift > hi)
            return fail(std::format("name label {:.2f} m over the feet, expected {:.2f}..{:.2f}", lift, lo, hi));
        VO_LOG_INFO("world: name label {:.2f} m over the feet{}", lift, name.empty() ? "" : " (" + name + ")");
        return true;
    }
    if (verb == "expectuiscale")
    {
        float lo = 0.0f;
        float hi = 0.0f;
        if (!(in >> lo >> hi))
            return fail("usage: expectuiscale <min> <max>");
        if (s < lo - 1e-3f || s > hi + 1e-3f)
            return fail(std::format("UI scale {:.3f}, expected {:.2f}..{:.2f}", s, lo, hi));
        VO_LOG_INFO("world: UI scale {:.3f}, bar {:.0f}x{:.0f} px", s, m_barLayout.frame.w * s, m_barLayout.frame.h * s);
        return true;
    }
    handled = false;
    return false;
}

} // namespace vo::client::scenes
