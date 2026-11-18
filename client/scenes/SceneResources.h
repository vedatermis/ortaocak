// Optional art shared by the 2D scenes, loaded once after the renderer starts (and released before
// it shuts down). Every image is optional: if the file does not exist the scenes draw their
// procedural fallback. Looked up as assets/ui/<file> first, then assets_src/ui/<file>:
//   login_bg.png                         full-screen background (cover-fit, dark vignette on top)
//   race_aydor.png / race_kargan.png     race portraits (character preview, race cards)
#pragma once

#include "client/core/Geometry.h"
#include "client/render/Texture2D.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace vo::client::render {
class AssetCache;
class RenderDevice;
class WorldRenderer;
} // namespace vo::client::render
namespace vo::client::world {
struct ItemData;
}

namespace vo::client::scenes {

// What the 3D character preview renders with (owned by the frame loop; any may be null).
struct PreviewServices
{
    render::RenderDevice* device = nullptr;
    render::WorldRenderer* renderer = nullptr;
    render::AssetCache* assets = nullptr;
};

class SceneResources
{
public:
    void setPreviewServices(const PreviewServices& services) noexcept { m_preview = services; }
    // nullptr if no 3D renderer is available (the preview then shows the 2D art).
    [[nodiscard]] const PreviewServices* previewServices() const noexcept
    {
        return m_preview.renderer != nullptr ? &m_preview : nullptr;
    }

    // Item templates + weapon attachments (M8), owned by the frame loop; survives reset() (device loss).
    void setItemData(const world::ItemData* items) noexcept { m_items = items; }
    // Never null once the frame loop set it (the data inside may be empty if the files were missing).
    [[nodiscard]] const world::ItemData* itemData() const noexcept { return m_items; }
    // Inventory icon of a template (ui/items/<icon>.vtex, premultiplied), loaded on first use;
    // nullptr for unknown templates, without assets or if the file is missing.
    [[nodiscard]] const render::Texture2D* itemIcon(std::uint16_t templateId) const;
    // M15: `base` multiplied by the template's tint (items sharing an icon look apart); `base` for
    // unknown or untinted templates. Every item icon is drawn with it.
    [[nodiscard]] Color itemTint(std::uint16_t templateId, Color base = colors::kWhite) const;
    // Any UI image of the asset cache by source path (e.g. "ui/items/coin.png", M10), loaded on first
    // use; nullptr without assets or if neither the png nor its compiled .vtex is on disk (no error).
    [[nodiscard]] const render::Texture2D* uiImage(std::string_view path) const;

    SceneResources() = default;
    SceneResources(const SceneResources&) = delete;
    SceneResources& operator=(const SceneResources&) = delete;

    void load(render::RenderDevice& device);
    void reset() noexcept;

    // nullptr when the image is not available.
    [[nodiscard]] const render::Texture2D* background() const noexcept;
    // fix1: the loading screen of a zone, ui/loading/zone<N> (.png / .jpg under assets/ui or assets_src/ui,
    // else the asset cache's compiled ui/loading/zone<N>.vtex), loaded on first use; nullptr when there is
    // none (the loading screen then shows background()).
    [[nodiscard]] const render::Texture2D* zoneLoadingImage(std::uint16_t zoneId) const;
    // race: vo::game::Race value (1 Aydor, 2 Kargan); nullptr for unknown races or missing files.
    [[nodiscard]] const render::Texture2D* racePortrait(std::uint8_t race) const noexcept;

private:
    render::Texture2D m_background;
    render::Texture2D m_aydor;
    render::Texture2D m_kargan;
    render::RenderDevice* m_device = nullptr;
    static constexpr std::size_t kLoadingZones = 8;
    mutable std::array<render::Texture2D, kLoadingZones> m_loading{};
    mutable std::array<bool, kLoadingZones> m_loadingTried{};
    mutable std::array<const render::Texture2D*, kLoadingZones> m_loadingCompiled{};
    PreviewServices m_preview;
    const world::ItemData* m_items = nullptr;
};

} // namespace vo::client::scenes
