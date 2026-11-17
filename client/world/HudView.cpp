#include "client/world/HudView.h"

#include "client/world/LootView.h"
#include "client/world/SkillsView.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace vo::client::world {

namespace {

constexpr std::array<HudButtonInfo, 9> kButtons = {{
    {"Saldır", "F", "Seçili canavara saldır; menzile yürür.", "hud.attack", "ui/hud/attack.png"},
    {"Otur / Kalk", "X", "Otur ve can ile manayı daha hızlı topla.", "hud.sit", "ui/hud/sit.png"},
    {"Ganimet", "E", "Yakındaki ganimet kesesini aç; açıksa hepsini al.", "hud.loot", "ui/hud/loot.png"},
    {"Yürü / Koş", "R", "Varsayılan yürüyüşü değiştir.", "hud.run", "ui/hud/run.png"},
    {"Envanter", "I", "Eşyaların, kuşandıkların ve akçen.", "world.inventory", "ui/hud/inventory.png"},
    {"Künye", "C", "Nitelikler, değerler ve nitelik puanları.", "world.kunye", "ui/hud/kunye.png"},
    {"Yetenekler", "K", "Yetenek yolları; çubuğa sürükle.", "world.skills", "ui/hud/skills.png"},
    {"Tuşlar", "H", "Bütün tuşlar ve fare kullanımı.", "hud.controls", "ui/hud/controls.png"},
    {"Menü", "Esc", "Devam et, karakter seçimi, oyundan çık.", "hud.menu", "ui/hud/menu.png"},
}};

float r1(float v) noexcept
{
    return std::round(v);
}

} // namespace

const HudButtonInfo& hudButtonInfo(HudButton b) noexcept
{
    const auto i = static_cast<std::size_t>(b);
    return kButtons[i < kButtons.size() ? i : 0];
}

std::string_view hudButtonIcon(HudButton b, const HudButtonState& s) noexcept
{
    if (b == HudButton::Run && !s.running)
        return "ui/hud/walk.png";
    return hudButtonInfo(b).icon;
}

std::vector<HudTipLine> hudButtonTipLines(HudButton b, const HudButtonState& s)
{
    const HudButtonInfo& info = hudButtonInfo(b);
    std::vector<HudTipLine> out;
    out.push_back({std::format("{} ({})", info.title, info.key), HudTipLine::Tone::Title});
    out.push_back({std::string(info.line), HudTipLine::Tone::Text});
    switch (b)
    {
    case HudButton::Attack:
        if (s.attacking)
            out.push_back({"Şu an: saldırıyorsun", HudTipLine::Tone::State});
        else if (!s.hasTarget)
            out.push_back({"Önce bir canavar seç (sol tık / Tab).", HudTipLine::Tone::Dim});
        break;
    case HudButton::Sit:
        out.push_back({s.seated ? "Şu an: oturuyorsun" : "Şu an: ayaktasın", HudTipLine::Tone::State});
        break;
    case HudButton::Loot:
        if (s.lootInRange)
            out.push_back({"Yakında alınabilecek ganimet var.", HudTipLine::Tone::State});
        break;
    case HudButton::Run:
        out.push_back({s.running ? "Şu an: koşu (Shift basılıyken yürü)" : "Şu an: yürüyüş (Shift basılıyken koş)",
                       HudTipLine::Tone::State});
        break;
    case HudButton::Kunye:
        if (s.points > 0)
            out.push_back({std::format("Harcanmamış nitelik puanı: {}", s.points), HudTipLine::Tone::Points});
        break;
    case HudButton::Skills:
        if (s.points > 0)
            out.push_back({skillPointsText(s.points), HudTipLine::Tone::Points});
        break;
    default: break;
    }
    if (s.windowOpen && (b == HudButton::Inventory || b == HudButton::Kunye || b == HudButton::Skills ||
                         b == HudButton::Controls))
        out.push_back({"Açık: tıkla ya da tuşa bas, kapansın.", HudTipLine::Tone::Dim});
    return out;
}

std::string hudTipPlainText(const std::vector<HudTipLine>& lines)
{
    std::string out;
    for (const HudTipLine& l : lines)
    {
        if (!out.empty())
            out += '\n';
        out += l.text;
    }
    return out;
}

// ---- Bar layout ----

float hudBarNaturalWidth(const HudBarMetrics& m) noexcept
{
    const float actions = 4.0f * m.actionButton + 3.0f * m.actionGap;
    const float slots = 8.0f * m.slot + 7.0f * m.slotGap;
    const float menus = 5.0f * m.menuButton + 4.0f * m.menuGap;
    return 2.0f * m.padX + actions + m.separator + slots + m.separator + menus;
}

