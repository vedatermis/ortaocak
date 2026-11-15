#include "client/scenes/CharacterPreview.h"

#include "client/render/AssetCache.h"
#include "client/render/RenderDevice.h"
#include "client/render/RenderTarget.h"
#include "client/render/SpriteBatch.h"
#include "client/render/TextRenderer.h"
#include "client/render/Texture2D.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/SceneResources.h"
#include "client/ui/DrawContext.h"
#include "client/world/WorldMath.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace {

constexpr std::uint8_t kAydor = static_cast<std::uint8_t>(game::Race::Aydor);
constexpr std::uint8_t kKargan = static_cast<std::uint8_t>(game::Race::Kargan);

struct Shape
{
    Rect rect;
    float radius;
};

// Up to a few dozen rounded shapes making up one silhouette.
class Silhouette
{
public:
    void add(float x, float y, float w, float h, float radius)
    {
        if (m_count < m_shapes.size())
            m_shapes[m_count++] = {{x, y, w, h}, radius};
    }
    void circle(float cx, float cy, float r) { add(cx - r, cy - r, 2.0f * r, 2.0f * r, r); }
    void capsule(float x, float y, float w, float h) { add(x, y, w, h, std::min(w, h) * 0.5f); }

    // Rim light (shapes grown by `rim`) first, then the dark body on top.
    void draw(render::SpriteBatch& b, float rim, const Color& rimColor, const Color& body) const
    {
        for (std::size_t i = 0; i < m_count; ++i)
        {
            const Shape& s = m_shapes[i];
            b.fillRoundedRect(s.rect.inset(-rim), s.radius + rim, rimColor);
        }
        for (std::size_t i = 0; i < m_count; ++i)
            b.fillRoundedRect(m_shapes[i].rect, m_shapes[i].radius, body);
    }

private:
    std::array<Shape, 32> m_shapes{};
    std::size_t m_count = 0;
};

void fillCircle(render::SpriteBatch& b, float cx, float cy, float r, const Color& c)
{
    b.fillRoundedRect({cx - r, cy - r, 2.0f * r, 2.0f * r}, r, c);
}

void drawSky(render::SpriteBatch& b, const Rect& r, std::uint8_t race)
{
    if (race == kAydor)
    {
        // Twilight blue sky with the pale sun of the Aydor banner rising behind the figure.
        b.fillRectGradient(r, Color::hex(0x2A3B5C), Color::hex(0x0B1220));
        fillCircle(b, r.centerX(), r.y + r.h * 0.30f, r.h * 0.24f, Color::hex(0xE8C36A, 0.05f));
        fillCircle(b, r.centerX(), r.y + r.h * 0.30f, r.h * 0.17f, Color::hex(0xF1D48A, 0.07f));
        fillCircle(b, r.centerX(), r.y + r.h * 0.30f, r.h * 0.11f, Color::hex(0xF8E3A8, 0.10f));
        b.fillRectGradient({r.x, r.y + r.h * 0.72f, r.w, r.h * 0.28f}, Color::hex(0x101826, 0.0f),
                           Color::hex(0x080C14, 0.9f));
    }
    else if (race == kKargan)
    {
        // Dark mountain halls lit by forge embers.
        b.fillRectGradient(r, Color::hex(0x3B2618), Color::hex(0x100A07));
        const float base = r.y + r.h * 0.92f;
        b.fillDiamond({r.x + r.w * 0.22f, base}, r.h * 0.48f, Color::hex(0x24170F));
        b.fillDiamond({r.x + r.w * 0.80f, base}, r.h * 0.56f, Color::hex(0x1C120C));
        b.fillDiamond({r.x + r.w * 0.52f, base}, r.h * 0.36f, Color::hex(0x2A1B12));
        b.fillRectGradient({r.x, r.y + r.h * 0.62f, r.w, r.h * 0.38f}, Color::hex(0xC2561F, 0.0f),
                           Color::hex(0xC2561F, 0.28f));
    }
    else
    {
        b.fillRectGradient(r, Color::hex(0x2A313B), Color::hex(0x0E1116));
    }
}

