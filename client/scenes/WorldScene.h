// The 3D world (M6). Replaces the M4 placeholder.
//
// Loading: right after ENTER_WORLD_RES the scene requests the zone terrain, its prop meshes and
// layer textures and both races' character assets from the AssetCache (worker thread + per-frame
// GPU uploads) and shows a progress screen; world messages that arrive meanwhile are applied to the
// WorldModel at once. Then: terrain, props, characters, sky; local movement is predicted
// (client/world/LocalMovement.h) and MOVE_REQ is sent as docs/PROTOCOL.md §5.4 specifies; remote
// players are interpolated 100 ms in the past.
//
// Controls (Turkish help line on screen): W/S forward/back and A/D sideways, all relative to the
// camera (the character turns to face where it goes); left click on the ground walks/runs there
// (WASD cancels); right mouse drag turns the camera around the character (cursor hidden; the vertical angle is fixed
// and only changes with PageUp / PageDown); wheel zooms; Shift held =
// walk, R toggles the default run/walk; Space jumps and X sits down / stands up (cosmetic, local
// only: other players do not see it, nothing is sent); F3 toggles the info panel; Esc opens the
// menu ("Devam Et", "Karakter Seçimine Dön" = LEAVE_WORLD_REQ, "Oyundan Çık").
//
// Story: the loading screen names the zone (shared/game/Zones.h) with one short lore line; on
// entering, a banner "<zone> — <tagline>" fades in and out; the HUD shows the zone name.
// The camera collides with the terrain and with the zone's large props (CameraObstacles); the local
// character dithers out when the camera is pushed closer than ~0.8 m.
//
// Automation: states "world", "loading" | "ready", "menu", "moving" | "standing", "running" |
// "walking", "dest", "remotes<N>", "banner" (zone banner visible), "jumping", "seated"; widgets
// world.name, world.level, world.zone, world.position, world.rtt, world.fps, world.menu.resume /
// .charselect / .quit. `scene` commands:
//   camera <yawDeg|behind> <pitchDeg> <distance>   set the orbit pose (yaw is absolute world yaw)
//   walkto <x> <z>                                  click-to-move to a world point (same path as a click)
//   correct [metres]                                inject a POSITION_CORRECT that puts the player
//                                                   `metres` (default 3) behind its position
//   expectnear <x> <z> <r> / expectfar <x> <z> <r>  player within / beyond r metres of (x, z)
//   expectwalkable                                  player stands on a walkable cell
//   expectgrounded                                  feet on the terrain, camera above it
//   log                                             write player/camera/remote state to the log
//   walkrel <dx> <dz>                               click-to-move relative to the player
//   walkpath <x> <z>                                path-found walk to (x, z) (nearest clear spot)
//   expectremote <name> <race> / expectnoremote <name>   a remote player with that name (and race)
//   markremote <name>                               remember its position: state "remotemoved" once
//                                                   it is 2 m away from there, "remotenear<r>"
//                                                   (r = 4, 8, 16) while within r metres of the player
//   walktoremote <name> <stopShort>                 click-to-move to `stopShort` metres before it
//   orbitremote <name> <radius> <angleDeg>          click-to-move to a point on a circle around it
//                                                   (both pick the nearest clear walkable spot, and
//                                                   orbitremote other angles, if the point is blocked)
//   expectnearremote <name> <r>                     player within r metres of it
//   expectlocalmesh / expectremotemesh <name>       the character is drawn with the mesh of its
//                                                   race + look (client/world/CharacterAssets.h)
//   jump / sit                                      as Space / X
//   expectanimrate <min> <max>                      local walk/run playback rate within [min, max]
//   cameracollision on|off                          obstacle collision of the camera (screenshots)
//   expectcameraoutside                             the eye is not inside an obstacle box
//   expectcamerapulled                              obstacle collision shortened the boom
//   expectlocalfaded / expectlocalvisible           the local character is dithered out / opaque
#pragma once

#include "client/scenes/UiScene.h"
#include "client/world/Animator.h"
#include "client/world/CameraObstacles.h"
#include "client/world/DebugGhosts.h"
#include "client/world/OrbitCamera.h"
#include "client/world/TerrainQuery.h"
#include "client/world/WorldModel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vo::client::render {
struct GpuMesh;
}
namespace vo::client::ui {
class Label;
class Panel;
class Widget;
} // namespace vo::client::ui

namespace vo::client::scenes {

class WorldScene final : public UiScene
{
public:
    WorldScene();
    ~WorldScene() override;

    [[nodiscard]] const char* name() const noexcept override { return "world"; }
    void render() override;
    bool automationCommand(std::string_view command, std::string& error) override;

protected:
    void build() override;
    void teardown() override;
    void onUnhandledKey(const platform::InputEvent& e) override;
    void onUnhandledEvent(const platform::InputEvent& e) override;
    void onNetEvent(const net::NetEvent& e) override;
    void onFrame(double dt) override;
    [[nodiscard]] bool isBusy() const override;
    void extraStates(std::vector<std::string>& out) const override;

private:
    enum class Phase : std::uint8_t
    {
        LoadingTerrain,
        LoadingAssets,
        Ready,
        Failed,
    };

