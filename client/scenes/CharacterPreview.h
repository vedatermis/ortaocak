// Character preview shown on the character select and create screens.
//
// M6: the real rigged model of the chosen race and look (client/world/CharacterAssets.h), playing
// that look's idle clip in front of a lit backdrop. It is rendered with the world renderer into an offscreen target the
// size of the frame (render-to-texture during the UI draw, then drawn as an image) and can be
// turned by dragging with the left mouse button. If the 3D renderer or the character assets are
// unavailable it falls back to the 2D art: the race portrait image (SceneResources:
// race_<aydor|kargan>.png) cover-fitted if it exists, otherwise a procedural silhouette. A caption
// (name, race, class, level) is drawn on top either way. The class has no visual yet.
#pragma once

#include "client/ui/Widget.h"
#include "client/world/Animator.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace vo::client::render {
class AssetCache;
class RenderTarget;
class SpriteBatch;
class Texture2D;
} // namespace vo::client::render
namespace vo::client::ui {
struct Theme;
}

namespace vo::client::scenes {

class SceneResources;

struct CharacterLook
{
    std::uint8_t race = 0;      // vo::game::Race
    std::uint8_t charClass = 0; // vo::game::CharClass, 0 = not chosen yet
    std::uint8_t look = 0;      // character look index (the wire `face` field; client/world/CharacterAssets.h)
    std::string name;      // empty: no caption name
    std::uint16_t level = 0; // 0: no level in the caption
};

class CharacterPreview : public ui::Widget
{
public:
    explicit CharacterPreview(const SceneResources& resources);
    ~CharacterPreview() override;

    // nullopt shows the "empty" state with `emptyText`.
    void setLook(std::optional<CharacterLook> look);
    void setEmptyText(std::string text);
    void setCaptionVisible(bool visible) noexcept { m_caption = visible; }
    [[nodiscard]] const std::optional<CharacterLook>& look() const noexcept { return m_look; }
    // True if the last draw showed the 3D model (automation / tests).
    [[nodiscard]] bool showing3d() const noexcept { return m_shown3d; }
    // Mesh asset path the last 3D draw used (empty if none; automation).
    [[nodiscard]] const std::string& shownMesh() const noexcept { return m_shownMesh; }

    void drawSelf(ui::DrawContext& ctx) override;
    void update(double dt) override;
    bool onMouseDown(const platform::InputEvent& e) override;
    bool onMouseMove(const platform::InputEvent& e) override;
    bool onMouseUp(const platform::InputEvent& e) override;

private:
    // Renders the model into m_target (sized to `art`); false if 3D is unavailable.
    bool render3d(const Rect& art);

    const SceneResources& m_resources;
    std::optional<CharacterLook> m_look;
    std::string m_emptyText = "Karakter seçilmedi";
    bool m_caption = true;
    // 3D preview state.
    std::unique_ptr<render::RenderTarget> m_target;
    world::Animator m_animator;
    const world::CharacterClips* m_animClips = nullptr; // what m_animator is bound to
    const void* m_animMesh = nullptr;
    std::string m_shownMesh;
    float m_yaw = -0.45f; // three-quarter view; dragging turns the model
    bool m_dragging = false;
    float m_dragX = 0.0f;
    double m_time = 0.0;
    bool m_shown3d = false;
    bool m_failed3d = false; // logged once
};

// Draws the portrait art for `look` into `rect` (image cover-fit if `portrait` is valid, else the
// procedural silhouette). Shared with the race cards.
void drawCharacterArt(render::SpriteBatch& batch, const ui::Theme& theme, const render::Texture2D* portrait,
                      const CharacterLook& look, const Rect& rect, float scale);

// Automation check shared by the select and create screens: `preview` last drew the 3D model with
// the mesh `assets` resolves for (race, look), and that is look n's own characters/<race>_<n>.vmesh
// whenever that file exists. Logs the mesh on success.
bool expectPreviewMesh(const CharacterPreview& preview, const render::AssetCache* assets, std::uint8_t race, int look,
                       std::string& error);

// "Kargan · Savaşçı · Seviye 1" (parts that are not set are left out).
[[nodiscard]] std::string describeCharacter(std::uint8_t race, std::uint8_t charClass, std::uint16_t level);

} // namespace vo::client::scenes