void drawFigure(render::SpriteBatch& b, const Rect& r, const CharacterLook& look, float scale)
{
    const bool kargan = look.race == kKargan;
    const float u = std::min(r.w * 0.95f, r.h);
    const float cx = std::round(r.centerX());
    const float baseY = r.y + r.h * 0.94f;
    const float height = (kargan ? 0.70f : 0.80f) * u;
    const float top = baseY - height;

    // Ground shadow.
    b.fillRoundedRect({cx - 0.24f * u, baseY - 0.02f * u, 0.48f * u, 0.045f * u}, 0.022f * u,
                      Color::hex(0x000000, 0.55f));

    const float headR = (kargan ? 0.072f : 0.064f) * u;
    const float headY = top + headR + 0.012f * u;
    const float shoulderW = (kargan ? 0.44f : 0.31f) * u;
    const float torsoW = (kargan ? 0.33f : 0.22f) * u;
    const float torsoTop = headY + headR + 0.02f * u;
    const float torsoH = (kargan ? 0.26f : 0.29f) * u;
    const float armW = (kargan ? 0.09f : 0.062f) * u;
    const float armH = (kargan ? 0.27f : 0.31f) * u;
    const float legW = (kargan ? 0.115f : 0.085f) * u;
    const float hipsTop = torsoTop + torsoH - 0.03f * u;
    const float legTop = hipsTop + 0.05f * u;
    const float legH = baseY - legTop;
    const float armX = shoulderW * 0.5f - armW * 0.85f; // arms overlap the torso: no rim gap

    Silhouette s;
    s.add(cx - headR * 1.05f, headY - headR * 1.12f, headR * 2.1f, headR * 1.1f, headR); // hair
    if (look.charClass == static_cast<std::uint8_t>(game::CharClass::Rogue))
        s.circle(cx, headY - headR * 0.1f, headR * 1.35f); // hood
    s.circle(cx, headY, headR);
    if (kargan) // heavy jaw with the two tusks' outline
        s.add(cx - headR * 0.8f, headY + headR * 0.2f, headR * 1.6f, headR * 1.15f, headR * 0.55f);
    s.capsule(cx - 0.028f * u, headY + headR * 0.6f, 0.056f * u, 0.07f * u);           // neck
    if (kargan) // fur mantle over the shoulders
        s.add(cx - shoulderW * 0.56f, torsoTop - 0.025f * u, shoulderW * 1.12f, 0.15f * u, 0.07f * u);
    else // long cape behind the body
        s.add(cx - shoulderW * 0.52f, torsoTop + 0.01f * u, shoulderW * 1.04f, (legTop - torsoTop) + legH * 0.78f,
              0.05f * u);
    s.add(cx - shoulderW * 0.5f, torsoTop, shoulderW, 0.10f * u, 0.045f * u);         // shoulders
    s.add(cx - torsoW * 0.5f, torsoTop + 0.03f * u, torsoW, torsoH, 0.05f * u);       // torso
    s.capsule(cx - armX - armW, torsoTop + 0.02f * u, armW, armH);                    // arms
    s.capsule(cx + armX, torsoTop + 0.02f * u, armW, armH);
    s.add(cx - torsoW * 0.55f, hipsTop, torsoW * 1.1f, 0.09f * u, 0.035f * u);        // hips
    s.capsule(cx - legW - 0.008f * u, legTop, legW, legH);                            // legs
    s.capsule(cx + 0.008f * u, legTop, legW, legH);
    if (look.charClass == static_cast<std::uint8_t>(game::CharClass::Mage) ||
        look.charClass == static_cast<std::uint8_t>(game::CharClass::Priest))
        s.add(cx - torsoW * 0.62f, hipsTop, torsoW * 1.24f, legH * 0.85f + 0.05f * u, 0.04f * u); // robe

    const Color rim = kargan ? Color::hex(0xF08A3C, 0.55f) : Color::hex(0xE6C77A, 0.55f);
    s.draw(b, std::max(1.0f, 1.6f * scale), rim, Color::hex(0x0A0C10)); // body must be opaque

    const Color eye = Color::hex(kargan ? 0xFFB070 : 0xBFD8FF, 0.9f); // eye glint
    const float eyeR = std::max(1.0f, 0.0075f * u);
    fillCircle(b, cx - headR * 0.38f, headY - headR * 0.05f, eyeR, eye);
    fillCircle(b, cx + headR * 0.38f, headY - headR * 0.05f, eyeR, eye);

    // Class prop in the right hand (and a shield for warriors).
    const float handX = cx + armX + armW * 0.5f;
    const float handY = torsoTop + armH;
    const float lw = std::max(1.5f, 0.018f * u);
    switch (static_cast<game::CharClass>(look.charClass))
    {
    case game::CharClass::Warrior:
    {
        const Color steel = Color::hex(0xC9CED6);
        b.drawLine({handX, handY}, {handX + 0.03f * u, handY - 0.36f * u}, lw * 1.2f, steel, true);
        b.drawLine({handX - 0.05f * u, handY - 0.02f * u}, {handX + 0.05f * u, handY - 0.01f * u}, lw,
                   Color::hex(0xB08A3E), true);
        const Rect shield{cx - armX - armW - 0.10f * u, torsoTop + 0.06f * u, 0.15f * u, 0.22f * u};
        b.fillRoundedRect(shield.inset(-std::max(1.0f, 1.5f * scale)), 0.04f * u, Color::hex(0xD9AE4A, 0.8f));
        b.fillRoundedRect(shield, 0.04f * u, Color::hex(0x4A3218));
        b.fillDiamond({shield.centerX(), shield.centerY()}, 0.03f * u, Color::hex(0xD9AE4A, 0.9f));
        break;
    }
    case game::CharClass::Rogue:
    {
        const Color steel = Color::hex(0xB8C0CA);
        b.drawLine({handX, handY - 0.01f * u}, {handX + 0.05f * u, handY + 0.10f * u}, lw, steel, true);
        const float leftX = cx - armX - armW * 0.5f;
        b.drawLine({leftX, handY - 0.01f * u}, {leftX - 0.05f * u, handY + 0.10f * u}, lw, steel, true);
        break;
    }
    case game::CharClass::Mage:
    {
        b.drawLine({handX + 0.02f * u, baseY - 0.01f * u}, {handX + 0.02f * u, top - 0.01f * u}, lw,
                   Color::hex(0x8A6A42), true);
        fillCircle(b, handX + 0.02f * u, top - 0.04f * u, 0.075f * u, Color::hex(0x7FB7FF, 0.22f));
        fillCircle(b, handX + 0.02f * u, top - 0.04f * u, 0.035f * u, Color::hex(0x9CCBFF));
        break;
    }
    case game::CharClass::Priest:
    {
        b.drawLine({handX + 0.02f * u, baseY - 0.01f * u}, {handX + 0.02f * u, top + 0.02f * u}, lw,
                   Color::hex(0xD8C9A0), true);
        fillCircle(b, handX + 0.02f * u, top - 0.01f * u, 0.07f * u, Color::hex(0xF2D27A, 0.20f));
        fillCircle(b, handX + 0.02f * u, top - 0.01f * u, 0.04f * u, Color::hex(0xF2D27A));
        fillCircle(b, handX + 0.02f * u, top - 0.01f * u, 0.022f * u, Color::hex(0x2A2416));
        break;
    }
    }
}

} // namespace

