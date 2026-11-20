#include "client/scenes/InventoryFigure.h"

#include "client/scenes/InventoryWindow.h"

namespace vo::client::scenes {

CharacterPreview& addInventoryFigure(InventoryWindow& window, const SceneResources& resources)
{
    auto& figure = window.add<CharacterPreview>(resources);
    const Rect a = window.figureArea();
    figure.setAnchors(ui::anchor::kTopLeft).setMargins({a.x, a.y, 0.0f, 0.0f}).setSize(a.w, a.h);
    figure.setCaptionVisible(false);
    figure.setName("inv.figure");
    return figure;
}

} // namespace vo::client::scenes
