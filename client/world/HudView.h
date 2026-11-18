// HUD logic of the world scene (the "HUD" milestone after M13, docs/CLIENT_RENDERING.md §15): the
// layout of the bottom action bar, the HUD buttons (name, key, tooltip lines), the controls reference
// of the "Tuşlar" window, the Esc order with that window, where a side window is placed so it keeps
// clear of the HUD, and the experience bar texts. Pure (no UI toolkit, no D3D): unit-tested in
// tests/client/HudViewTests.cpp; client/scenes/HudBar.h draws it.
//
// Units: everything here is in logical UI units (physical pixels / UI scale), y grows downwards.
#pragma once

#include "client/core/Geometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::client::world {

enum class EscapeAction : std::uint8_t; // LootView.h

// ---- Fixed pieces (logical, top-left corner of the screen) ----

// The player frame (level medallion, name, race · class, hp / mp bars).
inline constexpr Rect kHudPlayerFrame{12.0f, 12.0f, 300.0f, 92.0f};
// The player's effect icons start under it.
inline constexpr float kHudSelfEffectsTop = 110.0f;
// The zone name and the "Güvenli Bölge" line on the top right (from the right edge).
inline constexpr float kHudZoneTop = 14.0f;
inline constexpr float kHudZoneBottom = 64.0f;

// ---- HUD buttons ----

enum class HudButton : std::uint8_t
{
    // Action buttons (left wing of the bar).
    Attack,
    Sit,
    Loot,
    Run,
    // Menu buttons (right wing of the bar).
    Inventory,
    Kunye,
    Skills,
    Controls,
    Menu,
};
inline constexpr std::size_t kHudActionCount = 4;
inline constexpr std::size_t kHudMenuCount = 5;
inline constexpr std::array<HudButton, kHudActionCount> kHudActions = {HudButton::Attack, HudButton::Sit,
                                                                       HudButton::Loot, HudButton::Run};
inline constexpr std::array<HudButton, kHudMenuCount> kHudMenus = {HudButton::Inventory, HudButton::Kunye,
                                                                   HudButton::Skills, HudButton::Controls,
                                                                   HudButton::Menu};

struct HudButtonInfo
{
    std::string_view title;  // Turkish name ("Saldır")
    std::string_view key;    // the key as printed on the badge ("F", "Esc")
    std::string_view line;   // one short line: what it does
    std::string_view widget; // automation widget name ("hud.attack", "world.inventory")
    std::string_view icon;   // runtime art path ("ui/hud/attack.png")
};
[[nodiscard]] const HudButtonInfo& hudButtonInfo(HudButton b) noexcept;

// What a tooltip needs to know about the scene.
struct HudButtonState
{
    bool seated = false;        // Sit: sitting (or sitting down)
    bool running = true;        // Run: the default gait is running
    bool attacking = false;     // Attack: auto-attack armed
    bool hasTarget = false;     // Attack: a living monster is selected
    bool lootInRange = false;   // Loot: a bag we may take from is within reach (or the window is open)
    bool windowOpen = false;    // menu buttons: their window is open
    std::uint16_t points = 0;   // Künye: unspent stat points, Yetenekler: unspent skill points
};
struct HudTipLine
{
    enum class Tone : std::uint8_t
    {
        Title,  // gold, bold
        Text,   // normal
        State,  // a current state ("Şu an: koşu")
        Points, // gold: unspent points
        Dim,
    };
    std::string text;
    Tone tone = Tone::Text;
};
// Tooltip lines: "<title> (<key>)", the short line, then a state line where it applies. The plain
// text of a tooltip (automation) is the lines joined by '\n'.
[[nodiscard]] std::vector<HudTipLine> hudButtonTipLines(HudButton b, const HudButtonState& s);
[[nodiscard]] std::string hudTipPlainText(const std::vector<HudTipLine>& lines);
// Icon of a button for its state (the walk / run button shows the current gait).
[[nodiscard]] std::string_view hudButtonIcon(HudButton b, const HudButtonState& s) noexcept;

// ---- Bottom action bar layout ----

struct HudBarMetrics
{
    float actionButton = 40.0f; // action buttons (square)
    float actionGap = 5.0f;
    float slot = 48.0f; // skill slots (square)
    float slotGap = 6.0f;
    float menuButton = 34.0f; // menu buttons (square)
    float menuGap = 4.0f;
    float separator = 18.0f; // between the groups (a gold rule with a diamond)
    float padX = 10.0f;
    float padTop = 9.0f;
    float rowGap = 7.0f; // between the button row and the experience bar
    float xpHeight = 14.0f;
    float padBottom = 8.0f;
    float bottomMargin = 6.0f; // frame bottom to the screen bottom
    float sideMargin = 8.0f;   // minimum frame to screen edge
    float minScale = 0.6f;
};

