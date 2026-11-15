// Procedural full-screen backdrop shared by the 2D scenes (until generated art replaces it).
#pragma once

#include "client/render/SpriteBatch.h"
#include "client/ui/Theme.h"

namespace vo::client::scenes {

void drawBackdrop(render::SpriteBatch& batch, const ui::Theme& theme, float width, float height, float scale);

} // namespace vo::client::scenes
