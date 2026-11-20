// M17 (armour, second pass): the own character, live in 3D with its worn armour and weapons, in the inventory
// window's figure area between the equipment slots. It is the character select screen's CharacterPreview
// (render-to-texture through the world renderer, idle clip, drag to turn) without the caption, added as a child
// of the InventoryWindow at InventoryWindow::figureArea(), so it covers the window's flat silhouette (which stays
// as the fallback when 3D is unavailable). The world scene sets the look every frame while the window is open
// (race, look, the drawn equipment: client/scenes/WorldScene.cpp render()). Widget name: inv.figure.
//
// The offscreen pass runs during the UI draw, after the world frame was rendered and resolved; it reuses the world
// renderer (like the select screen), so while the window is open the renderer's per-frame statistics describe
// the figure's pass, not the world's (WorldRenderer::stats(); the perf flow keeps the window closed).
#pragma once

#include "client/scenes/CharacterPreview.h"

namespace vo::client::scenes {

class InventoryWindow;

// Adds the figure to `window` and returns it.
CharacterPreview& addInventoryFigure(InventoryWindow& window, const SceneResources& resources);

} // namespace vo::client::scenes
