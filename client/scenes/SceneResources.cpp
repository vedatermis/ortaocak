#include "client/scenes/SceneResources.h"

#include "client/render/AssetCache.h"
#include "client/render/RenderDevice.h"
#include "client/world/ItemData.h"
#include "client/world/WeaponPose.h"
#include "shared/assets/Texture.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/game/Character.h"

#include <filesystem>
#include <optional>
#include <string>

namespace vo::client::scenes {

namespace {

std::optional<std::filesystem::path> findArt(const char* file)
{
    if (auto p = vo::core::locateDataFile(std::filesystem::path("assets/ui") / file))
        return p;
    return vo::core::locateDataFile(std::filesystem::path("assets_src/ui") / file);
}

void loadArt(render::RenderDevice& device, const char* file, render::Texture2D& out)
{
    const auto path = findArt(file);
    if (!path)
    {
        VO_LOG_INFO("art: {} not found (assets/ui, assets_src/ui); using the procedural fallback", file);
        return;
    }
    render::Texture2D::Options opts;
    opts.debugName = file;
    std::string error;
    if (out.loadFromFile(device.device(), device.context(), *path, opts, error))
        VO_LOG_INFO("art: loaded {} ({}x{})", vo::core::pathToUtf8(*path), out.width(), out.height());
    else
        VO_LOG_WARN("art: {}: {}; using the procedural fallback", vo::core::pathToUtf8(*path), error);
}

} // namespace

void SceneResources::load(render::RenderDevice& device)
{
    loadArt(device, "login_bg.png", m_background);
    loadArt(device, "race_aydor.png", m_aydor);
    loadArt(device, "race_kargan.png", m_kargan);
    loadArt(device, "loading/zone1.png", m_zoneLoading[0]);
    loadArt(device, "loading/zone2.png", m_zoneLoading[1]);
    loadArt(device, "loading/zone3.png", m_zoneLoading[2]);
}

void SceneResources::reset() noexcept
{
    m_background.reset();
    m_aydor.reset();
    m_kargan.reset();
    for (auto& t : m_zoneLoading)
        t.reset();
}

const render::Texture2D* SceneResources::background() const noexcept
{
    return m_background.valid() ? &m_background : nullptr;
}

const render::Texture2D* SceneResources::zoneBackground(std::uint16_t zoneId) const noexcept
{
    if (zoneId >= 1 && zoneId <= 3 && m_zoneLoading[zoneId - 1].valid())
        return &m_zoneLoading[zoneId - 1];
    return background();
}

const render::Texture2D* SceneResources::racePortrait(std::uint8_t race) const noexcept
{
    if (race == static_cast<std::uint8_t>(game::Race::Aydor))
        return m_aydor.valid() ? &m_aydor : nullptr;
    if (race == static_cast<std::uint8_t>(game::Race::Kargan))
        return m_kargan.valid() ? &m_kargan : nullptr;
    return nullptr;
}

const render::Texture2D* SceneResources::itemIcon(std::uint16_t templateId) const
{
    if (m_items == nullptr || m_preview.assets == nullptr || !m_preview.assets->hasRoot())
        return nullptr;
    const game::ItemTemplate* item = m_items->catalog.find(templateId);
    if (item == nullptr || item->icon.empty())
        return nullptr;
    const std::string path = world::itemIconPath(*item);
    if (!m_preview.assets->texture(path) && !m_preview.assets->failed(path) &&
        !world::itemIconAvailable(m_preview.assets->root(), *item))
        return nullptr; // not delivered yet (M10 icons): an empty slot, no load error
    if (const render::Texture2D* t = m_preview.assets->texture(path))
        return t;
    if (m_preview.assets->failed(path))
        return nullptr;
    return m_preview.assets->loadTextureNow(render::AssetCache::Kind::UiImage, path);
}

const render::Texture2D* SceneResources::uiImage(std::string_view path) const
{
    if (m_preview.assets == nullptr || !m_preview.assets->hasRoot() || path.empty())
        return nullptr;
    const std::string p(path);
    if (const render::Texture2D* t = m_preview.assets->texture(p))
        return t;
    if (m_preview.assets->failed(p))
        return nullptr;
    std::error_code ec;
    const std::filesystem::path root = m_preview.assets->root();
    if (!std::filesystem::is_regular_file(root / core::pathFromUtf8(p), ec) &&
        !std::filesystem::is_regular_file(root / core::pathFromUtf8(assets::compiledTexturePath(p)), ec))
        return nullptr; // not delivered: the caller draws its placeholder, no load error
    return m_preview.assets->loadTextureNow(render::AssetCache::Kind::UiImage, p);
}

} // namespace vo::client::scenes
