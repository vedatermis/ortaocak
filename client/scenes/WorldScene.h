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
// walk, R toggles the default run/walk; Space jumps (cosmetic, local only: nothing is sent) and X
// sits down / stands up (M9: STANCE_REQ, the pose follows ENTITY_STATE); F3 toggles the info panel; Esc opens the
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
//
// Items (M8, docs/PROTOCOL_ITEMS.md): INVENTORY / ITEM_UPDATE / ITEM_MOVE_RES feed a
// world::ClientInventory (optimistic moves, rollback + Turkish message on a refusal), ENTITY_EQUIP the
// remotes' weapons. I or the HUD button "Envanter" (world.inventory) toggles the InventoryWindow;
// Esc closes it before it opens the menu. Weapons are drawn in the hands of every character
// (attachments.toml). States: "inv" (window open), "invloaded" (INVENTORY received), "itemsidle"
// (no move waiting for an answer). `scene` commands (places: main, off, head, chest, legs, hands,
// feet, reserved, bag<N>):
//   inventory open|close                            open / close the window
//   itemmove <from> <to> [force]                    as a drag & drop; `force` sends a move the local
//                                                   rules refuse (tests the server's answer)
//   expectequip main <id> off <id>                  the local character's MainHand / OffHand (0 = empty)
//   expectbag <slot> <id|empty>                     one bag slot of the shown inventory
//   expectremoteequip <name> <main> <off>           a remote's weapons (ENTITY_SPAWN / ENTITY_EQUIP)
//   expectitemmessage <text>                        the last item message contains <text>
//   expectlocalweapons <n>                          n weapon meshes attached to the local character
//
// Combat (M9, docs/PROTOCOL_COMBAT.md; client/scenes/WorldCombat.cpp, docs/CLIENT_RENDERING.md §10):
// monsters (entity_type 2) drawn from gamedata/monsters.toml + assets/monsters/; left click selects,
// Tab cycles monsters, Esc drops the target (after the inventory, before the menu); double click /
// right click without drag / F auto-attack (running into range), 1 the class skill; X asks the
// server to sit (STANCE_REQ). HUD: hp / mp bars, target frame, xp bar, skill button (world.skill),
// cast bar, floating numbers, death screen (world.death, world.death.respawn), level-up effect, safe
// zone indicator. Combat `scene` commands and states: see CombatView.h parseCombatCommand and
// WorldScene::combatStates().
//
// Loot (M10, docs/PROTOCOL_LOOT.md; client/scenes/WorldLoot.cpp, docs/CLIENT_RENDERING.md §11): loot
// containers (entity_type 3) are drawn as props/loot_bag.vmesh with a "Ganimet" label and a ground ring
// (gold and breathing: ours; grey and dim: someone else's for now; warm: free for all). Left / right
// click on a bag or E opens it (walking into range first); the LootWindow lists Akçe and items, a
// click takes a row, "Hepsini Al" / E takes everything; Esc closes it (Esc order: loot window,
// inventory, target, menu). It closes on LOOT_CLOSED, when the bag despawns, beyond 4.5 m and on
// death. COINS feeds the Akçe balance of the inventory window; pickups show "+7 Akçe" / "Kül Tozu x2
// alındı" lines above the combat message. Loot `scene` commands: LootView.h parseLootCommand; states:
// WorldScene::lootStates().
#pragma once

#include "client/net/GameClient.h"
#include "client/scenes/UiScene.h"
#include "client/world/Animator.h"
#include "client/world/CameraObstacles.h"
#include "client/world/ClientInventory.h"
#include "client/world/CombatView.h"
#include "client/world/DebugGhosts.h"
#include "client/world/LootView.h"
#include "client/world/OrbitCamera.h"
#include "client/world/TerrainQuery.h"
#include "client/world/WorldModel.h"
#include "shared/game/Combat.h"
#include "shared/game/Monsters.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vo::client::render {
struct GpuMesh;
}
namespace vo::client::ui {
class Button;
class Label;
class Panel;
class Widget;
} // namespace vo::client::ui

