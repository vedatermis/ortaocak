// Full-screen backdrop shared by the 2D scenes: the optional background image (cover-fit, with a
// dark vignette so UI on top stays readable) or, without one, the procedural gradient backdrop.
#pragma once

#include "client/render/SpriteBatch.h"
#include "client/ui/Theme.h"

namespace vo::client::render {
class Texture2D;
}

namespace vo::client::scenes {

// Procedural gradient + horizon glow + vignette + thin gold rule near the top.
void drawBackdrop(render::SpriteBatch& batch, const ui::Theme& theme, float width, float height, float scale);

// `image` may be null (then this is drawBackdrop()).
void drawSceneBackground(render::SpriteBatch& batch, const ui::Theme& theme, const render::Texture2D* image,
                         float width, float height, float scale);

} // namespace vo::client::scenes