bool expectPreviewMesh(const CharacterPreview& preview, const render::AssetCache* assets, std::uint8_t race, int look,
                       std::string& error)
{
    if (assets == nullptr || !assets->hasRoot())
    {
        error = "no assets";
        return false;
    }
    if (!preview.showing3d())
    {
        error = "the preview does not show the 3D model";
        return false;
    }
    const std::string& expected = assets->characterMeshPath(race, look);
    if (preview.shownMesh() != expected)
    {
        error = std::format("preview shows '{}', expected '{}' (race {}, look {})", preview.shownMesh(), expected, race,
                            look);
        return false;
    }
    const world::CharacterLookTable& looks = assets->looks();
    const std::string own = std::format("characters/{}_{}.vmesh", world::raceAssetName(race), look);
    if (looks.available(race, look) && !looks.files(race, look).legacy && expected != own)
    {
        error = std::format("look {} has its own mesh '{}' but '{}' is shown", look, own, expected);
        return false;
    }
    VO_LOG_INFO("preview: race {} look {} -> {} ({} look(s), effective look {})", race, look, expected,
                looks.lookCount(race), looks.effectiveLook(race, look));
    return true;
}

std::string describeCharacter(std::uint8_t race, std::uint8_t charClass, std::uint16_t level)
{
    std::string out;
    auto append = [&out](std::string_view part)
    {
        if (!out.empty())
            out += " · ";
        out += part;
    };
    if (game::isValidRace(race))
        append(game::raceName(static_cast<game::Race>(race)));
    if (game::isValidClass(charClass))
        append(game::className(static_cast<game::CharClass>(charClass)));
    if (level > 0)
        append("Seviye " + std::to_string(level));
    return out;
}

