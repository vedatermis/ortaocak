#include "client/scenes/WorldScene.h"

#include "client/net/GameClient.h"
#include "client/platform/Input.h"
#include "client/render/AssetCache.h"
#include "client/render/RenderDevice.h"
#include "client/render/TextRenderer.h"
#include "client/render/WorldRenderer.h"
#include "client/scenes/Backdrop.h"
#include "client/scenes/CharSelectScene.h"
#include "client/scenes/CharacterPreview.h"
#include "client/scenes/SceneResources.h"
#include "client/ui/Ui.h"
#include "client/world/TerrainQuery.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/PathFind.h"
#include "shared/game/Zones.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <cmath>
#include <format>
#include <sstream>
#include <unordered_set>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;
using platform::EventType;
using platform::MouseButton;
using world::Vec3;

namespace {

constexpr float kFocusHeight = 1.55f;    // camera orbit point above the feet
constexpr float kLabelFullAlpha = 28.0f; // name labels fade between these distances
constexpr float kLabelHidden = 70.0f;
constexpr double kAssetBudget = 0.008; // seconds of GPU uploads per loading frame
// Zone banner on entering: fade in, hold, fade out (seconds).
constexpr float kBannerFadeIn = 0.8f;
constexpr float kBannerHold = 3.2f;
constexpr float kBannerFadeOut = 1.2f;
// The local character dithers out between these eye distances (metres from its body axis).
// Half transparent at ~0.75 m, gone below kFadeNear.
constexpr float kFadeNear = 0.45f;
constexpr float kFadeFar = 1.05f;

// One of these on the loading screen (docs/LORE.md).
constexpr std::array<std::string_view, 6> kLoreLines = {
    "Ortaocak'ta eski bir gelenek gereği kan dökülmez.",
    "Ant Taşı hâlâ meydanda duruyor, ortasından çatlamış halde.",
    "Köz kırıkları bulundukları yeri ısıtır, taşıyana güç verir.",
    "Aydorlu Közaran yeminini mühürler; Karganlı adını ocak taşına kazır.",
    "Közyürek parçalandığı gece Uzun Kış bitti. Savaş bitmedi.",
    "Kırıkların düştüğü yerlerde kül kıpırdıyor.",
};

// Full-screen modal layer of the Esc menu: dims the world, Esc closes it.
class MenuLayer final : public ui::Widget
{
public:
    explicit MenuLayer(std::function<void()> onEscape) : m_onEscape(std::move(onEscape)) { setName("world.menu"); }
    void drawSelf(ui::DrawContext& ctx) override { ctx.batch.fillRect(rect(), Color::hex(0x05070A, 0.55f)); }
    bool onKeyDown(const platform::InputEvent& e) override
    {
        if (e.key == platform::keys::kEscape && !e.repeat)
        {
            if (m_onEscape)
                m_onEscape();
            return true;
        }
        return false;
    }

private:
    std::function<void()> m_onEscape;
};

bool isWorldOpcode(std::uint16_t op)
{
    return op == static_cast<std::uint16_t>(vn::Opcode::EntitySpawn) ||
           op == static_cast<std::uint16_t>(vn::Opcode::EntityMove) ||
           op == static_cast<std::uint16_t>(vn::Opcode::EntityDespawn) ||
           op == static_cast<std::uint16_t>(vn::Opcode::PositionCorrect);
}

std::uint8_t validRace(std::uint8_t race)
{
    return game::isValidRace(race) ? race : static_cast<std::uint8_t>(game::Race::Aydor);
}

float degToRad(float d)
{
    return d * world::kPi / 180.0f;
}

// std::format into a fixed buffer (no heap allocation); the result views the buffer.
template <class... Args>
std::string_view formatInto(std::array<char, 160>& buffer, std::format_string<Args...> fmt, Args&&... args)
{
    const auto r = std::format_to_n(buffer.data(), static_cast<std::ptrdiff_t>(buffer.size()), fmt,
                                    std::forward<Args>(args)...);
    return {buffer.data(), std::min(static_cast<std::size_t>(r.size), buffer.size())};
}

} // namespace

WorldScene::WorldScene() = default;
WorldScene::~WorldScene() = default;

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void WorldScene::build()
{
    const auto& entry = ctx().net.account().world;
    const vn::CharSummary character = entry ? entry->character : vn::CharSummary{};
    const vn::EnterWorldRes enter = entry ? entry->enter : vn::EnterWorldRes{};
    m_name = character.name;
    m_race = validRace(character.race);
    m_look = character.face;
    {
        const std::uint16_t zoneId = enter.zoneId != 0 ? enter.zoneId : game::kStartZoneId;
        m_zoneName = game::zoneDisplayName(zoneId);
        const game::ZoneInfo* zone = game::findZone(zoneId);
        m_zoneTagline = zone != nullptr ? std::string(zone->tagline) : std::string();
    }
    m_loreLine = (static_cast<std::size_t>(enter.entityId) + static_cast<std::size_t>(character.charId)) % kLoreLines.size();
    m_time = 0.0;
    m_model.enter(enter, nullptr, m_time, m_runDefault);
    m_camera.setPose(enter.yaw, 0.36f, 8.5f);
    m_camera.reset(m_model.local().mover().position() + Vec3{0.0f, kFocusHeight, 0.0f}, enter.yaw);
    m_ghostSink = [this](std::uint16_t op, std::vector<std::uint8_t> payload)
    { injectGhostPacket(op, std::move(payload)); };
    VO_LOG_INFO("world: entered as '{}' (entity {}, zone {}) at ({:.1f}, {:.1f}, {:.1f}), speeds {:.1f}/{:.1f} m/s",
                m_name, enter.entityId, enter.zoneId, enter.x, enter.y, enter.z, m_model.params().walkSpeed,
                m_model.params().runSpeed);

    buildHud();

    const std::uint16_t zone = enter.zoneId != 0 ? enter.zoneId : game::kStartZoneId;
    m_terrainPath = std::format("terrain/zone{0}/zone{0}.vterrain", zone);
    render::AssetCache* assets = ctx().assets;
    if (assets == nullptr || ctx().worldRenderer == nullptr || !assets->hasRoot())
    {
        failLoading("assets");
        return;
    }
    assets->request(render::AssetCache::Kind::Terrain, m_terrainPath);
    assets->requestCharacter(static_cast<std::uint8_t>(game::Race::Aydor));
    assets->requestCharacter(static_cast<std::uint8_t>(game::Race::Kargan));
    m_phase = Phase::LoadingTerrain;
}

void WorldScene::buildHud()
{
    const ui::Theme& t = theme();
    ui::Widget& root = ui().root();
    const auto& entry = ctx().net.account().world;
    const vn::CharSummary character = entry ? entry->character : vn::CharSummary{};

    // Top-left: who we are.
    m_hudPanel = &root.add<ui::Panel>(ui::Panel::Style::Plain);
    m_hudPanel->setAnchors(anchor::kTopLeft).setMargins({14.0f, 14.0f, 0.0f, 0.0f}).setSize(300.0f, 62.0f);
    m_hudPanel->setPadding({12.0f, 6.0f, 12.0f, 6.0f});
    auto& who = m_hudPanel->add<ui::StackPanel>(ui::Orientation::Vertical, 0.0f);
    who.setAnchors(anchor::kFill);
    auto& name = who.add<ui::Label>(character.name);
    name.setFontSize(22.0f).setWeight(render::FontWeight::Bold).setColor(t.textTitle).setShadow(true);
    name.setSize(0.0f, 28.0f);
    name.setName("world.name");
    auto& level = who.add<ui::Label>(describeCharacter(character.race, character.charClass, character.level));
    level.setColor(t.text).setShadow(true);
    level.setSize(0.0f, 20.0f);
    level.setName("world.level");

    // Top-right: where we are.
    m_zoneLabel = &root.add<ui::Label>(m_zoneName);
    m_zoneLabel->setFontSize(20.0f).setWeight(render::FontWeight::Bold).setColor(t.textTitle).setShadow(true);
    m_zoneLabel->setAlign(render::HAlign::Right, render::VAlign::Middle);
    m_zoneLabel->setAnchors(anchor::kTopRight).setMargins({0.0f, 16.0f, 18.0f, 0.0f}).setSize(260.0f, 28.0f);
    m_zoneLabel->setName("world.zone");

    // Below it: technical info (F3).
    m_infoPanel = &root.add<ui::Panel>(ui::Panel::Style::Plain);
    m_infoPanel->setAnchors(anchor::kTopLeft).setMargins({14.0f, 84.0f, 0.0f, 0.0f}).setSize(420.0f, 100.0f);
    m_infoPanel->setPadding({12.0f, 6.0f, 12.0f, 6.0f});
    auto& info = m_infoPanel->add<ui::StackPanel>(ui::Orientation::Vertical, 0.0f);
    info.setAnchors(anchor::kFill);
    auto addInfo = [&](const char* widgetName)
    {
        auto& l = info.add<ui::Label>(std::string(), ui::TextRole::Small);
        l.setColor(t.text).setShadow(true);
        l.setSize(0.0f, 0.0f);
        l.setName(widgetName);
        return &l;
    };
    m_position = addInfo("world.position");
    m_rtt = addInfo("world.rtt");
    m_fps = addInfo("world.fps");
    m_stats = addInfo("world.stats");
    auto& entity = info.add<ui::Label>(std::format("Varlık No: {}", entry ? entry->enter.entityId : 0u),
                                       ui::TextRole::Small);
    entity.setColor(t.textDim).setShadow(true);
    entity.setSize(0.0f, 0.0f);
    entity.setName("world.entity");

    // Bottom: controls.
    m_help = &root.add<ui::Label>("WASD: hareket (kameraya göre)  ·  Sol tık: oraya git  ·  Sağ tık + sürükle: kamera  ·  "
                                  "Tekerlek: yakınlaştır  ·  Shift: yürü  ·  R: koşu/yürüyüş  ·  F3: bilgi  ·  Esc: menü",
                                  ui::TextRole::Small);
    m_help->setColor(t.text).setShadow(true).setAlign(render::HAlign::Center);
    m_help->setAnchors(anchor::kBottom | anchor::kFillX).setMargins({10.0f, 0.0f, 10.0f, 10.0f}).setSize(0.0f, 20.0f);
    m_help->setName("world.help");

    m_hudPanel->setVisible(false);
    m_zoneLabel->setVisible(false);
    m_infoPanel->setVisible(false);
    m_help->setVisible(false);
    updateHud(1.0f);
}

