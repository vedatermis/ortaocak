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
// Story: the loading screen names the zone (gamedata/zones.toml, see "Zones" below) with one short lore
// line of that zone; on entering, a banner "<zone> — <tagline>" fades in and out; the HUD shows the name.
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
//
// Künye (M11, docs/PROTOCOL_STATS.md §7; client/scenes/WorldStats.cpp, KunyeWindow.h): CHAR_STATS
// feeds a world::StatSheet; C or the HUD button "Künye (C)" (world.kunye, a glowing number on it while
// points are unspent) toggles the KunyeWindow on the left; "+" / Shift+"+" / "−" make pending points
// with a live preview (game::deriveStats), "Onayla" sends one STAT_ALLOCATE_REQ (the first time per
// session after a "geri alınamaz" confirmation), "Vazgeç" drops them. Esc order: loot window, Künye,
// inventory, target, menu. A dodged monster attack (COMBAT_EVENT kind Miss) shows a grey "Iska" over
// the dodger and "Kaçındın!" when it is us. Künye `scene` commands: StatsView.h parseStatsCommand;
// states: WorldScene::statsStates().
//
// Skills (M12, docs/PROTOCOL_SKILLS.md §7; client/scenes/WorldSkills.cpp, SkillBar.h, SkillWindow.h):
// SKILL_STATE / SKILL_BAR / SKILL_*_RES feed a world::SkillBook, EFFECT_* a world::EffectBook,
// SKILL_COOLDOWNS the per-skill timers. The 8-slot bar (keys 1-8, click, drag to rearrange, right click
// or drag off to clear) sits at the bottom centre; K or the HUD button "Yetenekler (K)" (world.skills,
// a gold marker while points are unspent) toggles the "Yetenekler" window (two branches, "+" with a
// once-per-session confirmation, drag a learned skill onto the bar). Effects show as icons under the
// player's panel and the target frame, and in the world as effect meshes (SKILL_USED, ticks, auras
// while an effect lasts; SKILL_LEAP glides the leaper to its landing point). Esc order: loot window,
// Yetenekler, Künye, inventory, target, menu. Skill `scene` commands: SkillsView.h parseSkillsCommand;
// states: WorldScene::skillsStates().
//
// Zones (M13, docs/PROTOCOL_ZONES.md §7; client/scenes/WorldZones.cpp, ZoneView.h): names / taglines
// from gamedata/zones.toml; the zone banner (top centre, 5 s, on world entry and every zone change; a
// selected target fades it out); ZONE_CHANGE_BEGIN stops the character, drops target / loot / remotes
// and shows the gate screen ("<Bölge>", "Geçitten geçiliyor…") instead of the world; ZONE_CHANGE(Ok)
// builds a new scene ("<Bölge> yükleniyor…", banner, "<Bölge> bölgesine geçtin."); ZONE_CHANGE(Failed)
// keeps this scene, resumes in place and says "Geçit şu an kullanılamıyor, tekrar dene."; gates carry
// a label within 40 m, a hint within 8 m (open / closed to our race) and their veil effect mesh.
// States "gatescreen", "gatehint" / "nogatehint", "gatelabel", "nobanner"; `scene` commands: gates,
// expectgates, walktogate, facegate, expectgatelabel / expectnogatelabel, expectgatehint /
// expectnogatehint, expectzonemessage, expectveils, expectzoneseen, zonehold, zonefail
// (WorldScene::zonesAutomation).
#pragma once

#include "client/net/GameClient.h"
#include "client/scenes/HudBar.h"
#include "client/scenes/SkillBar.h"
#include "client/scenes/UiScene.h"
#include "client/world/Animator.h"
#include "client/world/CameraObstacles.h"
#include "client/world/ClientInventory.h"
#include "client/world/CombatView.h"
#include "client/world/DebugGhosts.h"
#include "client/world/LootView.h"
#include "client/world/OrbitCamera.h"
#include "client/world/StatsView.h"
#include "client/world/TerrainQuery.h"
#include "client/world/WorldModel.h"
#include "client/world/ZoneView.h"
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
class KunyeWindow;
class LootWindow;
class SkillWindow;