void drawCharacterArt(render::SpriteBatch& batch, const ui::Theme&, const render::Texture2D* portrait,
                      const CharacterLook& look, const Rect& rect, float scale)
{
    if (rect.empty())
        return;
    batch.pushClip(rect);
    if (portrait != nullptr && portrait->valid())
    {
        // Contain-fit (the whole figure stays visible) on a dark backing; the image edges fade into it.
        const Color backing = Color::hex(0x15181C);
        batch.fillRect(rect, backing);
        const float iw = static_cast<float>(portrait->width());
        const float ih = static_cast<float>(portrait->height());
        Rect dst = rect;
        if (iw / ih > rect.w / rect.h)
        {
            const float h = rect.w * ih / iw;
            dst = Rect{rect.x, rect.y + (rect.h - h) * 0.5f, rect.w, h}.snapped();
        }
        else
        {
            const float w = rect.h * iw / ih;
            dst = Rect{rect.x + (rect.w - w) * 0.5f, rect.y, w, rect.h}.snapped();
        }
        batch.drawImage(*portrait, dst);
        constexpr int kStrips = 10;
        if (dst.x > rect.x + 0.5f)
        {
            const float strip = std::ceil(dst.w * 0.16f / static_cast<float>(kStrips));
            for (int i = 0; i < kStrips; ++i)
            {
                const float t = 1.0f - static_cast<float>(i) / static_cast<float>(kStrips);
                const Color c = backing.withAlpha(t * t);
                const float off = static_cast<float>(i) * strip;
                batch.fillRect({dst.x + off, dst.y, strip, dst.h}, c);
                batch.fillRect({dst.right() - off - strip, dst.y, strip, dst.h}, c);
            }
        }
        if (dst.y > rect.y + 0.5f)
        {
            const float band = std::round(dst.h * 0.10f);
            batch.fillRectGradient({dst.x, dst.y, dst.w, band}, backing, backing.withAlpha(0.0f));
            batch.fillRectGradient({dst.x, dst.bottom() - band, dst.w, band}, backing.withAlpha(0.0f), backing);
        }
    }
    else
    {
        drawSky(batch, rect, look.race);
        if (game::isValidRace(look.race))
            drawFigure(batch, rect, look, scale);
    }
    batch.popClip();
}

CharacterPreview::CharacterPreview(const SceneResources& resources) : m_resources(resources) {}

CharacterPreview::~CharacterPreview() = default;

void CharacterPreview::update(double dt)
{
    ui::Widget::update(dt);
    m_time += dt;
    if (m_animator.bound())
        m_animator.update(static_cast<float>(dt), 0.0f, false);
}

bool CharacterPreview::onMouseDown(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    m_dragging = true;
    m_dragX = static_cast<float>(e.x);
    return true;
}

bool CharacterPreview::onMouseMove(const platform::InputEvent& e)
{
    if (!m_dragging || !hasCapture())
        return false;
    m_yaw = world::wrapAngle(m_yaw + (static_cast<float>(e.x) - m_dragX) * 0.012f);
    m_dragX = static_cast<float>(e.x);
    return true;
}

bool CharacterPreview::onMouseUp(const platform::InputEvent& e)
{
    if (e.button != platform::MouseButton::Left)
        return false;
    m_dragging = false;
    return true;
}