void WorldScene::teardown()
{
    stopDrag();
    m_ghosts = world::DebugGhosts{};
    if (ctx().worldRenderer != nullptr)
        ctx().worldRenderer->clearTerrain();
    m_menuLayer = nullptr;
}

// ---------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------

void WorldScene::pollLoading()
{
    render::AssetCache& cache = *ctx().assets;
    cache.pump(kAssetBudget);
    if (m_phase == Phase::LoadingTerrain)
    {
        if (const assets::TerrainData* terrain = cache.terrain(m_terrainPath))
        {
            m_terrain = terrain;
            for (const assets::TerrainLayer& l : terrain->layers)
                cache.request(render::AssetCache::Kind::Texture, l.texture);
            std::unordered_set<std::string_view> meshes;
            for (const assets::ObjectPlacement& o : terrain->objects)
            {
                if (meshes.insert(o.mesh).second)
                    cache.request(render::AssetCache::Kind::Mesh, o.mesh);
            }
            VO_LOG_INFO("world: terrain {} loaded ({} m, {} objects, {} meshes) after {:.2f} s", m_terrainPath,
                        terrain->worldSize, terrain->objects.size(), meshes.size(), m_loadingTime);
            m_phase = Phase::LoadingAssets;
        }
        else if (cache.failed(m_terrainPath))
        {
            failLoading(m_terrainPath);
            return;
        }
    }
    if (m_phase == Phase::LoadingAssets && cache.idle())
        becomeReady();
}

void WorldScene::becomeReady()
{
    std::string error;
    if (!ctx().worldRenderer->setTerrain(*m_terrain, *ctx().assets, error))
    {
        VO_LOG_ERROR("world: {}", error);
        failLoading("terrain");
        return;
    }
    m_model.setTerrain(m_terrain);
    world::LocalMover& mover = m_model.local().mover();
    mover.setPosition(mover.position(), mover.yaw(), m_terrain); // snap to the ground (server y ignored)
    m_camera.reset(mover.position() + Vec3{0.0f, kFocusHeight, 0.0f}, mover.yaw());

    render::AssetCache& cache = *ctx().assets;
    m_propOccluders.clear();
    m_propOccluders.reserve(m_terrain->objects.size());
    for (const assets::ObjectPlacement& o : m_terrain->objects)
    {
        if (const render::GpuMesh* mesh = cache.mesh(o.mesh); mesh != nullptr && mesh->cpu)
            m_propOccluders.push_back(world::makeOccluder(o.worldMatrix(), mesh->cpu->boundsMin, mesh->cpu->boundsMax));
    }
    buildCameraObstacles();
    if (const render::GpuMesh* mesh = cache.mesh(cache.characterMeshPath(m_race, m_look)); mesh != nullptr && mesh->cpu)
        m_localAnim.bind(mesh->cpu.get(), cache.characterClips(m_race, m_look));
    if (!m_localAnim.bound())
        VO_LOG_WARN("world: character assets of race {} look {} missing; the local character is not drawn", m_race,
                    m_look);
    else
        VO_LOG_INFO("world: local character race {} look {} -> {} (effective look {})", m_race, m_look,
                    cache.characterMeshPath(m_race, m_look), cache.looks().effectiveLook(m_race, m_look));

    if (ctx().debugGhosts > 0)
    {
        m_ghosts.start(ctx().debugGhosts, mover.position(), m_terrain, m_model.clock(), m_time, m_ghostSink);
        VO_LOG_INFO("world: --debug-ghosts: {} synthetic remote players", m_ghosts.count());
    }
    m_phase = Phase::Ready;
    m_bannerTime = 0.0f;
    m_hudPanel->setVisible(true);
    m_zoneLabel->setVisible(true);
    m_infoPanel->setVisible(m_showInfo);
    m_help->setVisible(true);
    const auto p = cache.progress();
    VO_LOG_INFO("world: ready after {:.2f} s ({} assets, {} failed); player at ({:.2f}, {:.2f}, {:.2f})",
                m_loadingTime, p.requested, p.failed, mover.position().x, mover.position().y, mover.position().z);
}

void WorldScene::buildCameraObstacles()
{
    m_cameraObstacles.clear();
    m_cameraObstacles.setTerrain(m_terrain);
    render::AssetCache& cache = *ctx().assets;
    std::size_t skipped = 0;
    for (const assets::ObjectPlacement& o : m_terrain->objects)
    {
        // Trees: their bounds are mostly open canopy; the camera passes through foliage.
        if (o.mesh.find("tree_") != std::string::npos)
        {
            ++skipped;
            continue;
        }
        const render::GpuMesh* mesh = cache.mesh(o.mesh);
        if (mesh == nullptr || !mesh->cpu || !m_cameraObstacles.add(o.worldMatrix(), mesh->cpu->boundsMin, mesh->cpu->boundsMax))
            ++skipped;
    }
    m_cameraObstacles.finish();
    m_camera.setObstacles(m_cameraCollision ? &m_cameraObstacles : nullptr);
    VO_LOG_INFO("world: camera obstacles: {} boxes ({} placements skipped: trees, thin or small props)",
                m_cameraObstacles.size(), skipped);
}

float WorldScene::localOpacity() const
{
    // Distance from the eye to the character's body axis (feet + 0.2 m .. feet + 1.8 m).
    const Vec3 feet = m_model.local().visualPosition();
    const Vec3 a = feet + Vec3{0.0f, 0.2f, 0.0f};
    const Vec3 b = feet + Vec3{0.0f, 1.8f, 0.0f};
    const Vec3 e = m_camera.eye();
    const float t = std::clamp((e.y - a.y) / (b.y - a.y), 0.0f, 1.0f);
    const float d = assets::length(e - assets::lerp(a, b, t));
    return std::clamp((d - kFadeNear) / (kFadeFar - kFadeNear), 0.0f, 1.0f);
}

void WorldScene::updateFootProbe(float dt)
{
    FootProbe& f = m_footProbe;
    if (!m_localAnim.bound() || m_terrain == nullptr)
        return;
    m_localAnim.evaluate();
    const auto model = m_localAnim.modelMatrices();
    const Vec3 body = m_model.local().visualPosition();
    const world::Mat4 toWorld = world::makeWorld(body + Vec3{0.0f, m_localAnim.heightOffset(), 0.0f},
                                                 m_model.local().mover().yaw(), 1.0f);
    f.time += dt;
    f.travel += std::hypot(body.x - f.lastBody.x, body.z - f.lastBody.z);
    f.lastBody = body;
    for (std::size_t i = 0; i < 2; ++i)
    {
        const int b = f.bone[i];
        if (b < 0 || static_cast<std::size_t>(b) >= model.size())
            continue;
        const assets::Mat4& m = model[static_cast<std::size_t>(b)];
        const Vec3 p = assets::transformPoint(toWorld, {m.at(0, 3), m.at(1, 3), m.at(2, 3)});
        // In contact: the ball of the foot within 8 cm of the ground.
        const bool contact = p.y < m_terrain->heightAt(p.x, p.z) + 0.08f;
        if (contact && f.contact[i])
        {
            f.slide += std::hypot(p.x - f.last[i].x, p.z - f.last[i].z);
            f.contactTime += dt;
        }
        f.contact[i] = contact;
        f.last[i] = p;
    }
}

void WorldScene::startJump()
{
    if (!m_localAnim.bound() || m_localAnim.seated())
        return;
    if (m_localAnim.jump())
        VO_LOG_INFO("world: jump (cosmetic, local only)");
}