namespace vo::client::scenes {

class InventoryWindow;
class LootWindow;

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
    bool onPreviewEvent(const platform::InputEvent& e) override;
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
                          float alpha, float opacity, std::uint8_t race, std::uint8_t look, std::uint16_t mainHand,
                          std::uint16_t offHand, float scale = 1.0f, float glow = 0.0f);
    // ---- Items (M8) ----
    void buildInventoryWindow();
    void setInventoryOpen(bool open);
    // Asks for a move (drag & drop, quick move, automation); `force` sends a locally refused one.
    bool requestItemMove(game::ItemPlace from, game::ItemPlace to, bool force = false);
    void showItemMessage(const std::string& text, bool error);
    // MainHand / OffHand the local character is drawn with.
    [[nodiscard]] game::VisibleEquipment localEquipment() const;
    bool handleItemMessage(const net::NetEvent& e);
    void buildCameraObstacles();
    // Binds every active remote's animator to its race + look's mesh and clips (once loaded).
    void bindRemoteAnimators();
    // Opacity of the local character for the current camera (dithered out when the eye is close).
    [[nodiscard]] float localOpacity() const;
    void startJump();
    void toggleSit();

    // ---- Combat (M9, client/scenes/WorldCombat.cpp) ----
    void initCombat(const vo::net::EnterWorldRes& enter, std::uint8_t charClass);
    void buildCombatHud();
    void requestCombatAssets();
    // Combat messages (COMBAT_EVENT, CAST_*, ENTITY_STATS/STATE/DEATH, SELF_STATS, XP_GAIN, LEVEL_UP,
    // RESPAWN_RES, ACTION_FAIL); false if not a combat opcode.
    bool handleCombatMessage(const net::NetEvent& e);
    void updateCombat(float dt);
    void layoutCombatHud();
    void drawCombatHud();
    void drawCombatOverlays(const world::Mat4& vp); // numbers, monster / player hp bars
    void addCombatDraws();                         // selection ring, level-up ring, bolts
    // Entity under the cursor (monsters and players, not the local character); 0 if none.
    [[nodiscard]] std::uint32_t pickAt(int x, int y) const;
    void setTarget(std::uint32_t id);
    void clearTarget();
    void cycleTarget();
    // F / right click / double click: auto-attack the target, walking into range.
    void startAttack();
    void stopAttack(bool sendStop);
    // Key 1: the class skill (walks into range first if needed).
    void useSkill();
    void sendSkill(std::uint16_t skillId);
    void requestRespawn();
    void requestStance(bool sit);
    void showCombatMessage(std::string_view text);
    void bindMonsterAnimator(world::RemoteEntity& e);
    void onLocalDeath();
    void onLocalRevive();
    // Combat requests (ATTACK/SKILL/TARGET/STANCE) share the server's 10/s window: kept below it.
    template <class T>
    bool sendCombat(const T& msg)
    {
        if (!combatBudget() || m_leavingWorld || m_model.leaving() || ctx().net.movesBlocked())
            return false;
        noteCombatSend();
        return ctx().net.send(msg);
    }
    [[nodiscard]] bool combatBudget() const;
    void noteCombatSend();
    [[nodiscard]] bool localDead() const noexcept { return m_self.dead; }
    [[nodiscard]] float attackRange() const;
    [[nodiscard]] const game::MonsterTemplate* monsterTemplate(const world::RemoteEntity& e) const;
    [[nodiscard]] world::Vec3 headPoint(const world::RemoteEntity& e) const;
    [[nodiscard]] bool combatAutomation(std::string_view command, bool& handled, std::string& error);
    void combatStates(std::vector<std::string>& out) const;
    [[nodiscard]] bool inSafeZone() const;

    // ---- Loot (M10, client/scenes/WorldLoot.cpp) ----
    void buildLootWindow();
    void requestLootAssets();
    // LOOT_* and COINS; false if not a loot opcode.
    bool handleLootMessage(const net::NetEvent& e);
    // ITEM_UPDATE was applied: pickup lines for the units gained.
    void notePickups(const game::Inventory& before);
    void updateLoot(float dt);
    void layoutLootWindow();
    // Click on a bag / E: opens it, walking into range first.
    void requestLootOpen(std::uint32_t lootId);
    bool sendLootOpen(std::uint32_t lootId);
    void requestLootTake(std::uint8_t what);
    // Hides the window; `sendClose` tells the server (Esc, walking away), `message` is shown if any.
    void closeLootWindow(bool sendClose, std::string_view message);
    // E: take everything from the open window, else open the nearest bag we may take from.
    void lootInteract(bool includeLocked);
    void showLootMessage(std::string_view text, bool error);
    void addPickup(std::string text, std::uint32_t rgb);
    void addLootDraw(const world::RemoteEntity& e);
    void drawLootHud(); // pickup lines
    [[nodiscard]] bool lootBudget() const;
    [[nodiscard]] world::LootLabel lootLabelOf(const world::RemoteEntity& e) const;
    [[nodiscard]] std::string lootSourceText(const world::RemoteEntity& e) const;
    [[nodiscard]] bool lootAutomation(std::string_view command, bool& handled, std::string& error);
    void lootStates(std::vector<std::string>& out) const;

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

    // Items (M8).
    world::ClientInventory m_inventory;
    game::VisibleEquipment m_summaryEquip; // CharSummary weapons, until INVENTORY arrives
    InventoryWindow* m_invWindow = nullptr;
    ui::Button* m_invButton = nullptr;
    bool m_invOpen = false;
    std::string m_lastItemMessage; // automation / log
    std::uint32_t m_itemMovesSent = 0;
    std::uint32_t m_itemMovesRefused = 0; // by the server
    std::size_t m_localWeaponsDrawn = 0;  // last frame

    // ---- Combat (M9) ----
    struct SelfCombat
    {
        std::uint16_t level = 1;
        std::uint32_t xp = 0;
        std::uint32_t xpToNext = 0;
        std::uint32_t hp = 1;
        std::uint32_t maxHp = 1;
        std::uint32_t mp = 0;
        std::uint32_t maxMp = 0;
        std::uint16_t attack = 0;
        std::uint16_t defense = 0;
        bool dead = false;
        std::uint8_t state = 0; // own ENTITY_STATE bits
        double diedAt = 0.0;
        bool respawnPending = false; // RESPAWN_REQ sent, no answer yet
        double respawnNotBefore = 0.0;
    };
    SelfCombat m_self;
    std::uint8_t m_charClass = 1;
    const game::SkillDef* m_skill = nullptr; // the class skill
    std::uint32_t m_target = 0;              // targeted entity (0 none; may be our own id)
    bool m_targetSent = true;                // TARGET_REQ for m_target went out
    bool m_autoAttack = false;               // ATTACK_REQ sent for m_target and not stopped
    bool m_chasing = false;                  // walking into range of m_target
    std::uint16_t m_queuedSkill = 0;         // cast once in range
    bool m_attackOnArrive = false;           // send ATTACK_REQ once in range (if not yet armed)
    float m_chaseReplan = 0.0f;
    world::SkillTimers m_skillTimers;
    world::FloatingNumbers m_numbers;
    std::string m_combatMessage;
    float m_combatMessageAge = 99.0f;
    float m_levelUpAge = 99.0f;
    std::uint16_t m_levelUpLevel = 0;
    float m_hitFlash = 0.0f; // red edge flash when the local player is hit
    std::uint32_t m_localSwings = 0;
    struct Bolt
    {
        std::uint32_t target = 0;
        world::Vec3 from{};
        world::Vec3 to{};
        float age = 0.0f;
    };
    std::vector<Bolt> m_bolts;
    std::uint32_t m_boltsLaunched = 0;
    std::array<double, 10> m_combatSends{}; // send times of the last combat requests (ring)
    std::size_t m_combatSendNext = 0;
    // Right click without dragging = attack (right drag still turns the camera).
    bool m_rightDown = false;
    int m_rightDownX = 0;
    int m_rightDownY = 0;
    double m_rightDownTime = 0.0;
    float m_dragTravel = 0.0f;
    // Automation counters.
    struct CombatSeen
    {
        std::uint32_t ownHits = 0;     // own damage events
        std::uint32_t ownSkillHits = 0;
        std::uint32_t hitsTaken = 0;
        std::uint32_t heals = 0;       // heals by us (any target)
        std::uint32_t healed = 0;      // heals received
        std::uint32_t otherHits = 0;   // damage events between two other entities
        std::uint32_t otherDeaths = 0; // deaths of entities we did not kill
        std::uint32_t remoteAttackClips = 0;
        std::uint32_t kills = 0;
        std::uint32_t levelUps = 0;
        std::uint32_t respawns = 0;
        std::uint32_t remoteHealsSeen = 0;
        std::uint32_t remoteDamageSeen = 0; // damage to remote players we watched
    };
    CombatSeen m_seen;
    std::string m_lastCombatFail;
    // Automation `hunt <name> <level>`: fight / rest loop until the level is reached.
    std::string m_huntName;
    std::uint16_t m_huntLevel = 0;
    double m_huntNextTry = 0.0;
    std::uint32_t m_lastAttacker = 0; // the last entity that damaged us
    double m_lastAttackedAt = -1e9;
    void updateHunt();
    // HUD widgets.
    ui::Widget* m_combatHud = nullptr; // custom-drawn HUD layer (bars, target frame, skill button)
    ui::Widget* m_skillButton = nullptr;
    ui::Label* m_combatHelp = nullptr; // second help line: the combat keys
    ui::Widget* m_deathLayer = nullptr;
    ui::Label* m_deathCountdown = nullptr;
    ui::Button* m_respawnButton = nullptr;
    ui::Label* m_levelLabel = nullptr; // "Aydor · Savaşçı · Seviye N" (refreshed on level-up)
    ui::Label* m_safeLabel = nullptr;
    friend class CombatHudWidget;

    // ---- Loot (M10) ----
    world::LootWindowModel m_loot;
    LootWindow* m_lootWindow = nullptr;
    std::uint32_t m_lootPendingOpen = 0; // LOOT_OPEN_REQ sent, no LOOT_OPEN_RES yet
    std::uint32_t m_lootWalk = 0;        // walking into range of this bag to open it
    float m_lootWalkReplan = 0.0f;
    std::uint32_t m_lootHover = 0;       // bag under the cursor
    std::uint32_t m_coins = 0;
    bool m_coinsKnown = false;
    std::string m_lastLootMessage;
    std::array<double, 10> m_lootSends{}; // send times of the last loot requests (ring)
    std::size_t m_lootSendNext = 0;
    struct Pickup
    {
        std::string text;
        std::uint32_t rgb = 0xFFFFFF;
        float age = 0.0f;
    };
    std::vector<Pickup> m_pickups;
    struct LootSeen
    {
        std::uint32_t opened = 0;  // LOOT_OPEN_RES Ok
        std::uint32_t takes = 0;   // LOOT_TAKE_RES (any result)
        std::uint32_t refused = 0; // LOOT_OPEN_RES / LOOT_TAKE_RES not Ok
        std::uint32_t pickups = 0; // item pickup lines
        std::uint32_t coinGains = 0;
        std::uint32_t closedByServer = 0;
    };
    LootSeen m_lootSeen;
    float m_lootLayoutTop = -1.0f; // logical top margin the window was last placed at

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