bool CharacterPreview::render3d(const Rect& art)
{
    const PreviewServices* svc = m_resources.previewServices();
    if (svc == nullptr || svc->device == nullptr || svc->renderer == nullptr || svc->assets == nullptr ||
        !svc->assets->hasRoot() || !m_look || !game::isValidRace(m_look->race) || art.empty())
        return false;
    render::AssetCache& cache = *svc->assets;
    cache.loadCharacterNow(m_look->race, m_look->look); // no-op once loaded
    const std::string& meshPath = cache.characterMeshPath(m_look->race, m_look->look);
    const render::GpuMesh* mesh = cache.mesh(meshPath);
    const world::CharacterClips* clips = cache.characterClips(m_look->race, m_look->look);
    if (mesh == nullptr || !mesh->cpu || !mesh->skinned)
    {
        if (!m_failed3d)
            VO_LOG_WARN("preview: character mesh {} (race {}, look {}) unavailable; showing the 2D art", meshPath,
                        m_look->race, m_look->look);
        m_failed3d = true;
        return false;
    }
    if (m_animMesh != mesh || m_animClips != clips)
    {
        // Another race or look: rebind at once (each look has its own skeleton and clips).
        m_animator.bind(mesh->cpu.get(), clips);
        m_animator.setState(world::AnimState::Idle);
        m_animMesh = mesh;
        m_animClips = clips;
    }
    m_animator.evaluate();

    std::string error;
    if (!m_target)
        m_target = std::make_unique<render::RenderTarget>();
    if (!m_target->ensure(svc->device->device(), static_cast<std::uint32_t>(art.w), static_cast<std::uint32_t>(art.h),
                          "preview.target", error))
    {
        if (!m_failed3d)
            VO_LOG_WARN("preview: {}", error);
        m_failed3d = true;
        return false;
    }

    // Camera framing the whole figure (bind-pose height), slightly from above.
    const float height = std::max(mesh->cpu->boundsMax.y, 1.0f);
    const float aspect = art.w / std::max(art.h, 1.0f);
    const float fovY = 30.0f * world::kPi / 180.0f;
    const float halfTan = std::tan(fovY * 0.5f);
    const float distV = (height * 0.66f) / halfTan;
    const float distH = 0.85f / (halfTan * aspect);
    const float dist = std::max(distV, distH);
    const world::Vec3 target{0.0f, height * 0.47f, 0.0f};
    const world::Vec3 eye{0.0f, target.y + dist * 0.12f, dist};

    render::WorldRenderer& r = *svc->renderer;
    const auto savedHorizon = r.horizonColor;
    const auto savedZenith = r.zenithColor;
    const bool kargan = m_look->race == static_cast<std::uint8_t>(game::Race::Kargan);
    // Dusky studio backdrop in the race's colours (the sky shader below/above the horizon line).
    r.horizonColor = kargan ? std::array<float, 3>{0.16f, 0.075f, 0.035f} : std::array<float, 3>{0.075f, 0.10f, 0.16f};
    r.zenithColor = kargan ? std::array<float, 3>{0.02f, 0.01f, 0.006f} : std::array<float, 3>{0.01f, 0.015f, 0.035f};

    render::ViewSetup view;
    view.view = world::lookAtLH(eye, target, {0.0f, 1.0f, 0.0f});
    view.projection = world::perspectiveReverseZ(fovY, aspect, 0.1f, 100.0f);
    view.eye = eye;
    view.time = static_cast<float>(m_time);
    view.drawTerrain = false;
    view.shadows = false;
    r.beginFrame(view);
    render::CharacterDraw d;
    d.mesh = mesh;
    d.skin = m_animator.skinMatrices();
    d.world = world::makeWorld({0.0f, 0.0f, 0.0f}, m_yaw, 1.0f);
    r.addCharacter(d);
    render::DecalDraw pool; // warm light pool on the floor
    pool.radius = 1.5f;
    pool.color = kargan ? std::array<float, 4>{0.75f, 0.36f, 0.14f, 0.55f}
                        : std::array<float, 4>{0.55f, 0.52f, 0.40f, 0.50f};
    r.addDecal(pool);
    render::DecalDraw blob;
    blob.radius = 0.6f;
    blob.color = {0.0f, 0.0f, 0.0f, 0.6f};
    r.addDecal(blob);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_target->clear(svc->device->context(), clear);
    r.render(m_target->views());
    r.horizonColor = savedHorizon;
    r.zenithColor = savedZenith;
    svc->device->bindBackBuffer();
    m_shownMesh = meshPath;
    return true;
}

void CharacterPreview::setLook(std::optional<CharacterLook> look)
{
    m_look = std::move(look);
}

void CharacterPreview::setEmptyText(std::string text)
{
    m_emptyText = std::move(text);
}