void WorldScene::toggleSit()
{
    if (!m_localAnim.bound())
        return;
    const bool wasSeated = m_localAnim.seated();
    if (!wasSeated && (m_model.local().mover().moving() || m_model.local().mover().destination() || routePending()))
        return; // sit down only when standing still
    if (m_localAnim.toggleSit())
        VO_LOG_INFO("world: {} (cosmetic, local only)", wasSeated ? "standing up" : "sitting down");
}

void WorldScene::failLoading(const std::string& why)
{
    m_phase = Phase::Failed;
    VO_LOG_ERROR("world: cannot show the world: {} missing or invalid (build the assets: "
                 "build\\x64-debug\\bin\\assetc.exe build assets_src assets)",
                 why);
    showMessage({"Dünya", "Dünya dosyaları yüklenemedi. Oyun verilerinin (assets) kurulu olduğundan emin olun."},
                [this]() { leaveWorld(); });
}

// ---------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------

void WorldScene::onFrame(double dt)
{
    const auto fdt = static_cast<float>(dt);
    m_time += dt;
    if (m_phase == Phase::LoadingTerrain || m_phase == Phase::LoadingAssets)
    {
        m_loadingTime += fdt;
        pollLoading();
        return;
    }
    if (m_phase != Phase::Ready)
        return;
    updateWorld(fdt);
    updateCamera(fdt);
    updateHud(fdt);
}

world::MoveIntent WorldScene::readIntent() const
{
    const platform::Input& input = ctx().input;
    world::MoveIntent in;
    in.cameraYaw = m_camera.yaw();
    in.running = m_runDefault != input.isKeyDown(platform::keys::kShift);
    if (m_menuOpen || m_leavingWorld || leaving() || ui().hasModal())
        return in;
    namespace k = platform::keys;
    if (input.isKeyDown(k::letter('W')) || input.isKeyDown(k::kUp))
        in.forward += 1.0f;
    if (input.isKeyDown(k::letter('S')) || input.isKeyDown(k::kDown))
        in.forward -= 1.0f;
    if (input.isKeyDown(k::letter('D')) || input.isKeyDown(k::kRight))
        in.right += 1.0f;
    if (input.isKeyDown(k::letter('A')) || input.isKeyDown(k::kLeft))
        in.right -= 1.0f;
    return in;
}

void WorldScene::updateWorld(float dt)
{
    world::LocalMover& walker = m_model.local().mover();
    // Seated (sitting down, sitting, standing up): no movement; any movement input stands up first.
    const bool seated = m_localAnim.seated();
    if (!seated && m_pendingDestination && !walker.destination() && !m_model.leaving())
    {
        walker.setDestination(m_pendingDestination->x, m_pendingDestination->y);
        m_pendingDestination.reset();
    }
    // Scripted path walk: the next waypoint once the previous one is reached.
    if (!seated && routePending() && !walker.destination() && !m_model.leaving())
    {
        const world::Vec2 next = m_route[m_routeNext++];
        walker.setDestination(next.x, next.y);
    }
    world::MoveIntent intent = readIntent();
    if (seated)
    {
        if (walker.destination())
        {
            m_pendingDestination = *walker.destination();
            walker.clearDestination();
        }
        if (intent.forward != 0.0f || intent.right != 0.0f || m_pendingDestination || routePending())
            m_localAnim.standUp();
        intent.forward = 0.0f;
        intent.right = 0.0f;
    }
    if (const auto req = m_model.updateLocal(m_time, intent, dt))
        (void)ctx().net.send(*req);
    if (m_ghosts.count() > 0)
        m_ghosts.update(m_time, dt, m_model.clock(), m_model.local().mover().position(), m_model.params(),
                        m_ghostSink);
    m_model.updateRemotes(m_time, dt);

    const world::LocalMover& mover = m_model.local().mover();
    if (m_localAnim.bound())
        m_localAnim.update(dt, mover.speed(), mover.running());
    if (m_footProbe.active)
        updateFootProbe(dt);
    render::AssetCache& cache = *ctx().assets;
    for (world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active)
            continue;
        // Mesh and clips of the remote's race + look (a look without assets falls back to look 0).
        const std::uint8_t race = validRace(e.race);
        const render::GpuMesh* mesh = cache.mesh(cache.characterMeshPath(race, e.face));
        const world::CharacterClips* clips = cache.characterClips(race, e.face);
        if (mesh != nullptr && mesh->cpu && clips != nullptr && (e.animatorMesh != mesh || e.animatorClips != clips))
        {
            e.animator.bind(mesh->cpu.get(), clips);
            e.animatorMesh = mesh;
            e.animatorClips = clips;
        }
        if (e.animator.bound())
            e.animator.update(dt, e.speed, e.running);
    }
    m_markerPhase += dt;
    if (m_bannerTime >= 0.0f)
    {
        m_bannerTime += dt;
        if (m_bannerTime > kBannerFadeIn + kBannerHold + kBannerFadeOut)
            m_bannerTime = -1.0f;
    }
}

void WorldScene::updateCamera(float dt)
{
    const platform::Input& input = ctx().input;
    if (m_dragging)
    {
        if (!input.isMouseDown(MouseButton::Right))
        {
            stopDrag();
        }
        else
        {
            const int dx = input.mouseX() - m_lastMouseX;
            const int dy = input.mouseY() - m_lastMouseY;
            m_camera.rotate(static_cast<float>(dx), static_cast<float>(dy));
            if (!ctx().scripted && ctx().warpMouse && (dx != 0 || dy != 0))
            {
                ctx().warpMouse(m_dragAnchorX, m_dragAnchorY); // keep the hidden cursor in place
                m_lastMouseX = m_dragAnchorX;
                m_lastMouseY = m_dragAnchorY;
            }
            else
            {
                m_lastMouseX = input.mouseX();
                m_lastMouseY = input.mouseY();
            }
        }
    }
    if (m_pendingZoom != 0.0f)
    {
        m_camera.zoom(m_pendingZoom);
        m_pendingZoom = 0.0f;
    }
    m_camera.update(m_model.local().visualPosition() + Vec3{0.0f, kFocusHeight, 0.0f}, m_terrain, dt);
}