    void buildHud();
    void pollLoading();
    void becomeReady();
    void failLoading(const std::string& why);
    void updateWorld(float dt);
    void updateCamera(float dt);
    void updateHud(float dt);
    void injectGhostPacket(std::uint16_t opcode, std::vector<std::uint8_t> payload);
    void openMenu();
    void closeMenu();
    void leaveWorld();
    void stopDrag();
    void clickToMove(int x, int y);
    [[nodiscard]] world::MoveIntent readIntent() const;
    [[nodiscard]] world::Mat4 currentViewProj() const;

    void drawLoading();
    void drawLabels();
    void drawZoneBanner();
    void addCharacterDraw(const render::GpuMesh* mesh, world::Animator& animator, world::Vec3 pos, float yaw,
                          float alpha, float opacity = 1.0f);
    void buildCameraObstacles();
    // Binds every active remote's animator to its race + look's mesh and clips (once loaded).
    void bindRemoteAnimators();
    // Opacity of the local character for the current camera (dithered out when the eye is close).
    [[nodiscard]] float localOpacity() const;
    void startJump();
    void toggleSit();

    Phase m_phase = Phase::LoadingTerrain;
    std::string m_terrainPath;
    const assets::TerrainData* m_terrain = nullptr;
    double m_time = 0.0; // local simulation clock (s)
    float m_loadingTime = 0.0f;

    world::WorldModel m_model;
    world::OrbitCamera m_camera;
    world::Animator m_localAnim;
    world::DebugGhosts m_ghosts;
    world::DebugGhosts::Sink m_ghostSink;
    std::uint8_t m_race = 1;
    std::uint8_t m_look = 0; // CharSummary.face
    std::string m_name;
    std::string m_zoneName;
    std::string m_zoneTagline;
    std::size_t m_loreLine = 0; // which loading-screen lore line
    bool m_runDefault = true;
    // Camera vs. buildings.
    world::CameraObstacles m_cameraObstacles;
    bool m_cameraCollision = true;
    float m_bannerTime = -1.0f; // seconds since the zone banner started (< 0: not shown)
    // Click-to-move target given while seated: walked to once the character has stood up.
    std::optional<world::Vec2> m_pendingDestination;
    std::size_t m_shownPlayers = static_cast<std::size_t>(-1); // HUD player count last shown
    // Automation: foot slide probe (`scene footprobe start|check <maxRatio>`): while active, how far
    // the local character's ball-of-foot bones slide horizontally while in ground contact.
    struct FootProbe
    {
        bool active = false;
        std::array<int, 2> bone{-1, -1};
        std::array<world::Vec3, 2> last{};
        std::array<bool, 2> contact{};
        float slide = 0.0f;       // metres slid during contact
        float contactTime = 0.0f; // foot-seconds in contact
        float travel = 0.0f;      // metres the body moved
        float time = 0.0f;
        world::Vec3 lastBody{};
    };
    FootProbe m_footProbe;
    void updateFootProbe(float dt);

    // Mouse look.
    bool m_dragging = false;
    int m_dragAnchorX = 0;
    int m_dragAnchorY = 0;
    int m_lastMouseX = 0;
    int m_lastMouseY = 0;
    float m_pendingYaw = 0.0f;
    float m_pendingPitch = 0.0f;
    float m_pendingZoom = 0.0f;
    float m_markerPhase = 0.0f;

    // HUD.
    bool m_leavingWorld = false;
    bool m_menuOpen = false;
    bool m_showInfo = true;
    float m_hudTimer = 0.0f;
    ui::Panel* m_infoPanel = nullptr;
    ui::Label* m_position = nullptr;
    ui::Label* m_rtt = nullptr;
    ui::Label* m_fps = nullptr;
    ui::Label* m_stats = nullptr;
    ui::Widget* m_menuLayer = nullptr;
    ui::Panel* m_hudPanel = nullptr;
    ui::Label* m_zoneLabel = nullptr;
    ui::Label* m_help = nullptr;
    std::array<char, 160> m_textBuffer{};

    // Automation: a remote player marked with `scene markremote <name>` (states remotemoved/remotenear).
    std::string m_markName;
    world::Vec3 m_markPos{};
    bool m_markSet = false;
    // Automation: remaining waypoints of a path-found scripted walk (walktoremote / orbitremote).
    std::vector<world::Vec2> m_route;
    // Bounding boxes of the zone's props, built once the world is ready (name-label occlusion).
    std::vector<world::LabelOccluder> m_propOccluders;
    std::size_t m_routeNext = 0;
    [[nodiscard]] bool routePending() const noexcept { return m_routeNext < m_route.size(); }
    bool setRoute(world::Vec3 from, float x, float z, std::string& error);
    // As setRoute, but to the nearest clear walkable spot within `searchRadius` if (x, z) is blocked
    // or unreachable.
    bool setRouteNear(world::Vec3 from, float x, float z, float searchRadius, std::string& error);
};

} // namespace vo::client::scenes
