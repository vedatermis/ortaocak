// Optional art shared by the 2D scenes, loaded once after the renderer starts (and released before
// it shuts down). Every image is optional: if the file does not exist the scenes draw their
// procedural fallback. Looked up as assets/ui/<file> first, then assets_src/ui/<file>:
//   login_bg.png                         full-screen background (cover-fit, dark vignette on top)
//   race_aydor.png / race_kargan.png     race portraits (character preview, race cards)
#pragma once

#include "client/render/Texture2D.h"

#include <cstdint>

namespace vo::client::render {
class AssetCache;
class RenderDevice;
class WorldRenderer;
} // namespace vo::client::render

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

    SceneResources() = default;
    SceneResources(const SceneResources&) = delete;
    SceneResources& operator=(const SceneResources&) = delete;

    void load(render::RenderDevice& device);
    void reset() noexcept;

    // nullptr when the image is not available.
    [[nodiscard]] const render::Texture2D* background() const noexcept;
    // race: vo::game::Race value (1 Aydor, 2 Kargan); nullptr for unknown races or missing files.
    [[nodiscard]] const render::Texture2D* racePortrait(std::uint8_t race) const noexcept;

private:
    render::Texture2D m_background;
    render::Texture2D m_aydor;
    render::Texture2D m_kargan;
    PreviewServices m_preview;
};

} // namespace vo::client::scenes
