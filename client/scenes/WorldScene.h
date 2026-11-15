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
// (WASD cancels); right mouse drag orbits the camera (cursor hidden); wheel zooms; Shift held =
// walk, R toggles the default run/walk; F3 toggles the info panel; Esc opens the menu ("Devam Et",
// "Karakter Seçimine Dön" = LEAVE_WORLD_REQ, "Oyundan Çık").
//
// Automation: states "world", "loading" | "ready", "menu", "moving" | "standing", "running" |
// "walking", "dest", "remotes<N>"; widgets world.name, world.level, world.position, world.rtt,
// world.fps, world.menu.resume / .charselect / .quit. `scene` commands:
//   camera <yawDeg|behind> <pitchDeg> <distance>   set the orbit pose (yaw is absolute world yaw)
//   walkto <x> <z>                                  click-to-move to a world point (same path as a click)
//   correct [metres]                                inject a POSITION_CORRECT that puts the player
//                                                   `metres` (default 3) behind its position
//   expectnear <x> <z> <r> / expectfar <x> <z> <r>  player within / beyond r metres of (x, z)
//   expectwalkable                                  player stands on a walkable cell
//   expectgrounded                                  feet on the terrain, camera above it
//   log                                             write player/camera/remote state to the log
//   walkrel <dx> <dz>                               click-to-move relative to the player
//   expectremote <name> <race> / expectnoremote <name>   a remote player with that name (and race)
//   markremote <name>                               remember its position: state "remotemoved" once
//                                                   it is 2 m away from there, "remotenear<r>"
//                                                   (r = 4, 8, 16) while within r metres of the player
//   walktoremote <name> <stopShort>                 click-to-move to `stopShort` metres before it
//   orbitremote <name> <radius> <angleDeg>          click-to-move to a point on a circle around it
//   expectnearremote <name> <r>                     player within r metres of it
#pragma once

#include "client/scenes/UiScene.h"
#include "client/world/Animator.h"
#include "client/world/DebugGhosts.h"
#include "client/world/OrbitCamera.h"
#include "client/world/TerrainQuery.h"
#include "client/world/WorldModel.h"

#include <array>
#include <cstdint>
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
    void addCharacterDraw(const render::GpuMesh* mesh, world::Animator& animator, world::Vec3 pos, float yaw,
                          std::uint8_t face, std::uint8_t hair, float alpha);

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
    std::uint8_t m_face = 0;
    std::uint8_t m_hair = 0;
    std::string m_name;
    bool m_runDefault = true;

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
};

} // namespace vo::client::scenes