void CharacterPreview::drawSelf(ui::DrawContext& ctx)
{
    const ui::Theme& t = ctx.theme;
    auto& b = ctx.batch;
    const Rect r = rect();
    if (r.empty())
        return;
    const float radius = ctx.pxf(t.cornerRadius);
    const float border = std::max(1.0f, ctx.pxf(t.borderWidth));
    const float inset = ctx.px(4.0f);
    const Rect art = r.inset(inset);

    b.fillRoundedRect(r, radius, t.insetFill);
    m_shown3d = false;
    m_shownMesh.clear();
    if (m_look)
    {
        const Rect artPx = art.snapped();
        if (render3d(artPx))
        {
            b.drawQuad(m_target->srv(), artPx, {0.0f, 0.0f}, {1.0f, 1.0f}, colors::kWhite, false);
            m_shown3d = true;
        }
        else
        {
            drawCharacterArt(b, t, m_resources.racePortrait(m_look->race), *m_look, art, ctx.scale);
        }
    }
    else
    {
        b.fillRectGradient(art, Color::hex(0x1E242C), Color::hex(0x0B0E12));
        const render::Font& font = ctx.font(ui::TextRole::Large);
        render::TextStyle style;
        style.color = t.textDim;
        style.shadow = true;
        style.shadowColor = t.textShadow;
        render::drawTextAligned(b, font, m_emptyText, art, render::HAlign::Center, render::VAlign::Middle, style);
    }

    if (m_look && m_caption)
    {
        const float bandH = ctx.px(72.0f);
        const Rect band{art.x, art.bottom() - bandH, art.w, bandH};
        b.fillRectGradient(band, Color::hex(0x000000, 0.0f), Color::hex(0x000000, 0.78f));
        render::TextStyle nameStyle;
        nameStyle.color = t.textTitle;
        nameStyle.shadow = true;
        nameStyle.shadowColor = t.textShadow;
        nameStyle.shadowOffset = std::max(1.0f, std::round(ctx.scale));
        const render::Font& nameFont = ctx.font(24.0f, render::FontWeight::Bold);
        const render::Font& infoFont = ctx.font(ui::TextRole::Normal);
        const std::string info = describeCharacter(m_look->race, m_look->charClass, m_look->level);
        const float gap = ctx.px(2.0f);
        const float textH = (m_look->name.empty() ? 0.0f : nameFont.lineHeight() + gap) + infoFont.lineHeight();
        float y = std::round(band.bottom() - ctx.px(12.0f) - textH);
        if (!m_look->name.empty())
        {
            render::drawTextAligned(b, nameFont, m_look->name, {band.x, y, band.w, nameFont.lineHeight()},
                                    render::HAlign::Center, render::VAlign::Top, nameStyle);
            y += nameFont.lineHeight() + gap;
        }
        render::TextStyle infoStyle = nameStyle;
        infoStyle.color = t.text;
        render::drawTextAligned(b, infoFont, info, {band.x, y, band.w, infoFont.lineHeight()},
                                render::HAlign::Center, render::VAlign::Top, infoStyle);
    }

    // Frame: bronze border, inner bevel and gold corner brackets.
    b.strokeRoundedRect(r, radius, border, t.panelBorder);
    b.strokeRoundedRect(art, std::max(0.0f, radius - inset * 0.5f), border, Color::hex(0x000000, 0.6f));
    const float len = ctx.px(14.0f);
    const float o = ctx.px(8.0f);
    const float w = std::max(1.0f, ctx.pxf(1.5f));
    const Color c = t.ornament.withAlpha(0.9f);
    const float l = r.x + o;
    const float tp = r.y + o;
    const float rt = r.right() - o;
    const float bt = r.bottom() - o;
    b.drawLine({l, tp}, {l + len, tp}, w, c);
    b.drawLine({l, tp}, {l, tp + len}, w, c);
    b.drawLine({rt, tp}, {rt - len, tp}, w, c);
    b.drawLine({rt, tp}, {rt, tp + len}, w, c);
    b.drawLine({l, bt}, {l + len, bt}, w, c);
    b.drawLine({l, bt}, {l, bt - len}, w, c);
    b.drawLine({rt, bt}, {rt - len, bt}, w, c);
    b.drawLine({rt, bt}, {rt, bt - len}, w, c);
}

} // namespace vo::client::scenes