struct HudBarLayout
{
    float k = 1.0f; // metrics scale (< 1: shrunk to fit a narrow screen)
    Rect frame;     // the framed piece (screen coordinates)
    // The rest relative to the frame's top-left corner.
    std::array<Rect, kHudActionCount> actions{};
    Rect slots; // the 8 slots as one row
    float slot = 48.0f;
    float slotGap = 6.0f;
    std::array<Rect, kHudMenuCount> menus{};
    std::array<float, 2> separators{}; // x of the two group separators (centre)
    Rect xp;                           // the experience bar
    [[nodiscard]] float top() const noexcept { return frame.y; }
    [[nodiscard]] Rect toScreen(const Rect& r) const noexcept { return {frame.x + r.x, frame.y + r.y, r.w, r.h}; }
};
// The bar at the bottom centre of a `width` x `height` screen: one row (actions | 8 slots | menu)
// and the experience bar under it; uniformly shrunk (down to minScale) when the screen is narrow.
[[nodiscard]] HudBarLayout computeHudBar(float width, float height, const HudBarMetrics& m = {});
// Unscaled width of the bar frame.
[[nodiscard]] float hudBarNaturalWidth(const HudBarMetrics& m = {}) noexcept;

// ---- HUD scale with the resolution (cleanup round fix1) ----
// The world scene's UI is laid out for a 720 pixel high screen: on taller screens it scales up in
// quarter steps (1080 -> 1.5, 1440 -> 2, 900 -> 1.25, 2160 -> 3), so the bar and the windows keep their
// share of the screen; the player's own UI scale (DPI x --ui-scale) is the minimum. Fonts are baked at
// the scaled pixel size (crisp text). The result still goes through UiScale.h fitUiScale.
inline constexpr float kHudReferenceHeight = 720.0f;
inline constexpr float kHudScaleStep = 0.25f;
[[nodiscard]] float hudResolutionScale(float heightPx, float requestedScale) noexcept;

// Where the transient bottom-centre lines go (screen y of their top / the pill's top), above the bar.
struct HudBottomLines
{
    float castBar = 0.0f;     // cast bar (18 high)
    float message = 0.0f;     // combat / zone message line
    float gateHint = 0.0f;    // the near-gate hint pill
    float pickupBottom = 0.0f; // the pickup log ends here (bottom left)
};
[[nodiscard]] HudBottomLines hudBottomLines(const HudBarLayout& bar, float messageLineHeight, float hintHeight) noexcept;

// ---- Windows next to the HUD ----

enum class WindowFit : std::uint8_t
{
    Free,       // between the top HUD and the bar: covers nothing
    OverTop,    // taller than that band: from the top of the screen down to the bar
    OverBar,    // taller still: centred on the screen, over the bar as well
};
struct WindowPlacement
{
    Rect rect;
    WindowFit fit = WindowFit::Free;
};
// A window of `size` on the left / right edge (margin `side`) or centred (`centered`): vertically
// centred between `topReserved` (bottom of the HUD above it) and `barTop`, else from the top of the
// screen down to the bar, else on the whole screen.
[[nodiscard]] WindowPlacement placeWindow(float width, float height, client::Vec2 size, bool left, bool centered,
                                          float sideMargin, float topReserved, float barTop) noexcept;
// The vertical part of placeWindow for a window whose left edge is `x` (cleanup round fix1): centred
// between `topReserved` and the bar, else from the top of the screen down to the bar, else on the whole
// screen. `bottom`: as low as it fits (just above the bar) instead of centred.
[[nodiscard]] WindowPlacement placeWindowAt(float height, float x, client::Vec2 size, float topReserved, float barTop,
                                            bool bottom = false) noexcept;
// True when two rects overlap by more than `slack` in both directions.
[[nodiscard]] bool rectsOverlap(const Rect& a, const Rect& b, float slack = 0.5f) noexcept;
// True when `r` overlaps one of `covers`: that HUD piece is hidden while the window is open.
[[nodiscard]] bool coveredByAny(const Rect& r, std::span<const Rect> covers) noexcept;

// ---- Window placement manager (cleanup round fix1) ----
//
// The side windows of the world scene never open on top of one another. Each has a preferred side
// (Künye, Yetenekler and the shop left, the inventory right, Tuşlar centred); a window that is already
// open keeps its place; a newly opened one takes its preferred place when it is free, else the next free
// place (beside an open window, the centre, the other side). Windows never cover the bar: they are
// placed in the band above it (over the top HUD if they must, which then steps aside). When there is no
// free place at all (a small screen: 1024x640 @150 % is 960x600 logical), the least recently used of the
// windows in the way closes instead (that may be the window itself when it is the older one, e.g. after
// a resize). A window the player dragged keeps that position until it conflicts with another window;
// then it is placed like any other again.
enum class HudWindow : std::uint8_t
{
    Shop,
    Inventory,
    Kunye,
    Skills,
    Controls,
};
inline constexpr std::size_t kHudWindowCount = 5;
[[nodiscard]] const char* hudWindowName(HudWindow w) noexcept; // "shop", "inv", "kunye", "skills", "controls"