HudBarLayout computeHudBar(float width, float height, const HudBarMetrics& m)
{
    HudBarLayout l;
    const float natural = hudBarNaturalWidth(m);
    const float avail = std::max(1.0f, width - 2.0f * m.sideMargin);
    float k = std::clamp(avail / natural, m.minScale, 1.0f);
    float action = 0.0f, actionGap = 0.0f, menu = 0.0f, menuGap = 0.0f, sep = 0.0f, padX = 0.0f, padTop = 0.0f;
    float rowGap = 0.0f, xpH = 0.0f, padBottom = 0.0f, slotsW = 0.0f, frameW = 0.0f;
    // Whole logical units per piece; the rounding may add a few units: shrink a little more then.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        action = r1(m.actionButton * k);
        actionGap = std::max(2.0f, r1(m.actionGap * k));
        l.slot = r1(m.slot * k);
        l.slotGap = std::max(2.0f, r1(m.slotGap * k));
        menu = r1(m.menuButton * k);
        menuGap = std::max(2.0f, r1(m.menuGap * k));
        sep = r1(m.separator * k);
        padX = r1(m.padX * k);
        padTop = r1(m.padTop * k);
        rowGap = r1(m.rowGap * k);
        xpH = std::max(10.0f, r1(m.xpHeight * k));
        padBottom = r1(m.padBottom * k);
        slotsW = 8.0f * l.slot + 7.0f * l.slotGap;
        frameW = 2.0f * padX + 4.0f * action + 3.0f * actionGap + sep + slotsW + sep + 5.0f * menu + 4.0f * menuGap;
        if (frameW <= avail || k <= m.minScale)
            break;
        k = std::max(m.minScale, k - 0.005f);
    }
    l.k = k;
    const float frameH = padTop + l.slot + rowGap + xpH + padBottom;
    l.frame = {r1((width - frameW) * 0.5f), r1(height - m.bottomMargin - frameH), frameW, frameH};

    const float rowY = padTop;
    float x = padX;
    for (std::size_t i = 0; i < kHudActionCount; ++i)
    {
        l.actions[i] = {x, rowY + r1((l.slot - action) * 0.5f), action, action};
        x += action + (i + 1 < kHudActionCount ? actionGap : 0.0f);
    }
    l.separators[0] = x + sep * 0.5f;
    x += sep;
    l.slots = {x, rowY, slotsW, l.slot};
    x += slotsW;
    l.separators[1] = x + sep * 0.5f;
    x += sep;
    for (std::size_t i = 0; i < kHudMenuCount; ++i)
    {
        l.menus[i] = {x, rowY + r1((l.slot - menu) * 0.5f), menu, menu};
        x += menu + (i + 1 < kHudMenuCount ? menuGap : 0.0f);
    }
    l.xp = {padX, rowY + l.slot + rowGap, frameW - 2.0f * padX, xpH};
    return l;
}

HudBottomLines hudBottomLines(const HudBarLayout& bar, float messageLineHeight, float hintHeight) noexcept
{
    HudBottomLines out;
    const float top = bar.top();
    out.castBar = top - 10.0f - 18.0f;
    out.message = out.castBar - 12.0f - messageLineHeight;
    out.gateHint = out.message - 10.0f - hintHeight;
    out.pickupBottom = top - 10.0f;
    return out;
}

// ---- Windows ----

WindowPlacement placeWindow(float width, float height, client::Vec2 size, bool left, bool centered, float sideMargin,
                            float topReserved, float barTop) noexcept
{
    constexpr float kGap = 4.0f;
    WindowPlacement p;
    float x = left ? sideMargin : width - sideMargin - size.x;
    if (centered)
        x = (width - size.x) * 0.5f;
    x = std::max(0.0f, x);
    const auto centreIn = [&](float top, float bottom) { return top + std::max(0.0f, (bottom - top - size.y) * 0.5f); };
    const float freeTop = topReserved + kGap;
    const float freeBottom = barTop - kGap;
    float y = 0.0f;
    if (size.y <= freeBottom - freeTop)
    {
        y = centreIn(freeTop, freeBottom);
        p.fit = WindowFit::Free;
    }
    else if (size.y <= freeBottom - kGap)
    {
        y = centreIn(kGap, freeBottom);
        p.fit = WindowFit::OverTop;
    }
    else
    {
        y = std::max(0.0f, (height - size.y) * 0.5f);
        p.fit = WindowFit::OverBar;
    }
    p.rect = {r1(x), r1(y), size.x, size.y};
    return p;
}

bool rectsOverlap(const Rect& a, const Rect& b, float slack) noexcept
{
    if (a.empty() || b.empty())
        return false;
    const Rect i = a.intersect(b);
    return i.w > slack && i.h > slack;
}

bool coveredByAny(const Rect& r, std::span<const Rect> covers) noexcept
{
    return std::any_of(covers.begin(), covers.end(), [&](const Rect& c) { return rectsOverlap(r, c); });
}

// ---- Experience ----

float xpFraction(std::uint32_t xp, std::uint32_t toNext) noexcept
{
    if (toNext == 0)
        return 1.0f;
    return std::clamp(static_cast<float>(xp) / static_cast<float>(toNext), 0.0f, 1.0f);
}

namespace {

int xpPercent(std::uint32_t xp, std::uint32_t toNext) noexcept
{
    if (toNext == 0)
        return 100;
    // Floor: "%100" only once it is really full.
    return static_cast<int>(std::min<std::uint64_t>(99, static_cast<std::uint64_t>(xp) * 100u / toNext));
}

} // namespace

