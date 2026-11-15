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
    m_face = character.face;
    m_hair = character.hair;
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
    if (const render::GpuMesh* mesh = cache.mesh(render::AssetCache::characterMeshPath(m_race)))
        m_localAnim.bind(mesh->cpu.get(), cache.characterClips(m_race));
    if (!m_localAnim.bound())
        VO_LOG_WARN("world: character assets of race {} missing; the local character is not drawn", m_race);

    if (ctx().debugGhosts > 0)
    {
        m_ghosts.start(ctx().debugGhosts, mover.position(), m_terrain, m_model.clock(), m_time, m_ghostSink);
        VO_LOG_INFO("world: --debug-ghosts: {} synthetic remote players", m_ghosts.count());
    }
    m_phase = Phase::Ready;
    m_hudPanel->setVisible(true);
    m_infoPanel->setVisible(m_showInfo);
    m_help->setVisible(true);
    const auto p = cache.progress();
    VO_LOG_INFO("world: ready after {:.2f} s ({} assets, {} failed); player at ({:.2f}, {:.2f}, {:.2f})",
                m_loadingTime, p.requested, p.failed, mover.position().x, mover.position().y, mover.position().z);
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
    // Scripted path walk: the next waypoint once the previous one is reached.
    world::LocalMover& walker = m_model.local().mover();
    if (routePending() && !walker.destination() && !m_model.leaving())
    {
        const world::Vec2 next = m_route[m_routeNext++];
        walker.setDestination(next.x, next.y);
    }
    const world::MoveIntent intent = readIntent();
    if (const auto req = m_model.updateLocal(m_time, intent, dt))
        (void)ctx().net.send(*req);
    if (m_ghosts.count() > 0)
        m_ghosts.update(m_time, dt, m_model.clock(), m_model.local().mover().position(), m_model.params(),
                        m_ghostSink);
    m_model.updateRemotes(m_time, dt);

    const world::LocalMover& mover = m_model.local().mover();
    if (m_localAnim.bound())
        m_localAnim.update(dt, mover.speed(), mover.running());
    render::AssetCache& cache = *ctx().assets;
    for (world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active)
            continue;
        const std::uint8_t race = validRace(e.race);
        if (e.animatorRace != race)
        {
            const render::GpuMesh* mesh = cache.mesh(render::AssetCache::characterMeshPath(race));
            const world::CharacterClips* clips = cache.characterClips(race);
            if (mesh != nullptr && clips != nullptr)
            {
                e.animator.bind(mesh->cpu.get(), clips);
                e.animatorRace = race;
            }
        }
        if (e.animator.bound())
            e.animator.update(dt, e.speed, e.running);
    }
    m_markerPhase += dt;
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
    if (m_hudTimer > 0.0f || m_position == nullptr)
        return;
    m_hudTimer = 0.25f;
    // Formatted into a fixed buffer; Label::setText reuses its string: no steady-state allocation.
    auto& b = m_textBuffer;
    const Vec3 p = m_model.local().mover().position();
    m_position->setText(formatInto(b, "Konum: X {:.1f} · Y {:.1f} · Z {:.1f} · Bölge {}", p.x, p.y, p.z,
                                   m_model.zoneId()));
    const auto rtt = ctx().net.rttMs();
    const std::size_t players = m_model.activeCount();
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
    m_model.local().mover().setDestination(hit.x, hit.z);
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
                                  std::uint8_t face, std::uint8_t hair, float alpha)
{
    if (mesh == nullptr || !animator.bound())
        return;
    render::WorldRenderer& r = *ctx().worldRenderer;
    animator.evaluate();
    render::CharacterDraw d;
    d.mesh = mesh;
    d.skin = animator.skinMatrices();
    d.world = world::makeWorld(pos, yaw, 1.0f);
    d.face = face;
    d.hair = hair;
    const float fade = 0.35f + 0.65f * std::clamp(alpha, 0.0f, 1.0f); // fresh spawns brighten in
    d.tint = {fade, fade, fade, 1.0f};
    r.addCharacter(d);
    render::DecalDraw blob;
    blob.center = pos;
    blob.normal = m_terrain != nullptr ? m_terrain->normalAt(pos.x, pos.z) : Vec3{0.0f, 1.0f, 0.0f};
    blob.radius = 0.62f;
    blob.color = {0.0f, 0.0f, 0.0f, 0.42f};
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
    addCharacterDraw(cache.mesh(render::AssetCache::characterMeshPath(m_race)), m_localAnim,
                     m_model.local().visualPosition(), mover.yaw(), m_face, m_hair, 1.0f);
    for (world::RemoteEntity& e : m_model.entities())
    {
        if (!e.active)
            continue;
        addCharacterDraw(cache.mesh(render::AssetCache::characterMeshPath(validRace(e.race))), e.animator, e.position,
                         e.yaw, e.face, e.hair, e.age / 0.4f);
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
    ui().draw(batch);
    batch.end();
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

    auto label = [&](std::string_view text, Vec3 feet, std::uint8_t race, const Color& color, bool occlusionTest)
    {
        const render::GpuMesh* mesh = cache.mesh(render::AssetCache::characterMeshPath(validRace(race)));
        const float height = mesh != nullptr && mesh->cpu ? mesh->cpu->boundsMax.y : 1.8f;
        const Vec3 head = feet + Vec3{0.0f, height + 0.16f, 0.0f};
        const float dist = assets::length(head - eye);
        if (dist > kLabelHidden)
            return;
        world::Vec2 s{};
        if (!world::projectToScreen(vp, head, w, h, s))
            return;
        if (s.x < -200.0f || s.x > w + 200.0f || s.y < -50.0f || s.y > h + 50.0f)
            return;
        // Hidden behind hills (props do not hide labels).
        if (occlusionTest && m_terrain != nullptr && !world::segmentClearOfTerrain(*m_terrain, eye, head, 0.1f, 2.0f))
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
            label(e.name, e.position, e.race, Color::hex(0xF4F1E8), true);
    }
    label(m_name, m_model.local().visualPosition(), m_race, t.textTitle, false);
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
    render::drawTextAligned(batch, titleFont, "Dünya yükleniyor…", {box.x, box.y + 14.0f * scale, box.w, titleFont.lineHeight()},
                            render::HAlign::Center, render::VAlign::Top, title);
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
    m_model.local().mover().setDestination(m_route[0].x, m_route[0].y);
    m_markerPhase = 0.0f;
    VO_LOG_INFO("world: scripted walk to ({:.1f}, {:.1f}) over {} waypoint(s)", x, z, m_route.size());
    return true;
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
            return setRoute(p, p.x + (r->position.x - p.x) * k, p.z + (r->position.z - p.z) * k, error);
        }
        // orbitremote
        float radius = 0.0f;
        float angle = 0.0f;
        if (!(in >> radius >> angle))
            return fail("usage: orbitremote <name> <radius> <angleDeg>");
        const float a = degToRad(angle);
        return setRoute(p, r->position.x + radius * std::sin(a), r->position.z + radius * std::cos(a), error);
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
