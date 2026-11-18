# ---- vo_client_core: platform-neutral client logic (no D3D/Win32), shared with vo_tests ----
add_library(vo_client_core STATIC
    core/Geometry.h
    core/Utf8.h
    core/Utf8.cpp
    ui/Layout.h
    ui/Layout.cpp
    ui/TextEditModel.h
    ui/TextEditModel.cpp
    ui/TextLayout.h
    ui/TextLayout.cpp
    platform/InputEvent.h
    platform/KeyNames.h
    platform/KeyNames.cpp
    automation/Script.h
    automation/Script.cpp
    net/AccountState.h
    net/NetText.h
    net/NetText.cpp
    net/RequestTracker.h
    net/RequestTracker.cpp
    net/GameClient.h
    net/GameClient.cpp
    scenes/CharacterSlots.h
    scenes/CharacterSlots.cpp
    scenes/UiScale.h
    scenes/UserSettings.h
    scenes/UserSettings.cpp
    scenes/Validation.h
    scenes/Validation.cpp
    # M6 world logic (movement prediction, interpolation, animation, camera; no D3D)
    world/WorldMath.h
    world/WorldMath.cpp
    world/TerrainQuery.h
    world/TerrainQuery.cpp
    world/LocalMovement.h
    world/LocalMovement.cpp
    world/RemoteMotion.h
    world/RemoteMotion.cpp
    world/CharacterAssets.h
    world/CharacterAssets.cpp
    world/Animator.h
    world/Animator.cpp
    world/CameraObstacles.h
    world/CameraObstacles.cpp
    world/OrbitCamera.h
    world/OrbitCamera.cpp
    world/WorldModel.h
    world/WorldModel.cpp
    world/DebugGhosts.h
    world/DebugGhosts.cpp
    # M8 items: inventory model, weapon placement, item game data
    world/ClientInventory.h
    world/ClientInventory.cpp
    world/WeaponPose.h
    world/WeaponPose.cpp
    world/ItemData.h
    world/ItemData.cpp
    # M9 combat presentation logic (targeting, cooldowns, damage numbers, labels)
    world/CombatView.h
    world/CombatView.cpp
    # M10 loot presentation logic (labels, loot window model, pickup texts, Esc order, commands)
    world/LootView.h
    world/LootView.cpp
    # M11 character sheet logic (pending points, preview, texts, commands)
    world/StatsView.h
    world/StatsView.cpp
    # M12 skill logic (ranks, bar, requests, effects, texts, effect art plan, commands)
    world/SkillsView.h
    world/SkillsView.cpp
    # M13 zone presentation logic (names, texts, gate views, lore lines)
    world/ZoneView.h
    world/ZoneView.cpp
    # HUD logic (action bar layout, button tooltips, controls reference, window placement)
    world/HudView.h
    world/HudView.cpp
    # M14 merchants and potions (NPCs in view, shop session, cooldown, texts, commands)
    world/ShopView.h
    world/ShopView.cpp
)
add_library(vo::client_core ALIAS vo_client_core)
target_link_libraries(vo_client_core PUBLIC vo::build_options vo::shared)

# ---- HLSL shaders: compiled at build time by fxc into bytecode headers ----
# Output: <build>/generated/client/shaders/<Name>_<vs|ps>.h with `const BYTE g_<Name><VS|PS>[]`,
# included as "client/shaders/<Name>_vs.h". fxc runs with /WX: a shader warning fails the build.
find_program(VO_FXC fxc
    HINTS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "$ENV{WindowsSdkBinPath}/$ENV{WindowsSDKVersion}/x64"
    DOC "Windows SDK effect compiler (fxc.exe)")
if(NOT VO_FXC)
    message(FATAL_ERROR "fxc.exe (Windows SDK) not found; run from a VS developer environment (build.ps1 does).")
endif()