enum class WindowSide : std::uint8_t
{
    Left,
    Right,
    Center,
};

struct WindowSpec
{
    bool open = false;
    client::Vec2 size{};
    WindowSide side = WindowSide::Left;
    float margin = 14.0f;      // to its screen edge
    bool bottom = false;       // as low as it fits (the shop pair on a narrow screen: the merchant above)
    std::uint64_t lastUse = 0; // larger: used more recently (opened, clicked, dragged)
};

struct WindowScreen
{
    float width = 0.0f;
    float height = 0.0f;
    float barTop = 0.0f;
    float leftTop = 0.0f;     // bottom of the player frame and its effects (over x < leftColumn)
    float rightTop = 0.0f;    // bottom of the zone name (everywhere else)
    float leftColumn = 0.0f;  // x the player frame reaches
};

struct ArrangedWindow
{
    bool shown = false; // open and placed
    Rect rect;
    WindowFit fit = WindowFit::Free;
};

class WindowArranger
{
public:
    static constexpr float kGap = 6.0f; // between two windows side by side

    struct Result
    {
        std::array<ArrangedWindow, kHudWindowCount> windows{};
        std::vector<HudWindow> close; // no room: the caller closes these (least recently used first)
    };
    // Places the open windows of `specs` (indexed by HudWindow). Remembers where each went: a window
    // stays there while it is open, the screen size and its own size do not change and nothing older
    // takes the place.
    Result arrange(const WindowScreen& screen, const std::array<WindowSpec, kHudWindowCount>& specs);

    // The player dragged the window to `topLeft` (clamped onto the screen above the bar by arrange).
    // `live`: the drag goes on (the window follows the cursor even over another one); once released, a
    // dragged window that overlaps another is placed like any other again.
    void setDragged(HudWindow w, client::Vec2 topLeft, bool live = false) noexcept;
    void clearDragged(HudWindow w) noexcept;
    [[nodiscard]] bool dragged(HudWindow w) const noexcept;
    void reset() noexcept;

private:
    enum class Anchor : std::uint8_t
    {
        LeftEdge,
        RightEdge,
        Center,
        FixedX,
    };
    struct Memory
    {
        bool placed = false;
        Anchor anchor = Anchor::LeftEdge;
        float x = 0.0f;
        client::Vec2 size{};
        bool dragged = false;
        bool live = false;
        client::Vec2 drag{};
    };
    [[nodiscard]] ArrangedWindow placeAt(const WindowScreen& s, const WindowSpec& spec, float x) const noexcept;
    [[nodiscard]] float anchorX(const WindowScreen& s, const WindowSpec& spec, Anchor a, float fixedX) const noexcept;

    std::array<Memory, kHudWindowCount> m_mem{};
    float m_screenW = -1.0f;
    float m_screenH = -1.0f;
    float m_barTop = -1.0f;
};

// ---- Experience bar ----

// "Seviye 5  ·  34 / 80  ·  %42" (or "Seviye 30  ·  en yüksek seviye").
[[nodiscard]] std::string xpBarText(std::uint16_t level, std::uint32_t xp, std::uint32_t toNext);
// Shorter, for a narrow bar: "Sv. 5  ·  %42".
[[nodiscard]] std::string xpBarShortText(std::uint16_t level, std::uint32_t xp, std::uint32_t toNext);
// Tooltip: "Deneyim (Seviye 5)", "34 / 80 (%42)", "Sonraki seviyeye 46".
[[nodiscard]] std::vector<HudTipLine> xpTipLines(std::uint16_t level, std::uint32_t xp, std::uint32_t toNext);
// 0..1 (1 at the level cap).
[[nodiscard]] float xpFraction(std::uint32_t xp, std::uint32_t toNext) noexcept;

// ---- "Tuşlar" window ----

struct ControlRow
{
    std::vector<std::string_view> keys; // alternatives, each drawn as a key cap
    std::string_view text;              // what it does
};
struct ControlGroup
{
    std::string_view title; // Hareket, Kamera, Dövüş, Pencereler, Diğer
    std::vector<ControlRow> rows;
    int column = 0; // 0 left, 1 right
};
[[nodiscard]] const std::vector<ControlGroup>& controlsReference();
// The whole reference as text (automation probe "controls.text"): "Grup: tuş / tuş — açıklama" lines.
[[nodiscard]] std::string controlsPlainText();

// Esc with the "Tuşlar" window: loot window, Tuşlar, Yetenekler, Künye, inventory, target, menu
// (None while the menu is open). Extends world::escapeActionWithSkills (SkillsView.h).
[[nodiscard]] EscapeAction escapeActionHud(bool lootOpen, bool controlsOpen, bool skillsOpen, bool kunyeOpen,
                                           bool inventoryOpen, bool hasTarget, bool menuOpen) noexcept;

} // namespace vo::client::world