class WorldScene final : public UiScene, private SkillUiHost
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
    // ---- HUD (docs/CLIENT_RENDERING.md §15; client/world/HudView.h, client/scenes/HudBar.h) ----
    void buildHudBar();  // the framed bottom bar: action buttons, (the skill slots), menu buttons
    void layoutHud();    // per frame: bar size for the screen, windows placed clear of the HUD
    void setControlsOpen(bool open); // "Tuşlar"
    [[nodiscard]] world::HudButtonState hudButtonState(world::HudButton b) const;
    void drawPlayerFrame(ui::DrawContext& ctx, const Rect& r);
    [[nodiscard]] bool lootReachable() const; // a bag we may take from is within reach (Ganimet lit)
    // Physical-pixel layout of the transient bottom lines (cast bar, messages, gate hint, pickups).
    [[nodiscard]] world::HudBottomLines bottomLines() const;
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
    void clickToMove(int x, int y, bool follow = false);
    void updateHoldMove(float dt); // left button held on the ground: keep walking towards the cursor
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
    // The class (free) skill, as before M12 (the hunt loop; walks into range first if needed).
    void useSkill();
    // SKILL_REQ with the target of §7.1 (enemy: the target; friendly: a selected player or 0; self: 0).
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
    // Automation `lootfarm`: one looting step of the hunt loop (true: busy looting, skip the rest).
    bool farmLootStep();

    // ---- Künye (M11, client/scenes/WorldStats.cpp) ----
    void buildKunye(); // the window (left side) and the HUD button with its unspent-points marker
    void setKunyeOpen(bool open);
    // CHAR_STATS and STAT_ALLOCATE_RES; false if not a stats opcode.
    bool handleStatsMessage(const net::NetEvent& e);
    // Texts and derived rows of the window from the sheet, level, experience and equipment.
    void refreshKunye();
    void kunyeAdd(std::size_t attribute, bool shift);
    void kunyeRemove(std::size_t attribute);
    void kunyeConfirm(); // "Onayla" (asks once per session first)
    void kunyeCancel();  // "Vazgeç"
    void sendStatAllocate();
    void showStatMessage(std::string_view text, bool error);
    // The inventory's rules (class, level, base attributes: requirements, tooltips).
    void refreshItemRules();
    void updateStats(float dt);
    [[nodiscard]] game::EquipBonus localEquipBonus() const;
    [[nodiscard]] bool statsAutomation(std::string_view command, bool& handled, std::string& error);
    void statsStates(std::vector<std::string>& out) const;

    // ---- Skills (M12, client/scenes/WorldSkills.cpp) ----
    void buildSkills();      // the bar, the HUD button, the effect rows, the probes (under the windows)
    void buildSkillWindow(); // "Yetenekler" (left, above the Künye)
    void requestSkillAssets();
    void setSkillsOpen(bool open);
    // SKILL_*, EFFECT_*; peeks at ENTITY_SPAWN / DESPAWN / DEATH (effects are dropped) and returns
    // false for those. False if not a skill opcode.
    bool handleSkillMessage(const net::NetEvent& e);
    // Uses a skill by id with the bar's rules (§7.1): target, range walk, local checks.
    void useSkillById(std::uint16_t skillId);
    void sendSkillLearn(std::uint16_t skillId);
    void sendSkillBar(const game::SkillBar& bar);
    void showSkillMessage(std::string_view text, bool error);
    void updateSkills(float dt);
    void addSkillDraws();    // effect meshes (SKILL_USED art, ticks, leap trails, auras of effects)
    void drawSkillOverlay(); // tooltips of slots / cells / effect icons / the button, the drag ghost
    void spawnSkillArt(const vo::net::SkillUsed& m);
    [[nodiscard]] world::SkillSelection skillSelection() const;
    // Where the target frame is drawn (physical pixels).
    [[nodiscard]] Rect targetFrameRect() const;
    // Feet of an entity (the local player included); false if it is not known.
    bool entityFeet(std::uint32_t id, world::Vec3& out) const;
    [[nodiscard]] float entityHeight(std::uint32_t id) const;
    // The local character's drawn position (a leap glides it, M12).
    [[nodiscard]] world::Vec3 localDrawPosition() const;
    [[nodiscard]] bool skillsAutomation(std::string_view command, bool& handled, std::string& error);
    void skillsStates(std::vector<std::string>& out) const;
    void updatePull();

    // ---- Zones (M13, client/scenes/WorldZones.cpp, docs/PROTOCOL_ZONES.md §7) ----
    // ZONE_CHANGE_BEGIN / ZONE_CHANGE / ZONE_GATE_REFUSED; false if not a zone opcode.
    bool handleZoneMessage(const net::NetEvent& e);
    // ZONE_CHANGE_BEGIN: stop, drop targets / loot / remotes, show the gate screen.
    void beginZoneChange(std::uint16_t toZone);
    // ZONE_CHANGE(Failed) in a ready scene of the same zone: back where we stood, resume here.
    void resumeAfterFailedChange();
    void endZoneChangeScreen();
    void buildGateViews();   // becomeReady: the gates of this zone, their props and veils
    void addZoneDraws();     // gate veils
    void drawGateOverlay(const world::Mat4& vp); // gate labels (40 m) and the near-gate hint (8 m)
    void showZoneMessage(std::string text); // a system line in the combat message area
    void updateZones(float dt);
    [[nodiscard]] bool zonesAutomation(std::string_view command, bool& handled, std::string& error);
    void zonesStates(std::vector<std::string>& out) const;
    [[nodiscard]] bool holdNetEvents() const override;
    // Automation `zonehold`: the loading screen of a zone change stays up for a screenshot.
    [[nodiscard]] bool loadingHeld() const;
    // SkillUiHost
    [[nodiscard]] const world::SkillBook& skillBook() const override { return m_skillBook; }
    [[nodiscard]] SlotView slotView(std::size_t slot) const override;
    [[nodiscard]] const render::Texture2D* skillImage(std::string_view path) const override;
    [[nodiscard]] std::span<const world::ShownEffect> shownEffects(bool target) const override;
    [[nodiscard]] double skillClock() const override { return m_time; }
    void useBarSlot(std::size_t slot) override;
    void clearBarSlot(std::size_t slot) override;
    void beginSkillDrag(std::uint16_t skillId, int fromSlot) override;
    void moveSkillDrag(float x, float y) override;
    void endSkillDrag(float x, float y, bool drop) override;
    [[nodiscard]] bool skillDragActive() const override { return m_skillDrag.active; }
    [[nodiscard]] std::uint16_t skillDragSkill() const override { return m_skillDrag.skill; }
    void raiseSkill(std::uint16_t skillId) override;
    void closeSkillWindow() override { setSkillsOpen(false); }

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
    bool m_zoneChanging = false; // M13: ZONE_CHANGE_BEGIN received, ZONE_CHANGE not yet
    bool m_menuOpen = false;
    bool m_showInfo = false; // F3 (off for players; flows turn it on with `key f3`)
    float m_hudTimer = 0.0f;
    ui::Panel* m_infoPanel = nullptr;
    ui::Label* m_position = nullptr;
    ui::Label* m_rtt = nullptr;
    ui::Label* m_fps = nullptr;
    ui::Label* m_stats = nullptr;
    ui::Widget* m_menuLayer = nullptr;
    ui::Widget* m_hudPanel = nullptr; // the player frame (HudFrameWidget)
    ui::Label* m_zoneLabel = nullptr;
    std::array<char, 160> m_textBuffer{};
    // HUD bar (bottom centre) and the "Tuşlar" window.
    HudBarFrame* m_hudBar = nullptr;
    std::array<HudIconButton*, world::kHudActionCount> m_actionButtons{};
    HudIconButton* m_controlsButton = nullptr;
    HudIconButton* m_menuButton = nullptr;
    ControlsWindow* m_controlsWindow = nullptr;
    bool m_controlsOpen = false;
    ui::Label* m_hudTipProbe = nullptr; // hud.tooltip: the hovered HUD button's tooltip text
    world::HudBarLayout m_barLayout;    // logical, last applied
    float m_barLayoutW = -1.0f;         // logical screen size it was computed for
    float m_barLayoutH = -1.0f;
    // HUD pieces an open window covers this frame (hidden until it closes).
    struct HudYield
    {
        bool bar = false;
        bool player = false;
        bool target = false;
        bool zone = false;
    };
    HudYield m_yield;
    std::vector<Rect> m_windowCovers; // physical rects of the open windows (last layout)

    // Items (M8).
    world::ClientInventory m_inventory;
    game::VisibleEquipment m_summaryEquip; // CharSummary weapons, until INVENTORY arrives
    InventoryWindow* m_invWindow = nullptr;
    HudIconButton* m_invButton = nullptr;
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
    // Automation `levelupfull` (UI flows): within 2 s of the last LEVEL_UP the client saw a full hp
    // bar (the level-up SELF_STATS, or the hp a hit found if another monster struck first).
    bool m_levelUpWatch = false;
    bool m_levelUpSawFull = false;
    float m_hitFlash = 0.0f; // red edge flash when the local player is hit
    std::uint32_t m_localSwings = 0;
    struct Bolt
    {
        std::uint32_t target = 0;
        world::Vec3 from{};
        world::Vec3 to{};
        float age = 0.0f;
        // M12: launched by SKILL_USED; Köz Oku's ember bolt or Kırağı Oku's frost bolt.
        std::string_view mesh = "ember_bolt";
        float duration = 0.25f;
        float scale = 1.6f;
    };
    std::vector<Bolt> m_bolts;
    std::uint32_t m_boltsLaunched = 0;
    std::array<double, 10> m_combatSends{}; // send times of the last combat requests (ring)
    std::size_t m_combatSendNext = 0;
    // Right click without dragging = attack (right drag still turns the camera).
    bool m_rightDown = false;
    bool m_holdMove = false;     // left button went down on the ground and is still held
    float m_holdMoveTimer = 0.0f; // seconds until the destination follows the cursor again
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
    ui::Widget* m_combatHud = nullptr; // custom-drawn HUD layer (target frame, cast bar, messages)
    ui::Widget* m_deathLayer = nullptr;
    ui::Label* m_deathCountdown = nullptr;
    ui::Button* m_respawnButton = nullptr;
    ui::Label* m_levelLabel = nullptr; // "Aydor · Savaşçı" under the name (the level is in the medallion)
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
    // Automation `lootfarm <name> <template> <pickups>`: the hunt loop plus looting.
    std::uint16_t m_farmTemplate = 0;
    std::uint32_t m_farmWant = 0;
    std::uint32_t m_farmSeen = 0;
    bool m_farmDone = false; // state farmdone
    bool m_farming = false;
    std::uint32_t m_farmRows = 0; // lootfarm ... rows <n>: stop at a bag with >= n rows (0: pickup mode)
    std::vector<std::uint32_t> m_farmSkip; // bags whose take was refused (not tried again)

    // ---- Künye (M11) ----
    world::StatSheet m_sheet;
    KunyeWindow* m_kunyeWindow = nullptr;
    HudIconButton* m_kunyeButton = nullptr;
    bool m_kunyeOpen = false;
    std::string m_lastStatMessage;
    std::uint32_t m_levelUpPoints = 0; // stat points of the last level-up (toast line)
    struct StatsSeen
    {
        std::uint32_t charStats = 0; // CHAR_STATS received
        std::uint32_t accepted = 0;  // STAT_ALLOCATE_RES Ok
        std::uint32_t refused = 0;   // STAT_ALLOCATE_RES not Ok
        std::uint32_t dodges = 0;    // monster attacks we dodged (COMBAT_EVENT Miss on us)
        std::uint32_t missesSeen = 0; // Miss events on other players
    };
    StatsSeen m_statsSeen;

    // ---- Skills (M12) ----
    world::SkillBook m_skillBook;
    world::EffectBook m_effects;
    SkillWindow* m_skillWindow = nullptr;
    HudIconButton* m_skillsButton = nullptr;
    SkillSlotsWidget* m_skillBar = nullptr;
    EffectRowWidget* m_selfEffects = nullptr;
    EffectRowWidget* m_targetEffects = nullptr;
    ui::Label* m_skillTipProbe = nullptr;  // skills.tooltip: the hovered skill's tooltip text
    ui::Label* m_effectTipProbe = nullptr; // effects.tooltip: the hovered effect icon's tooltip text
    bool m_skillsOpen = false;
    std::string m_lastSkillMessage;
    std::uint32_t m_levelUpSkillPoints = 0; // skill points of the last level-up (toast line)
    float m_localLabelTop = -1.0f;          // screen y of the own name label's top (last frame; < 0: none)
    float m_levelUpToastBottom = -1.0f;     // screen y under the level-up toast's last line (< 0: none)
    struct SkillDrag
    {
        bool active = false;
        std::uint16_t skill = 0;
        int fromSlot = -1; // -1: from the window
        float x = 0.0f;
        float y = 0.0f;
    };
    SkillDrag m_skillDrag;
    // Effect art instances (docs/SKILL_ART.md): SKILL_USED, periodic ticks, leap trails.
    struct SkillArt
    {
        std::string mesh; // effects/<name>.vmesh
        world::ArtMotion motion = world::ArtMotion::Burst;
        std::uint32_t entity = 0; // followed (feet / chest), 0: the fixed point `at`
        world::Vec3 at{};
        world::Vec3 to{};
        float age = 0.0f;
        float delay = 0.0f; // Fall: staggered motes
        float duration = 1.0f;
        float scale = 1.0f;
        float opacity = 1.0f;
        float seed = 0.0f;
        std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
        bool tintEmissive = false;
    };
    std::vector<SkillArt> m_art;
    std::vector<std::pair<std::string, std::uint32_t>> m_artCounts; // mesh name -> drawn last frame
    // SKILL_LEAP: the leaper glides to its landing point (0.25 s).
    struct Leap
    {
        std::uint32_t entity = 0;
        world::Vec3 from{};
        world::Vec3 to{};
        float age = 0.0f;
    };
    std::vector<Leap> m_leaps;
    std::uint32_t m_artSeed = 1;
    struct SkillsSeen
    {
        std::uint32_t states = 0;      // SKILL_STATE
        std::uint32_t learned = 0;     // SKILL_LEARN_RES Ok
        std::uint32_t learnRefused = 0;
        std::uint32_t barSets = 0;     // SKILL_BAR_SET_RES Ok
        std::uint32_t barRefused = 0;
        std::uint32_t ownUses = 0;     // own SKILL_USED
        std::uint32_t otherUses = 0;   // SKILL_USED of others
        std::uint32_t selfEffects = 0; // EFFECT_APPLY on us
        std::uint32_t effectsApplied = 0;
        std::uint32_t ticks = 0;       // periodic COMBAT_EVENTs seen
        std::uint32_t absorbed = 0;    // absorbed COMBAT_EVENTs on us
        std::uint32_t leaps = 0;
        std::uint32_t cooldownLists = 0; // SKILL_COOLDOWNS
    };
    SkillsSeen m_skillsSeen;
    std::uint16_t m_lastUsedSkill = 0; // own SKILL_USED: how many targets that use hit
    std::uint32_t m_lastUseHits = 0;
    double m_lastUseTime = -1e9;
    // SKILL_LEARN_REQ + SKILL_BAR_SET_REQ share the server's 10/s limit (more: ERROR Flood and the
    // connection is closed): at most 8 a second; a click over that waits here and goes out later.
    std::array<double, 8> m_skillSends{};
    std::size_t m_skillSendNext = 0;
    std::uint16_t m_deferredLearn = 0;
    std::optional<game::SkillBar> m_deferredBar;
    [[nodiscard]] bool skillRequestBudget() const;
    void noteSkillRequest();
    // Automation `pullpack`: walk near idle monsters until enough chase us.
    std::string m_pullName;
    std::uint32_t m_pullWant = 0;
    bool m_pulling = false;
    bool m_pullDone = false;
    double m_pullNext = 0.0;

    // ---- Zones (M13) ----
    std::uint16_t m_zoneId = 0;                // the zone this scene shows
    std::vector<world::GateView> m_gates;      // its gates (labels, hints, veils)
    std::uint16_t m_changeTo = 0;              // ZONE_CHANGE_BEGIN's target while m_zoneChanging
    float m_changeTime = 0.0f;                 // seconds on the gate screen
    bool m_simulatedChange = false;            // automation `zonefail`: a local simulation (no ZONE_LOADED_REQ)
    std::string m_gateHint;                    // the near-gate hint shown (last frame), "" none
    std::vector<std::string> m_gateLabelsShown; // gate labels drawn (last frame)
    std::uint32_t m_veilsDrawn = 0;            // gate veils drawn (last frame)
    std::vector<const char*> m_gateLabelState; // per gate: why its label was (not) drawn (automation)
    std::string m_lastZoneMessage;             // arrival / refusal / failure line (automation)
    struct ZonesSeen
    {
        std::uint32_t begins = 0;   // ZONE_CHANGE_BEGIN
        std::uint32_t failed = 0;   // ZONE_CHANGE(Failed)
        std::uint32_t refusals = 0; // ZONE_GATE_REFUSED
    };
    ZonesSeen m_zonesSeen;

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