set(VO_SHADER_OUT_DIR "${VO_GENERATED_DIR}/client/shaders")
set(VO_SHADER_HEADERS)
# Shared include of the 3D shaders (#include "World.hlsli"): every shader is rebuilt when it changes.
set(VO_SHADER_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/shaders/World.hlsli")

# vo_compile_shader(<hlsl> <name> <entry> <profile> <suffix>)
function(vo_compile_shader source name entry profile suffix)
    string(TOUPPER "${suffix}" suffixUpper)
    set(out "${VO_SHADER_OUT_DIR}/${name}_${suffix}.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${VO_SHADER_OUT_DIR}"
        COMMAND "${VO_FXC}" /nologo /WX /O3 /T ${profile} /E ${entry} /Vn g_${name}${suffixUpper}
                /I "${CMAKE_CURRENT_SOURCE_DIR}/shaders" /Fh "${out}" "${source}"
        DEPENDS "${source}" "${VO_SHADER_COMMON}"
        COMMENT "fxc ${profile} ${name}:${entry}"
        VERBATIM)
    set(VO_SHADER_HEADERS ${VO_SHADER_HEADERS} "${out}" PARENT_SCOPE)
endfunction()

set(VO_SPRITE_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Sprite.hlsl")
vo_compile_shader("${VO_SPRITE_HLSL}" Sprite VSMain vs_5_0 vs)
vo_compile_shader("${VO_SPRITE_HLSL}" Sprite PSMain ps_5_0 ps)

# ---- M6 3D shaders (docs/CLIENT_RENDERING.md) ----
set(VO_MESH_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Mesh.hlsl")
vo_compile_shader("${VO_MESH_HLSL}" MeshStatic VSStatic vs_5_0 vs)
vo_compile_shader("${VO_MESH_HLSL}" MeshSkinned VSSkinned vs_5_0 vs)
vo_compile_shader("${VO_MESH_HLSL}" Mesh PSMain ps_5_0 ps)
vo_compile_shader("${VO_MESH_HLSL}" MeshStaticShadow VSStaticShadow vs_5_0 vs)
vo_compile_shader("${VO_MESH_HLSL}" MeshSkinnedShadow VSSkinnedShadow vs_5_0 vs)
set(VO_TERRAIN_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Terrain.hlsl")
vo_compile_shader("${VO_TERRAIN_HLSL}" Terrain VSMain vs_5_0 vs)
vo_compile_shader("${VO_TERRAIN_HLSL}" Terrain PSMain ps_5_0 ps)
vo_compile_shader("${VO_TERRAIN_HLSL}" TerrainShadow VSShadow vs_5_0 vs)
set(VO_SKY_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Sky.hlsl")
vo_compile_shader("${VO_SKY_HLSL}" Sky VSMain vs_5_0 vs)
vo_compile_shader("${VO_SKY_HLSL}" Sky PSMain ps_5_0 ps)
set(VO_DECAL_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Decal.hlsl")
vo_compile_shader("${VO_DECAL_HLSL}" Decal VSMain vs_5_0 vs)
vo_compile_shader("${VO_DECAL_HLSL}" Decal PSMain ps_5_0 ps)
set(VO_POST_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/shaders/PostProcess.hlsl")
vo_compile_shader("${VO_POST_HLSL}" PostProcess VSMain vs_5_0 vs)
vo_compile_shader("${VO_POST_HLSL}" PostResolveMs PSResolveMs ps_5_0 ps)
vo_compile_shader("${VO_POST_HLSL}" PostResolve PSResolve ps_5_0 ps)
set(VO_3D_SHADER_SOURCES shaders/World.hlsli shaders/Mesh.hlsl shaders/Terrain.hlsl shaders/Sky.hlsl
    shaders/Decal.hlsl shaders/PostProcess.hlsl)

# ---- stb (stb_truetype, stb_image, stb_rect_pack): header-only, from vcpkg ----
find_package(Stb REQUIRED)

# ---- vo_client: Win32 + Direct3D 11 game client (WIN32 subsystem) ----
# Automation scripts (listed so IDEs show them; run by tools/dev/run-ui-flows.ps1).
file(GLOB VO_CLIENT_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/automation/flows/*.voscript")
add_executable(vo_client WIN32
    main.cpp
    automation/ScriptPlayer.h
    automation/ScriptPlayer.cpp
    automation/uidemo.voscript
    ${VO_CLIENT_SCRIPTS}
    platform/Clipboard.h
    platform/Clipboard.cpp
    platform/FrameTimer.h
    platform/Input.h
    platform/Input.cpp
    platform/Win32Util.h
    platform/Win32Util.cpp
    platform/Window.h
    platform/Window.cpp
    render/Font.h
    render/Font.cpp
    render/RenderDevice.h
    render/RenderDevice.cpp
    render/RenderStates.h
    render/RenderStates.cpp
    render/Screenshot.h
    render/Screenshot.cpp
    render/Shader.h
    render/Shader.cpp
    render/SpriteBatch.h
    render/SpriteBatch.cpp
    render/StbImpl.cpp
    render/TextRenderer.h
    render/TextRenderer.cpp
    render/Texture2D.h
    render/Texture2D.cpp
    render/GpuMesh.h
    render/GpuMesh.cpp
    render/AssetCache.h
    render/AssetCache.cpp
    render/WorldRenderer.h
    render/WorldRenderer.cpp
    render/RenderTarget.h
    render/RenderTarget.cpp
    render/PostProcess.h
    render/PostProcess.cpp
    render/LightingMath.h
    render/DebugModelViewer.h
    render/DebugModelViewer.cpp
    shaders/Sprite.hlsl
    ${VO_3D_SHADER_SOURCES}
    ${VO_SHADER_HEADERS}
    ui/Ui.h
    ui/Button.h
    ui/Button.cpp
    ui/Checkbox.h
    ui/Checkbox.cpp
    ui/ClipboardProvider.h
    ui/DrawContext.h
    ui/ImageWidget.h
    ui/ImageWidget.cpp
    ui/Label.h
    ui/Label.cpp
    ui/ListBox.h
    ui/ListBox.cpp
    ui/MessageBox.h
    ui/MessageBox.cpp
    ui/Panel.h
    ui/Panel.cpp
    ui/StackPanel.h
    ui/StackPanel.cpp
    ui/TextBox.h
    ui/TextBox.cpp
    ui/Theme.h
    ui/UiContext.h
    ui/UiContext.cpp
    ui/Widget.h
    ui/Widget.cpp
    scenes/Scenes.h
    scenes/Scene.h
    scenes/SceneManager.h
    scenes/SceneManager.cpp
    scenes/SceneResources.h
    scenes/SceneResources.cpp
    scenes/Backdrop.h
    scenes/Backdrop.cpp
    scenes/UiScene.h
    scenes/UiScene.cpp
    scenes/SceneWidgets.h
    scenes/SceneWidgets.cpp
    scenes/CharacterPreview.h
    scenes/CharacterPreview.cpp
    scenes/TypedConfirmDialog.h
    scenes/TypedConfirmDialog.cpp
    scenes/LoginScene.h
    scenes/LoginScene.cpp
    scenes/CharSelectScene.h
    scenes/CharSelectScene.cpp
    scenes/CharCreateScene.h
    scenes/CharCreateScene.cpp
    scenes/WorldScene.h
    scenes/WorldScene.cpp
    scenes/WorldCombat.cpp
    scenes/InventoryWindow.h
    scenes/InventoryWindow.cpp
    scenes/ItemTooltip.h
    scenes/ItemTooltip.cpp
    scenes/LootWindow.h
    scenes/LootWindow.cpp
    scenes/WorldLoot.cpp
    scenes/ShopWindow.h
    scenes/ShopWindow.cpp
    scenes/WorldShop.cpp
    scenes/WorldWindows.cpp
    scenes/KunyeWindow.h
    scenes/KunyeWindow.cpp
    scenes/WorldStats.cpp
    scenes/SkillBar.h
    scenes/SkillBar.cpp
    scenes/HudBar.h
    scenes/HudBar.cpp
    scenes/SkillWindow.h
    scenes/SkillWindow.cpp
    scenes/WorldSkills.cpp
    scenes/WorldZones.cpp
    scenes/UiDemoScene.h
    scenes/UiDemoScene.cpp
    net/ClientNet.h
)
set_source_files_properties(shaders/Sprite.hlsl ${VO_3D_SHADER_SOURCES} automation/uidemo.voscript ${VO_CLIENT_SCRIPTS}
    PROPERTIES HEADER_FILE_ONLY ON)

target_include_directories(vo_client SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_link_libraries(vo_client PRIVATE
    vo::build_options
    vo::client_core
    vo::shared
    d3d11
    dxgi
    dxguid
    windowscodecs
    ole32
    shell32
)
