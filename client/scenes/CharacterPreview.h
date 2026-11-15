// Character preview shown on the character select and create screens.
//
// For now a framed 2D placeholder: the race portrait image (SceneResources: race_<aydor|kargan>.png)
// cover-fitted if it exists, otherwise a procedural silhouette whose build follows the race, whose
// prop follows the class and whose hair/face variants follow the appearance indices, plus a caption
// (name, race, class, level). This class is the only thing the screens know about the preview: a
// later milestone replaces its drawing with a rendered 3D model without touching the scenes.
#pragma once

#include "client/ui/Widget.h"

#include <cstdint>
#include <optional>
#include <string>

namespace vo::client::render {
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
    std::uint8_t face = 0;
    std::uint8_t hair = 0;
    std::string name;      // empty: no caption name
    std::uint16_t level = 0; // 0: no level in the caption
};

class CharacterPreview : public ui::Widget
{
public:
    explicit CharacterPreview(const SceneResources& resources);

    // nullopt shows the "empty" state with `emptyText`.
    void setLook(std::optional<CharacterLook> look);
    void setEmptyText(std::string text);
    void setCaptionVisible(bool visible) noexcept { m_caption = visible; }
    [[nodiscard]] const std::optional<CharacterLook>& look() const noexcept { return m_look; }

    void drawSelf(ui::DrawContext& ctx) override;

private:
    const SceneResources& m_resources;
    std::optional<CharacterLook> m_look;
    std::string m_emptyText = "Karakter seçilmedi";
    bool m_caption = true;
};

// Draws the portrait art for `look` into `rect` (image cover-fit if `portrait` is valid, else the
// procedural silhouette). Shared with the race cards.
void drawCharacterArt(render::SpriteBatch& batch, const ui::Theme& theme, const render::Texture2D* portrait,
                      const CharacterLook& look, const Rect& rect, float scale);

// "Kargan · Savaşçı · Seviye 1" (parts that are not set are left out).
[[nodiscard]] std::string describeCharacter(std::uint8_t race, std::uint8_t charClass, std::uint16_t level);

} // namespace vo::client::scenes