std::string xpBarText(std::uint16_t level, std::uint32_t xp, std::uint32_t toNext)
{
    if (toNext == 0)
        return std::format("Seviye {}  ·  en yüksek seviye", level);
    return std::format("Seviye {}  ·  {} / {}  ·  %{}", level, xp, toNext, xpPercent(xp, toNext));
}

std::string xpBarShortText(std::uint16_t level, std::uint32_t xp, std::uint32_t toNext)
{
    if (toNext == 0)
        return std::format("Sv. {}", level);
    return std::format("Sv. {}  ·  %{}", level, xpPercent(xp, toNext));
}

std::vector<HudTipLine> xpTipLines(std::uint16_t level, std::uint32_t xp, std::uint32_t toNext)
{
    std::vector<HudTipLine> out;
    out.push_back({std::format("Deneyim (Seviye {})", level), HudTipLine::Tone::Title});
    if (toNext == 0)
    {
        out.push_back({"En yüksek seviyedesin.", HudTipLine::Tone::Text});
        return out;
    }
    out.push_back({std::format("{} / {} (%{})", xp, toNext, xpPercent(xp, toNext)), HudTipLine::Tone::Text});
    out.push_back({std::format("Sonraki seviyeye {}", toNext > xp ? toNext - xp : 0u), HudTipLine::Tone::Dim});
    return out;
}

// ---- Controls reference ----

const std::vector<ControlGroup>& controlsReference()
{
    static const std::vector<ControlGroup> groups = {
        {"Hareket",
         {
             {{"W", "A", "S", "D"}, "Yürü / koş (kameraya göre)"},
             {{"Ok tuşları"}, "Aynı hareket"},
             {{"Sol tık"}, "Yere tıkla: oraya git"},
             {{"Sol tık"}, "Basılı tut: oraya git, imleci izle"},
             {{"Shift"}, "Basılıyken yürü (koşmaz)"},
             {{"R"}, "Koşu / yürüyüş arasında geç"},
             {{"Boşluk"}, "Zıpla"},
             {{"X"}, "Otur / kalk"},
         },
         0},
        {"Kamera",
         {
             {{"Sağ tık"}, "Basılı tut, sürükle: kamerayı çevir"},
             {{"PgUp", "PgDn"}, "Kamera açısı: yukarı / aşağı"},
             {{"Tekerlek"}, "Yakınlaş / uzaklaş"},
         },
         0},
        // M14 (docs/PROTOCOL_SHOP.md §8): merchants and potions.
        {"Pazar",
         {
             {{"Sol tık"}, "Satıcıya tıkla: yanına git, dükkânı aç"},
             {{"E"}, "Yakındaki satıcıyla konuş (ganimet yoksa)"},
             {{"Sağ tık"}, "Dükkân açıkken: çantadaki eşyayı sat"},
             {{"Çift tık"}, "Çantadaki iksiri iç (sürükle: çubuğa koy)"},
         },
         0},
        {"Diğer",
         {
             {{"F3"}, "Teknik bilgi paneli"},
             {{"Esc"}, "Menü (pencere ve hedef yoksa)"},
         },
         0},
        {"Dövüş",
         {
             {{"Sol tık"}, "Hedef seç"},
             {{"Sağ tık", "Çift tık", "F"}, "Saldır (menzile yürür)"},
             {{"Tab"}, "Sıradaki hedef"},
             {{"1 – 8"}, "Yetenek çubuğu"},
             {{"Sürükle"}, "Yeteneği çubukta taşı"},
             {{"Sağ tık"}, "Çubuktaki yuvayı boşalt"},
             {{"E"}, "Ganimet: aç / hepsini al"},
             {{"Esc"}, "Hedefi bırak"},
         },
         1},
        {"Pencereler",
         {
             {{"I"}, "Envanter"},
             {{"C"}, "Künye"},
             {{"K"}, "Yetenekler"},
             {{"H"}, "Tuşlar (bu pencere)"},
             {{"Esc"}, "Açık pencereyi kapat"},
         },
         1},
    };
    return groups;
}

std::string controlsPlainText()
{
    std::string out;
    for (const ControlGroup& g : controlsReference())
        for (const ControlRow& r : g.rows)
        {
            std::string keys;
            for (const std::string_view k : r.keys)
                keys += (keys.empty() ? "" : " / ") + std::string(k);
            out += std::format("{}: {} — {}\n", g.title, keys, r.text);
        }
    return out;
}

EscapeAction escapeActionHud(bool lootOpen, bool controlsOpen, bool skillsOpen, bool kunyeOpen, bool inventoryOpen,
                             bool hasTarget, bool menuOpen) noexcept
{
    if (menuOpen)
        return EscapeAction::None;
    if (lootOpen)
        return EscapeAction::CloseLoot;
    if (controlsOpen)
        return EscapeAction::CloseControls;
    return escapeActionWithSkills(false, skillsOpen, kunyeOpen, inventoryOpen, hasTarget, false);
}

} // namespace vo::client::world