void WorldScene::updateHud(float dt)
{
    m_hudTimer -= dt;
    const std::size_t players = m_model.activeCount();
    // The 4 Hz refresh, or at once when somebody spawned / despawned (the count is never stale).
    if ((m_hudTimer > 0.0f && players == m_shownPlayers) || m_position == nullptr)
        return;
    m_hudTimer = 0.25f;
    m_shownPlayers = players;
    // Formatted into a fixed buffer; Label::setText reuses its string: no steady-state allocation.
    auto& b = m_textBuffer;
    const Vec3 p = m_model.local().mover().position();
    m_position->setText(formatInto(b, "Konum: X {:.1f} · Y {:.1f} · Z {:.1f} · {}", p.x, p.y, p.z, m_zoneName));
    const auto rtt = ctx().net.rttMs();
    if (rtt)
        m_rtt->setText(formatInto(b, "Gecikme (RTT): {} ms  ·  Oyuncular: {}", std::llround(*rtt), players));
    else
        m_rtt->setText(formatInto(b, "Gecikme (RTT): ölçülüyor…  ·  Oyuncular: {}", players));
    const char* gait = m_model.local().mover().running() ? "koşu" : "yürüyüş";
    m_fps->setText(formatInto(b, "FPS: {:.0f}  ·  {}", ctx().fps, gait));
    if (ctx().worldRenderer != nullptr)
    {
        const render::WorldRenderStats& s = ctx().worldRenderer->stats();
        const std::uint64_t kTris = (s.triangles + s.shadowTriangles + 500) / 1000;
        m_stats->setText(formatInto(b, "Çizim: {} + {} gölge + {} arayüz çağrısı · {} bin üçgen", s.drawCalls,
                                    s.shadowDrawCalls, ctx().lastBatchStats.drawCalls, kTris));
    }
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void WorldScene::onUnhandledEvent(const platform::InputEvent& e)
{
    if (e.type == EventType::FocusLost)
        stopDrag();
    if (m_phase != Phase::Ready || m_menuOpen || m_leavingWorld || m_model.leaving())
        return;
    switch (e.type)
    {
    case EventType::MouseDown:
        if (e.button == MouseButton::Right && !m_dragging)
        {
            m_dragging = true;
            m_dragAnchorX = m_lastMouseX = e.x;
            m_dragAnchorY = m_lastMouseY = e.y;
            if (!ctx().scripted && ctx().setMouseCaptured)
                ctx().setMouseCaptured(true);
        }
        else if (e.button == MouseButton::Left)
        {
            clickToMove(e.x, e.y);
        }
        break;
    case EventType::MouseWheel:
        m_pendingZoom += e.wheel;
        break;
    case EventType::FocusLost:
        stopDrag();
        break;
    default:
        break;
    }
}

void WorldScene::onUnhandledKey(const platform::InputEvent& e)
{
    if (e.repeat || m_leavingWorld || m_model.leaving())
        return; // waiting for LEAVE_WORLD_RES / quitting: world input is ignored
    if (e.key == platform::keys::kEscape)
    {
        if (m_phase == Phase::Ready && !m_menuOpen)
            openMenu();
        return;
    }
    if (m_phase != Phase::Ready)
        return;
    if (e.key == platform::keys::kF1 + 2) // F3
    {
        m_showInfo = !m_showInfo;
        m_infoPanel->setVisible(m_showInfo);
    }
    else if (e.key == platform::keys::letter('R'))
    {
        m_runDefault = !m_runDefault;
        VO_LOG_INFO("world: default movement now {}", m_runDefault ? "run" : "walk");
    }
    else if (e.key == platform::keys::kSpace && !m_menuOpen)
    {
        startJump();
    }
    else if (e.key == platform::keys::letter('X') && !m_menuOpen)
    {
        toggleSit();
    }
    else if (e.key == platform::keys::kF1 + 8 && m_ghosts.count() > 0) // F9: debug correction
    {
        std::string ignored;
        (void)automationCommand("correct", ignored);
    }
}

void WorldScene::stopDrag()
{
    if (!m_dragging)
        return;
    m_dragging = false;
    if (!ctx().scripted)
    {
        if (ctx().warpMouse)
            ctx().warpMouse(m_dragAnchorX, m_dragAnchorY);
        if (ctx().setMouseCaptured)
            ctx().setMouseCaptured(false);
    }
}

world::Mat4 WorldScene::currentViewProj() const
{
    const float aspect = ctx().viewportWidth / std::max(ctx().viewportHeight, 1.0f);
    return m_camera.projection(aspect) * m_camera.view();
}

void WorldScene::clickToMove(int x, int y)
{
    if (m_terrain == nullptr)
        return;
    world::Mat4 inv;
    if (!assets::invert(currentViewProj(), inv))
        return;
    const world::Ray ray = world::screenRay(inv, static_cast<float>(x), static_cast<float>(y), ctx().viewportWidth,
                                            ctx().viewportHeight);
    Vec3 hit{};
    if (!world::raycastTerrain(*m_terrain, ray, 600.0f, hit))
        return;
    m_route.clear();
    m_routeNext = 0;
    m_pendingDestination.reset();
    m_model.local().mover().setDestination(hit.x, hit.z); // seated: kept until the character stood up
    m_markerPhase = 0.0f;
    VO_LOG_DEBUG("world: click-to-move to ({:.1f}, {:.1f}){}", hit.x, hit.z,
                 m_terrain->isWalkable(hit.x, hit.z) ? "" : " (blocked cell)");
}

// ---------------------------------------------------------------------------------------------
// Menu / leaving
// ---------------------------------------------------------------------------------------------

void WorldScene::openMenu()
{
    if (m_menuOpen)
        return;
    stopDrag();
    auto layer = std::make_unique<MenuLayer>([this]() { closeMenu(); });
    auto& panel = layer->add<ui::Panel>(ui::Panel::Style::Window, "Menü");
    panel.setAnchors(anchor::kCenter).setSize(320.0f, 214.0f);
    panel.setPadding({22.0f, 18.0f, 22.0f, 18.0f});
    auto& stack = panel.add<ui::StackPanel>(ui::Orientation::Vertical, 10.0f);
    stack.setAnchors(anchor::kFill);
    auto& resume = stack.add<ui::Button>("Devam Et", [this]() { closeMenu(); });
    resume.setSize(0.0f, 40.0f);
    resume.setName("world.menu.resume");
    auto& back = stack.add<ui::Button>("Karakter Seçimine Dön", [this]() { leaveWorld(); });
    back.setSize(0.0f, 40.0f);
    back.setName("world.menu.charselect");
    auto& quit = stack.add<ui::Button>("Oyundan Çık", [this]() {
        m_model.beginLeaving(); // LOGOUT follows at shutdown: no MOVE_REQ any more
        ctx().requestQuit();
    });
    quit.setSize(0.0f, 40.0f);
    quit.setName("world.menu.quit");
    m_menuLayer = &ui().pushModal(std::move(layer));
    ui().setFocus(&resume);
    m_menuOpen = true;
}

void WorldScene::closeMenu()
{
    if (!m_menuOpen || m_menuLayer == nullptr)
        return;
    ui().closeModal(*m_menuLayer);
    m_menuLayer = nullptr;
    m_menuOpen = false;
}

void WorldScene::leaveWorld()
{
    if (m_leavingWorld || leaving())
        return;
    VO_LOG_INFO("world: leaving the world");
    closeMenu();
    stopDrag();
    // PROTOCOL §5.4 "Leaving": no MOVE_REQ after LEAVE_WORLD_REQ, world input ignored until the answer.
    m_route.clear();
    m_routeNext = 0;
    m_pendingDestination.reset();
    m_model.beginLeaving();
    m_leavingWorld = true;
    if (!ctx().net.request(vn::LeaveWorldReq{}, vn::Opcode::LeaveWorldRes))
    {
        m_leavingWorld = false;
        m_model.dropRemotes();
        returnToLogin(net::disconnectMessage({}));
        return;
    }
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

void WorldScene::injectGhostPacket(std::uint16_t opcode, std::vector<std::uint8_t> payload)
{
    ctx().net.injectMessage(opcode, std::move(payload));
}

void WorldScene::onNetEvent(const net::NetEvent& e)
{
    if (e.type != net::NetEventType::Message)
    {
        if (e.type == net::NetEventType::Disconnected)
        {
            m_model.dropRemotes(); // nobody is visible any more
            m_leavingWorld = false;
        }
        UiScene::onNetEvent(e);
        return;
    }
    if (isWorldOpcode(e.opcode))
    {
        if (!m_model.handleMessage(e.opcode, e.payload, m_time))
        {
            VO_LOG_ERROR("world: malformed {} payload ({} bytes)",
                         vn::opcodeName(static_cast<vn::Opcode>(e.opcode)), e.payload.size());
            returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        }
        return;
    }
    if (!e.is(vn::Opcode::LeaveWorldRes) || !m_leavingWorld)
        return;
    vn::LeaveWorldRes res;
    if (!vn::decodeMessage(e.payload, res))
    {
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return;
    }
    m_leavingWorld = false;
    if (res.result != vn::kLeaveWorldOk)
    {
        showMessage({"Dünya", net::leaveWorldResultMessage(res.result)});
        return;
    }
    m_model.dropRemotes(); // the server sends no ENTITY_DESPAWN to a player who leaves
    auto& account = ctx().net.account();
    CharSelectScene::Options opts;
    if (account.world)
        opts.selectCharId = account.world->character.charId;
    account.world.reset();
    changeScene(std::make_unique<CharSelectScene>(opts));
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

void WorldScene::addCharacterDraw(const render::GpuMesh* mesh, world::Animator& animator, Vec3 pos, float yaw,
                                  float alpha, float opacity)
{
    if (mesh == nullptr || !animator.bound())
        return;
    render::WorldRenderer& r = *ctx().worldRenderer;
    animator.evaluate();
    render::CharacterDraw d;
    d.mesh = mesh;
    d.skin = animator.skinMatrices();
    // The cosmetic jump lifts the drawn body only; the blob shadow stays on the ground.
    d.world = world::makeWorld(pos + Vec3{0.0f, animator.heightOffset(), 0.0f}, yaw, 1.0f);
    const float fade = 0.35f + 0.65f * std::clamp(alpha, 0.0f, 1.0f); // fresh spawns brighten in
    d.tint = {fade, fade, fade, 1.0f};
    d.opacity = opacity;
    r.addCharacter(d);
    render::DecalDraw blob;
    blob.center = pos;
    blob.normal = m_terrain != nullptr ? m_terrain->normalAt(pos.x, pos.z) : Vec3{0.0f, 1.0f, 0.0f};
    blob.radius = 0.62f * (1.0f - 0.5f * std::clamp(animator.heightOffset(), 0.0f, 1.0f));
    blob.color = {0.0f, 0.0f, 0.0f, 0.42f * std::clamp(opacity * 2.0f, 0.0f, 1.0f)};
    r.addDecal(blob);
}

void WorldScene::render()
{
    auto& batch = ctx().batch;
    if (m_phase != Phase::Ready)
    {
        batch.begin(static_cast<std::uint32_t>(ctx().viewportWidth), static_cast<std::uint32_t>(ctx().viewportHeight));
        drawLoading();
        ui().draw(batch);
        batch.end();
        return;
    }

    render::WorldRenderer& r = *ctx().worldRenderer;
    render::AssetCache& cache = *ctx().assets;
    const float aspect = ctx().viewportWidth / std::max(ctx().viewportHeight, 1.0f);
    render::ViewSetup view;
    view.view = m_camera.view();
    view.projection = m_camera.projection(aspect);
    view.eye = m_camera.eye();
    view.shadowFocus = m_model.local().visualPosition();
    view.time = static_cast<float>(m_time);
    view.shadows = ctx().shadows;
    r.beginFrame(view);

    const world::LocalMover& mover = m_model.local().mover();
    addCharacterDraw(cache.mesh(cache.characterMeshPath(m_race, m_look)), m_localAnim, m_model.local().visualPosition(),
                     mover.yaw(), 1.0f, localOpacity());
    for (world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active)
            continue;
        addCharacterDraw(cache.mesh(cache.characterMeshPath(validRace(e.race), e.face)), e.animator, e.position, e.yaw,
                         e.age / 0.4f);
    }
    if (const auto& dest = mover.destination())
    {
        render::DecalDraw ring;
        ring.center = {dest->x, m_terrain->heightAt(dest->x, dest->y), dest->y};
        ring.normal = m_terrain->normalAt(dest->x, dest->y);
        ring.radius = 0.75f;
        ring.color = {0.95f, 0.72f, 0.25f, 0.9f};
        ring.ring = true;
        ring.phase = std::fmod(m_markerPhase * 1.4f, 1.0f);
        r.addDecal(ring);
    }
    // The 3D pass goes into the multisampled scene target when graphics.msaa > 1 and is resolved
    // into the back buffer before labels and UI are drawn on top (single-sampled).
    render::RenderDevice& device = ctx().device;
    render::RenderTargetViews target;
    target.color = device.sceneColorView();
    target.depth = device.sceneDepthView();
    target.width = device.width();
    target.height = device.height();
    constexpr float kSceneClear[4] = {0.03f, 0.04f, 0.05f, 1.0f};
    device.beginScene(kSceneClear);
    r.render(target);
    device.resolveScene();

    batch.begin(static_cast<std::uint32_t>(ctx().viewportWidth), static_cast<std::uint32_t>(ctx().viewportHeight));
    drawLabels();
    drawZoneBanner();
    ui().draw(batch);
    batch.end();
}

void WorldScene::drawZoneBanner()
{
    if (m_bannerTime < 0.0f)
        return;
    const float t = m_bannerTime;
    float alpha = 1.0f;
    if (t < kBannerFadeIn)
        alpha = t / kBannerFadeIn;
    else if (t > kBannerFadeIn + kBannerHold)
        alpha = 1.0f - (t - kBannerFadeIn - kBannerHold) / kBannerFadeOut;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    alpha = alpha * alpha * (3.0f - 2.0f * alpha);
    if (alpha <= 0.0f)
        return;
    auto& batch = ctx().batch;
    const ui::Theme& th = theme();
    const float scale = ui().scale();
    const float w = ctx().viewportWidth;
    const float h = ctx().viewportHeight;
    const render::Font& nameFont = ctx().fonts.get(render::FontWeight::Bold, 40.0f * scale);
    const render::Font& tagFont = ctx().fonts.get(render::FontWeight::Regular, 18.0f * scale);
    // Lower third: clear of the HUD panels, the character and its name label, above the help line.
    const float top = std::round(h * 0.74f);
    // A soft dark cloud behind the text (fading out on all four sides), with a gold rule.
    const float bandH = std::round(nameFont.lineHeight() + tagFont.lineHeight() + 36.0f * scale);
    const float bandW = std::min(w, std::round(620.0f * scale));
    const Rect band{std::round((w - bandW) * 0.5f), top - std::round(14.0f * scale), bandW, bandH};
    constexpr int kStrips = 32; // horizontal fade: strips with a smooth alpha ramp
    const float strip = band.w / static_cast<float>(kStrips);
    const float halfH = std::round(band.h * 0.5f);
    for (int i = 0; i < kStrips; ++i)
    {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(kStrips);
        float ramp = 1.0f - std::fabs(u * 2.0f - 1.0f);
        ramp = std::min(1.0f, ramp * 2.2f);
        ramp = ramp * ramp * (3.0f - 2.0f * ramp);
        const float x0 = std::round(band.x + strip * static_cast<float>(i));
        const float x1 = std::round(band.x + strip * static_cast<float>(i + 1));
        const Color c = Color::hex(0x05070A, 0.5f * alpha * ramp);
        batch.fillRectGradient({x0, band.y, x1 - x0, halfH}, c.withAlpha(0.0f), c);
        batch.fillRectGradient({x0, band.y + halfH, x1 - x0, band.h - halfH}, c, c.withAlpha(0.0f));
    }
    render::TextStyle nameStyle;
    nameStyle.color = th.textTitle.withAlpha(alpha);
    nameStyle.shadow = true;
    nameStyle.shadowColor = Color::hex(0x000000, 0.8f * alpha);
    nameStyle.shadowOffset = std::max(1.0f, std::round(2.0f * scale));
    render::drawTextAligned(batch, nameFont, m_zoneName, {0.0f, top, w, nameFont.lineHeight()}, render::HAlign::Center,
                            render::VAlign::Top, nameStyle);
    const float ruleY = std::round(top + nameFont.lineHeight() + 4.0f * scale) + 0.5f;
    const float ruleHalf = std::round(150.0f * scale);
    batch.drawLine({w * 0.5f - ruleHalf, ruleY}, {w * 0.5f + ruleHalf, ruleY}, std::max(1.0f, scale),
                   th.ornament.withAlpha(0.7f * alpha));
    batch.fillDiamond({w * 0.5f, ruleY}, 4.0f * scale, th.ornament.withAlpha(alpha));
    if (!m_zoneTagline.empty())
    {
        render::TextStyle tagStyle = nameStyle;
        tagStyle.color = th.text.withAlpha(alpha);
        tagStyle.shadowOffset = std::max(1.0f, std::round(scale));
        render::drawTextAligned(batch, tagFont, m_zoneTagline, {0.0f, ruleY + 6.0f * scale, w, tagFont.lineHeight()},
                                render::HAlign::Center, render::VAlign::Top, tagStyle);
    }
}

void WorldScene::drawLabels()
{
    auto& batch = ctx().batch;
    const float scale = ui().scale();
    const render::Font& font = ctx().fonts.get(render::FontWeight::Bold, 15.0f * scale);
    const world::Mat4 vp = currentViewProj();
    const float w = ctx().viewportWidth;
    const float h = ctx().viewportHeight;
    const Vec3 eye = m_camera.eye();
    render::AssetCache& cache = *ctx().assets;

    // Characters hide labels behind them too (a box around each body; not the label's owner).
    std::array<world::LabelOccluder, 64> bodies{};
    std::array<std::uint32_t, 64> bodyOwner{};
    std::size_t bodyCount = 0;
    auto addBody = [&](std::uint32_t owner, Vec3 feet, float yaw, std::uint8_t race, std::uint8_t look) {
        if (bodyCount >= bodies.size())
            return;
        const render::GpuMesh* mesh = cache.mesh(cache.characterMeshPath(validRace(race), look));
        const float height = mesh != nullptr && mesh->cpu ? mesh->cpu->boundsMax.y : 1.8f;
        bodies[bodyCount] = world::makeOccluder(world::makeWorld(feet, yaw, 1.0f), {-0.28f, 0.0f, -0.28f},
                                                {0.28f, height * 0.92f, 0.28f});
        bodyOwner[bodyCount++] = owner;
    };
    addBody(m_model.localEntityId(), m_model.local().visualPosition(), m_model.local().mover().yaw(), m_race, m_look);
    for (const world::RemoteEntity& e : m_model.entities())
    {
        if (e.active && assets::length(e.position - eye) < kLabelHidden)
            addBody(e.id, e.position, e.yaw, e.race, e.face);
    }
    // Props and other characters between the camera and the head hide the label (CPU boxes, no
    // depth readback). The last 0.25 m before the head are not tested.
    auto occluded = [&](Vec3 head, std::uint32_t owner) {
        const Vec3 d = head - eye;
        const float len = assets::length(d);
        if (len < 0.3f)
            return false;
        const Vec3 end = eye + d * ((len - 0.25f) / len);
        for (std::size_t i = 0; i < bodyCount; ++i)
        {
            if (bodyOwner[i] != owner && world::segmentHitsOccluder(bodies[i], eye, end))
                return true;
        }
        for (const world::LabelOccluder& o : m_propOccluders)
        {
            if (world::segmentHitsOccluder(o, eye, end))
                return true;
        }
        return false;
    };

    auto label = [&](std::string_view text, Vec3 feet, std::uint8_t race, std::uint8_t look, const Color& color,
                     bool occlusionTest, std::uint32_t owner, float lift)
    {
        const render::GpuMesh* mesh = cache.mesh(cache.characterMeshPath(validRace(race), look));
        const float height = mesh != nullptr && mesh->cpu ? mesh->cpu->boundsMax.y : 1.8f;
        const Vec3 head = feet + Vec3{0.0f, height + 0.16f + lift, 0.0f};
        const float dist = assets::length(head - eye);
        if (dist > kLabelHidden)
            return;
        world::Vec2 s{};
        if (!world::projectToScreen(vp, head, w, h, s))
            return;
        if (s.x < -200.0f || s.x > w + 200.0f || s.y < -50.0f || s.y > h + 50.0f)
            return;
        // Hidden behind hills, props and other characters.
        if (occlusionTest && m_terrain != nullptr && !world::segmentClearOfTerrain(*m_terrain, eye, head, 0.1f, 2.0f))
            return;
        if (occlusionTest && occluded(head, owner))
            return;
        const float alpha = 1.0f - std::clamp((dist - kLabelFullAlpha) / (kLabelHidden - kLabelFullAlpha), 0.0f, 1.0f);
        render::TextStyle style;
        style.color = color.withAlpha(alpha);
        style.outline = true;
        style.outlineColor = Color::hex(0x000000, 0.85f * alpha);
        const float tw = render::measureText(font, text);
        const Vec2 topLeft{std::round(s.x - tw * 0.5f), std::round(s.y - font.lineHeight())};
        render::drawText(batch, font, text, topLeft, style);
    };

    const ui::Theme& t = theme();
    for (const world::RemoteEntity& e : m_model.entities())
    {
        if (e.active)
            label(e.name, e.position, e.race, e.face, Color::hex(0xF4F1E8), true, e.id, 0.0f);
    }
    // The own label stays visible (the camera follows the character; it is never "behind" anything),
    // except while the camera is pushed into the character (it is dithered out then).
    if (localOpacity() > 0.5f)
        label(m_name, m_model.local().visualPosition(), m_race, m_look, t.textTitle, false, m_model.localEntityId(),
              m_localAnim.heightOffset());
}

void WorldScene::drawLoading()
{
    auto& batch = ctx().batch;
    const ui::Theme& t = theme();
    const float w = ctx().viewportWidth;
    const float h = ctx().viewportHeight;
    const float scale = ui().scale();
    drawSceneBackground(batch, t, ctx().resources.background(), w, h, scale);
    if (m_phase == Phase::Failed)
        return;

    const render::AssetCache::Progress p = ctx().assets != nullptr ? ctx().assets->progress()
                                                                    : render::AssetCache::Progress{};
    // Terrain first, then everything it references: weight the two stages.
    float fraction = 0.0f;
    if (p.requested > 0)
        fraction = static_cast<float>(p.done) / static_cast<float>(p.requested);
    if (m_phase == Phase::LoadingTerrain)
        fraction *= 0.3f;

    const float boxW = std::round(460.0f * scale);
    const float boxH = std::round(118.0f * scale);
    const Rect box{std::round((w - boxW) * 0.5f), std::round(h * 0.62f), boxW, boxH};
    batch.fillRoundedRect(box, 8.0f * scale, Color::hex(0x0B0E12, 0.82f));
    batch.strokeRoundedRect(box, 8.0f * scale, std::max(1.0f, scale), t.panelBorder);
    render::TextStyle title;
    title.color = t.textTitle;
    title.shadow = true;
    const render::Font& titleFont = ctx().fonts.get(render::FontWeight::Bold, 22.0f * scale);
    render::drawTextAligned(batch, titleFont, formatInto(m_textBuffer, "{} yükleniyor…", m_zoneName),
                            {box.x, box.y + 14.0f * scale, box.w, titleFont.lineHeight()}, render::HAlign::Center,
                            render::VAlign::Top, title);
    // One short line of the story above the box.
    {
        render::TextStyle lore;
        lore.color = t.text;
        lore.shadow = true;
        lore.shadowColor = Color::hex(0x000000, 0.85f);
        lore.shadowOffset = std::max(1.0f, std::round(scale));
        const render::Font& loreFont = ctx().fonts.get(render::FontWeight::Regular, 17.0f * scale);
        const std::string_view line = kLoreLines[m_loreLine % kLoreLines.size()];
        const float lineY = std::round(box.y - loreFont.lineHeight() - 16.0f * scale);
        render::drawTextAligned(batch, loreFont, line, {0.0f, lineY, w, loreFont.lineHeight()}, render::HAlign::Center,
                                render::VAlign::Top, lore);
    }
    const Rect bar{box.x + 24.0f * scale, box.y + 58.0f * scale, box.w - 48.0f * scale, std::round(12.0f * scale)};
    batch.fillRoundedRect(bar, bar.h * 0.5f, Color::hex(0x1E242C));
    Rect fill = bar;
    fill.w = std::max(bar.h, bar.w * std::clamp(fraction, 0.0f, 1.0f));
    batch.fillRoundedRectGradient(fill, bar.h * 0.5f, t.accentBright, t.accent);
    render::TextStyle info;
    info.color = t.textDim;
    const render::Font& infoFont = ctx().fonts.get(render::FontWeight::Regular, 14.0f * scale);
    const char* stage = m_phase == Phase::LoadingTerrain ? "Arazi" : "Modeller ve dokular";
    render::drawTextAligned(batch, infoFont, formatInto(m_textBuffer, "{} · {}/{} dosya", stage, p.done, p.requested),
                            {box.x, bar.bottom() + 10.0f * scale, box.w, infoFont.lineHeight()}, render::HAlign::Center,
                            render::VAlign::Top, info);
}

// ---------------------------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------------------------

bool WorldScene::setRoute(Vec3 from, float x, float z, std::string& error)
{
    if (m_terrain == nullptr)
    {
        error = "no terrain";
        return false;
    }
    const auto path = game::findWalkPath(*m_terrain, {from.x, from.z}, {x, z});
    if (!path)
    {
        error = std::format("no walkable path from ({:.1f}, {:.1f}) to ({:.1f}, {:.1f})", from.x, from.z, x, z);
        return false;
    }
    m_route.clear();
    for (const game::PathPoint& pt : *path)
        m_route.push_back({pt.x, pt.z});
    m_routeNext = 1;
    m_pendingDestination.reset();
    m_model.local().mover().setDestination(m_route[0].x, m_route[0].y);
    m_markerPhase = 0.0f;
    VO_LOG_INFO("world: scripted walk to ({:.1f}, {:.1f}) over {} waypoint(s)", x, z, m_route.size());
    return true;
}

bool WorldScene::setRouteNear(Vec3 from, float x, float z, float searchRadius, std::string& error)
{
    if (m_terrain == nullptr)
    {
        error = "no terrain";
        return false;
    }
    // Clearance a bit above the mover's body radius, so the spot is not one it gets pushed off.
    const auto spot = game::findWalkPathNear(*m_terrain, {from.x, from.z}, {x, z}, searchRadius, 0.5f,
                                             m_model.params().bodyRadius + 0.2f);
    if (!spot)
    {
        error = std::format("no walkable path from ({:.1f}, {:.1f}) to ({:.1f}, {:.1f}) or within {:.1f} m of it",
                            from.x, from.z, x, z, searchRadius);
        return false;
    }
    if (std::hypot(spot->target.x - x, spot->target.z - z) > 0.01f)
        VO_LOG_INFO("world: ({:.1f}, {:.1f}) is blocked or unreachable; using the nearest clear spot ({:.1f}, {:.1f})",
                    x, z, spot->target.x, spot->target.z);
    return setRoute(from, spot->target.x, spot->target.z, error);
}


bool WorldScene::isBusy() const
{
    return m_phase == Phase::LoadingTerrain || m_phase == Phase::LoadingAssets || m_leavingWorld;
}

void WorldScene::extraStates(std::vector<std::string>& out) const
{
    switch (m_phase)
    {
    case Phase::LoadingTerrain:
    case Phase::LoadingAssets:
        out.emplace_back("loading");
        break;
    case Phase::Ready:
        out.emplace_back("ready");
        break;
    case Phase::Failed:
        out.emplace_back("failed");
        break;
    }
    if (m_menuOpen)
        out.emplace_back("menu");
    if (m_leavingWorld || m_model.leaving())
        out.emplace_back("leaving");
    const world::LocalMover& mover = m_model.local().mover();
    out.emplace_back(mover.moving() ? "moving" : "standing");
    out.emplace_back(mover.running() ? "running" : "walking");
    out.emplace_back(mover.destination() || routePending() ? "dest" : "nodest");
    out.emplace_back("remotes" + std::to_string(m_model.activeCount()));
    if (m_bannerTime >= 0.0f)
        out.emplace_back("banner");
    if (m_localAnim.action() == world::AnimAction::Jump)
        out.emplace_back("jumping");
    if (m_localAnim.seated())
        out.emplace_back(m_localAnim.state() == world::AnimState::SitIdle ? "seated" : "sitting");
    if (m_markSet)
    {
        for (const world::RemoteEntity& r : m_model.entities())
        {
            if (!r.active || r.name != m_markName)
                continue;
            if (std::hypot(r.position.x - m_markPos.x, r.position.z - m_markPos.z) >= 2.0f)
                out.emplace_back("remotemoved");
            const Vec3 me = m_model.local().mover().position();
            const float d = std::hypot(r.position.x - me.x, r.position.z - me.z);
            for (const int radius : {4, 8, 16})
            {
                if (d <= static_cast<float>(radius))
                    out.emplace_back("remotenear" + std::to_string(radius));
            }
        }
    }
}

bool WorldScene::automationCommand(std::string_view command, std::string& error)
{
    std::istringstream in{std::string(command)};
    std::string verb;
    in >> verb;
    auto fail = [&](std::string why)
    {
        error = std::move(why);
        return false;
    };
    if (m_phase != Phase::Ready)
        return fail("the world is not ready");
    world::LocalMover& mover = m_model.local().mover();
    const Vec3 p = mover.position();

    if (verb == "camera")
    {
        std::string yawText;
        float pitch = 0.0f;
        float dist = 0.0f;
        if (!(in >> yawText >> pitch >> dist))
            return fail("usage: camera <yawDeg|behind> <pitchDeg> <distance>");
        float yaw = mover.yaw();
        if (yawText != "behind")
        {
            const auto [ptr, ec] = std::from_chars(yawText.data(), yawText.data() + yawText.size(), yaw);
            if (ec != std::errc{} || ptr != yawText.data() + yawText.size())
                return fail("bad yaw");
            yaw = degToRad(yaw);
        }
        m_camera.setPose(yaw, degToRad(pitch), dist);
        m_camera.update(m_model.local().visualPosition() + Vec3{0.0f, kFocusHeight, 0.0f}, m_terrain, 1.0f / 60.0f);
        return true;
    }
    if (verb == "walkto")
    {
        float x = 0.0f;
        float z = 0.0f;
        if (!(in >> x >> z))
            return fail("usage: walkto <x> <z>");
        mover.setDestination(x, z);
        m_markerPhase = 0.0f;
        return true;
    }
    if (verb == "correct")
    {
        float back = 3.0f;
        in >> back;
        Vec3 target = p - world::forwardFromYaw(mover.yaw()) * back;
        if (m_terrain != nullptr && !m_terrain->isWalkable(target.x, target.z))
            target = p;
        world::DebugGhosts::correction(m_model.local().sender().lastSeq(), target, mover.yaw(), m_ghostSink);
        VO_LOG_INFO("world: debug POSITION_CORRECT injected (seq {}) -> ({:.2f}, {:.2f})",
                    m_model.local().sender().lastSeq(), target.x, target.z);
        return true;
    }
    if (verb == "expectnear" || verb == "expectfar")
    {
        float x = 0.0f;
        float z = 0.0f;
        float radius = 0.0f;
        if (!(in >> x >> z >> radius))
            return fail("usage: " + verb + " <x> <z> <r>");
        const float d = std::hypot(p.x - x, p.z - z);
        const bool isNear = d <= radius;
        if (isNear != (verb == "expectnear"))
            return fail(std::format("player at ({:.2f}, {:.2f}) is {:.2f} m from ({:.2f}, {:.2f})", p.x, p.z, d, x, z));
        return true;
    }
    // ---- Remote players (two-client flows) ----
    const auto findRemote = [&](const std::string& who) -> const world::RemoteEntity* {
        for (const world::RemoteEntity& r : m_model.entities())
        {
            if (r.active && r.name == who)
                return &r;
        }
        return nullptr;
    };
    if (verb == "walkrel")
    {
        float dx = 0.0f;
        float dz = 0.0f;
        if (!(in >> dx >> dz))
            return fail("usage: walkrel <dx> <dz>");
        return setRoute(p, p.x + dx, p.z + dz, error);
    }
    if (verb == "walkpath")
    {
        float x = 0.0f;
        float z = 0.0f;
        if (!(in >> x >> z))
            return fail("usage: walkpath <x> <z>");
        return setRouteNear(p, x, z, 2.0f, error);
    }
    if (verb == "expectremote" || verb == "expectnoremote" || verb == "markremote" || verb == "walktoremote" ||
        verb == "orbitremote" || verb == "expectnearremote")
    {
        std::string who;
        if (!(in >> who))
            return fail("usage: " + verb + " <name> ...");
        const world::RemoteEntity* r = findRemote(who);
        if (verb == "expectnoremote")
        {
            if (r != nullptr)
                return fail(std::format("remote '{}' is still visible at ({:.1f}, {:.1f})", who, r->position.x,
                                        r->position.z));
            return true;
        }
        if (r == nullptr)
            return fail(std::format("no remote player '{}' ({} visible)", who, m_model.activeCount()));
        const float dist = std::hypot(r->position.x - p.x, r->position.z - p.z);
        if (verb == "expectremote")
        {
            int race = 0;
            if (!(in >> race))
                return fail("usage: expectremote <name> <race>");
            if (r->race != race)
                return fail(std::format("remote '{}' has race {}, expected {}", who, r->race, race));
            VO_LOG_INFO("world: remote '{}' (entity {}, race {}, face {}, hair {}) at ({:.1f}, {:.1f}), {:.1f} m away",
                        who, r->id, r->race, r->face, r->hair, r->position.x, r->position.z, dist);
            return true;
        }
        if (verb == "markremote")
        {
            m_markName = who;
            m_markPos = r->position;
            m_markSet = true;
            VO_LOG_INFO("world: marked remote '{}' at ({:.2f}, {:.2f})", who, r->position.x, r->position.z);
            return true;
        }
        if (verb == "expectnearremote")
        {
            float radius = 0.0f;
            if (!(in >> radius))
                return fail("usage: expectnearremote <name> <r>");
            if (dist > radius)
                return fail(std::format("remote '{}' is {:.2f} m away (> {:.1f})", who, dist, radius));
            return true;
        }
        if (verb == "walktoremote")
        {
            float stopShort = 2.0f;
            in >> stopShort;
            const float k = dist > stopShort ? (dist - stopShort) / dist : 0.0f;
            // The point before it may be blocked (a remote next to a house): nearest clear spot.
            return setRouteNear(p, p.x + (r->position.x - p.x) * k, p.z + (r->position.z - p.z) * k, 2.0f, error);
        }
        // orbitremote: the point on the circle, else the nearest clear spot near it, else other
        // angles (alternating +-30 degrees) — a remote spawned next to a house still gets circled.
        float radius = 0.0f;
        float angle = 0.0f;
        if (!(in >> radius >> angle))
            return fail("usage: orbitremote <name> <radius> <angleDeg>");
        std::string lastError;
        for (int step = 0; step <= 12; ++step)
        {
            const int k = (step + 1) / 2;
            const float tryAngle = angle + static_cast<float>(step % 2 == 1 ? k : -k) * 30.0f;
            const float a = degToRad(tryAngle);
            if (setRouteNear(p, r->position.x + radius * std::sin(a), r->position.z + radius * std::cos(a), 1.5f,
                             lastError))
            {
                if (step != 0)
                    VO_LOG_INFO("world: orbitremote: {} deg is blocked, circling via {} deg", angle, tryAngle);
                return true;
            }
        }
        return fail(lastError);
    }
    if (verb == "expectlocalmesh" || verb == "expectremotemesh")
    {
        render::AssetCache* cache = ctx().assets;
        if (cache == nullptr)
            return fail("no assets");
        std::uint8_t race = m_race;
        std::uint8_t look = m_look;
        const world::Animator* animator = &m_localAnim;
        std::string who = m_name;
        if (verb == "expectremotemesh")
        {
            if (!(in >> who))
                return fail("usage: expectremotemesh <name>");
            const world::RemoteEntity* r = findRemote(who);
            if (r == nullptr)
                return fail(std::format("no remote player '{}'", who));
            race = validRace(r->race);
            look = r->face;
            animator = &r->animator;
            if (r->animatorMesh != cache->mesh(cache->characterMeshPath(race, look)))
                return fail(std::format("remote '{}' is animated on another mesh than {}", who,
                                        cache->characterMeshPath(race, look)));
        }
        const std::string& path = cache->characterMeshPath(race, look);
        const render::GpuMesh* mesh = cache->mesh(path);
        if (mesh == nullptr || !animator->bound())
            return fail(std::format("'{}' (race {}, look {}): mesh {} not loaded or not bound", who, race, look, path));
        const world::CharacterLookTable& looks = cache->looks();
        const std::string own = std::format("characters/{}_{}.vmesh", world::raceAssetName(race), look);
        if (looks.available(race, look) && !looks.files(race, look).legacy && path != own)
            return fail(std::format("look {} has its own mesh {} but {} is used", look, own, path));
        VO_LOG_INFO("world: '{}' race {} look {} -> {} (effective look {}, {} look(s))", who, race, look, path,
                    looks.effectiveLook(race, look), looks.lookCount(race));
        return true;
    }
    if (verb == "jump")
    {
        if (!m_localAnim.canJump())
            return fail("cannot jump now (no clips, or busy)");
        startJump();
        return true;
    }
    if (verb == "sit")
    {
        const bool wasSeated = m_localAnim.seated();
        toggleSit();
        if (m_localAnim.seated() == wasSeated && m_localAnim.state() != world::AnimState::SitExit)
            return fail("cannot sit down now (moving, no clips, or busy)");
        return true;
    }
    if (verb == "expectanimrate")
    {
        float lo = 0.0f;
        float hi = 0.0f;
        if (!(in >> lo >> hi))
            return fail("usage: expectanimrate <min> <max>");
        const world::AnimState s = m_localAnim.state();
        if (s != world::AnimState::Walk && s != world::AnimState::Run)
            return fail(std::format("local animation is {}, not walk/run", world::animStateName(s)));
        const float rate = m_localAnim.currentRate();
        VO_LOG_INFO("world: {} clip at {:.2f}x for {:.2f} m/s", world::animStateName(s), rate, mover.speed());
        if (rate < lo || rate > hi)
            return fail(std::format("{} plays at {:.2f}x, expected {:.2f}..{:.2f}", world::animStateName(s), rate, lo, hi));
        return true;
    }
    if (verb == "footprobe")
    {
        std::string mode;
        in >> mode;
        if (mode == "start")
        {
            const assets::MeshData* mesh = m_localAnim.mesh();
            if (mesh == nullptr)
                return fail("no local character");
            m_footProbe = {};
            m_footProbe.bone = {mesh->skeleton.findBone("ball_l"), mesh->skeleton.findBone("ball_r")};
            if (m_footProbe.bone[0] < 0 || m_footProbe.bone[1] < 0)
                m_footProbe.bone = {mesh->skeleton.findBone("foot_l"), mesh->skeleton.findBone("foot_r")};
            if (m_footProbe.bone[0] < 0 || m_footProbe.bone[1] < 0)
                return fail("no foot bones (ball_l/ball_r, foot_l/foot_r)");
            m_footProbe.lastBody = m_model.local().visualPosition();
            m_footProbe.active = true;
            return true;
        }
        float maxRatio = 0.0f;
        if (mode != "check" || !(in >> maxRatio))
            return fail("usage: footprobe start | footprobe check <maxSlideToTravelRatio>");
        FootProbe f = m_footProbe;
        m_footProbe.active = false;
        if (!f.active || f.time <= 0.0f || f.contactTime <= 0.0f)
            return fail("no foot contact recorded (footprobe start first, then walk)");
        const float bodySpeed = f.travel / f.time;
        const float slideSpeed = f.slide / f.contactTime;
        const float ratio = bodySpeed > 1e-3f ? slideSpeed / bodySpeed : 0.0f;
        VO_LOG_INFO("world: foot probe over {:.2f} s: body {:.2f} m/s, planted feet slide {:.3f} m/s "
                    "({:.0f}% of the body speed, {:.2f} foot-s in contact), anim {} at {:.2f}x",
                    f.time, bodySpeed, slideSpeed, ratio * 100.0f, f.contactTime,
                    world::animStateName(m_localAnim.state()), m_localAnim.currentRate());
        if (ratio > maxRatio)
            return fail(std::format("planted feet slide at {:.0f}% of the body speed (> {:.0f}%)", ratio * 100.0f,
                                    maxRatio * 100.0f));
        return true;
    }
    if (verb == "cameracollision")
    {
        std::string mode;
        in >> mode;
        if (mode != "on" && mode != "off")
            return fail("usage: cameracollision on|off");
        m_cameraCollision = mode == "on";
        m_camera.setObstacles(m_cameraCollision ? &m_cameraObstacles : nullptr);
        m_camera.update(m_model.local().visualPosition() + Vec3{0.0f, kFocusHeight, 0.0f}, m_terrain, 1.0f / 60.0f);
        return true;
    }
    if (verb == "expectcameraoutside" || verb == "expectcamerapulled")
    {
        const Vec3 e = m_camera.eye();
        VO_LOG_INFO("world: camera eye ({:.2f}, {:.2f}, {:.2f}), boom {:.2f} of {:.2f} m, local opacity {:.2f}", e.x, e.y,
                    e.z, m_camera.effectiveDistance(), m_camera.distance(), localOpacity());
        if (verb == "expectcameraoutside" && m_cameraObstacles.contains(e))
            return fail(std::format("camera eye ({:.2f}, {:.2f}, {:.2f}) is inside an obstacle", e.x, e.y, e.z));
        if (verb == "expectcamerapulled" && !(m_camera.effectiveDistance() < m_camera.distance() - 0.5f))
            return fail(std::format("boom {:.2f} of {:.2f} m: not pulled in", m_camera.effectiveDistance(),
                                    m_camera.distance()));
        return true;
    }
    if (verb == "expectlocalfaded" || verb == "expectlocalvisible")
    {
        const float o = localOpacity();
        if (verb == "expectlocalfaded" ? !(o < 1.0f) : !(o >= 1.0f))
            return fail(std::format("local character opacity {:.2f}", o));
        return true;
    }
    if (verb == "expectwalkable")
    {
        if (m_terrain == nullptr || !m_terrain->isWalkable(p.x, p.z))
            return fail(std::format("player at ({:.2f}, {:.2f}) is on a blocked cell", p.x, p.z));
        return true;
    }
    if (verb == "expectgrounded")
    {
        if (m_terrain == nullptr)
            return fail("no terrain");
        const float ground = m_terrain->heightAt(p.x, p.z);
        if (std::fabs(p.y - ground) > 0.01f)
            return fail(std::format("feet at y {:.3f}, ground {:.3f}", p.y, ground));
        const Vec3 e = m_camera.eye();
        const float under = m_terrain->heightAt(e.x, e.z);
        if (e.y < under + 0.2f)
            return fail(std::format("camera at y {:.2f}, ground {:.2f}", e.y, under));
        return true;
    }
    if (verb == "scan")
    {
        // Terrain survey for writing flows: highest walkable point within a radius, its distance and
        // the average slope towards it.
        float cx = 0.0f;
        float cz = 0.0f;
        float radius = 0.0f;
        if (!(in >> cx >> cz >> radius) || m_terrain == nullptr)
            return fail("usage: scan <x> <z> <radius>");
        Vec3 best{cx, m_terrain->heightAt(cx, cz), cz};
        for (float z = cz - radius; z <= cz + radius; z += 2.0f)
        {
            for (float x = cx - radius; x <= cx + radius; x += 2.0f)
            {
                if (std::hypot(x - cx, z - cz) > radius || !m_terrain->isWalkable(x, z))
                    continue;
                const float hgt = m_terrain->heightAt(x, z);
                if (hgt > best.y)
                    best = {x, hgt, z};
            }
        }
        const float h0 = m_terrain->heightAt(cx, cz);
        const float d = std::hypot(best.x - cx, best.z - cz);
        VO_LOG_INFO("world: scan ({:.1f}, {:.1f}) r {:.0f}: ground {:.2f}, highest walkable ({:.1f}, {:.1f}) at {:.2f} "
                    "({:.1f} m away, rise {:.2f} m, {:.1f} deg avg)",
                    cx, cz, radius, h0, best.x, best.z, best.y, d, best.y - h0,
                    d > 0.0f ? std::atan2(best.y - h0, d) * 180.0f / world::kPi : 0.0f);
        return true;
    }
    if (verb == "log")
    {
        const Vec3 e = m_camera.eye();
        VO_LOG_INFO("world: player ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f} deg, {} {} speed {:.2f}; camera ({:.2f}, {:.2f}, "
                    "{:.2f}) yaw {:.1f} pitch {:.1f} dist {:.1f}; anim {}; {} remote(s); corrections {} ({} ignored); "
                    "MOVE_REQ sent {}",
                    p.x, p.y, p.z, mover.yaw() * 180.0f / world::kPi, mover.moving() ? "moving" : "standing",
                    mover.running() ? "running" : "walking", mover.speed(), e.x, e.y, e.z,
                    m_camera.yaw() * 180.0f / world::kPi, m_camera.pitch() * 180.0f / world::kPi,
                    m_camera.effectiveDistance(), world::animStateName(m_localAnim.state()), m_model.activeCount(),
                    m_model.stats().corrections, m_model.stats().ignoredCorrections, m_model.local().sender().sentCount());
        if (ctx().worldRenderer != nullptr)
        {
            const render::WorldRenderStats& s = ctx().worldRenderer->stats();
            VO_LOG_INFO("world: perf {:.1f} fps ({}x{}); 3D {} draw calls / {} triangles (terrain chunks {}, prop "
                        "instances {}, characters {}), shadow pass {} / {}, 2D {} draw calls",
                        ctx().fps, ctx().device.width(), ctx().device.height(), s.drawCalls, s.triangles,
                        s.terrainChunks, s.propInstances, s.characters, s.shadowDrawCalls, s.shadowTriangles,
                        ctx().lastBatchStats.drawCalls);
        }
        for (const world::RemoteEntity& r : m_model.entities())
        {
            if (r.active)
                VO_LOG_INFO("world:   remote {} '{}' race {} at ({:.1f}, {:.1f}) speed {:.2f} {}{} anim {}", r.id, r.name,
                            r.race, r.position.x, r.position.z, r.speed, r.running ? "run" : "walk",
                            r.holding ? " (holding)" : "", world::animStateName(r.animator.state()));
        }
        return true;
    }
    return fail("unknown world command '" + verb + "'");
}

} // namespace vo::client::scenes
