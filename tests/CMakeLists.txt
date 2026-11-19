# vo_tests: Catch2 unit tests for shared code and the server library.
add_executable(vo_tests
    NoCrashDialogs.cpp
    core/ConfigTests.cpp
    core/ClockTests.cpp
    core/LogTests.cpp
    game/NamesTests.cpp
    game/ItemsTests.cpp
    game/CombatTests.cpp
    net/ConnectionTests.cpp
    net/CryptoTests.cpp
    net/FrameCodecTests.cpp
    net/MessagesTests.cpp
    net/PacketTests.cpp
    server/ServerTests.cpp
    server/ServerRobustnessTests.cpp
    server/ItemServerTests.cpp
    server/CombatServerTests.cpp
    server/StatsServerTests.cpp
    server/TestServer.h
)
target_link_libraries(vo_tests PRIVATE
    vo::build_options
    vo::shared
    vo_server_lib
    Catch2::Catch2WithMain
)

# ---- client core (M3): pure client logic from vo_client_core, no D3D ----
target_sources(vo_tests PRIVATE
    client/LayoutTests.cpp
    client/ScriptTests.cpp
    client/TextEditModelTests.cpp
    client/TextLayoutTests.cpp
    client/Utf8Tests.cpp
)
target_link_libraries(vo_tests PRIVATE vo::client_core)
# ---- end client core ----

# ---- client M4 (login / character screens): socket- and GPU-free logic from vo_client_core ----
target_sources(vo_tests PRIVATE
    client/CharacterSlotsTests.cpp
    client/NetTextTests.cpp
    client/RequestTrackerTests.cpp
    client/UserSettingsTests.cpp
    client/ValidationTests.cpp
)
# ---- end client M4 ----

# ---- client M6 (3D world): movement prediction, MOVE_REQ scheduling, interpolation, animation,
# camera and picking from vo_client_core (no GPU, no sockets) ----
target_sources(vo_tests PRIVATE
    client/WorldTestUtil.h
    client/MovementTests.cpp
    client/RemoteMotionTests.cpp
    client/WorldViewTests.cpp
    client/LooksCameraTests.cpp
)
# ---- end client M6 ----

# ---- client M8 (items): inventory model, attachments loader and maths, weapon placement ----
target_sources(vo_tests PRIVATE
    client/InventoryTests.cpp
    # M9 combat client: labels, picking, Tab, attack clips, cooldowns, numbers, parser, animator actions
    client/CombatViewTests.cpp
    # M10 loot client: label states, window model, pickups, tooltips, Esc order, parser
    client/LootViewTests.cpp
    # M11 Künye client: pending points, preview, texts, Miss numbers, tooltip attribute lines, parser
    client/StatsViewTests.cpp
    # M12 skills client: skill book, bar edits, learn gating, effects, texts, art plan, parser
    client/SkillsViewTests.cpp
    # M13 zones client: names, texts, veils, gate views, nearest gate, label fade, lore lines
    client/ZoneViewTests.cpp
    client/HudViewTests.cpp
    # M14 shop client: merchants in view, shop session, cooldown, quantity, entries, texts, Esc, parser, clips
    client/ShopViewTests.cpp
    # M16 upgrade client: the Yükselt section view, texts, session, (+N) names / colours, tooltip lines, parser
    client/UpgradeViewTests.cpp
    game/AttachmentsTests.cpp
)
# ---- end client M8 ----

# ---- assets (M5): formats, loaders, glTF conversion, terrain, assetc build ----
target_sources(vo_tests PRIVATE
    assets/AssetTestUtil.h
    assets/AssetFormatTests.cpp
    assets/GltfConversionTests.cpp
    assets/TerrainTests.cpp
    # PBR: .vmesh v2, .vtex, glTF materials / tangents (docs/ASSET_FORMATS.md)
    assets/GltfTestWriter.h
    assets/PbrFormatTests.cpp
    assets/GltfPbrTests.cpp
    assets/EnvFormatTests.cpp
    client/LightingMathTests.cpp
)
target_link_libraries(vo_tests PRIVATE vo::assetc_lib)
# ---- end assets ----

include(Catch)
catch_discover_tests(vo_tests
    DISCOVERY_MODE PRE_TEST
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES TIMEOUT 120
)

# ---- assets (M5): content checks on the real assets, built from assets_src by assetc ----
# `assets.build` (fixture) converts the committed sources into the build tree; the content tests
# run against that output.
set(VO_TEST_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_assets")
add_test(NAME assets.build
    COMMAND assetc build "${PROJECT_SOURCE_DIR}/assets_src" "${VO_TEST_ASSET_DIR}")
set_tests_properties(assets.build PROPERTIES FIXTURES_SETUP vo_assets_built TIMEOUT 600)
add_executable(vo_asset_content_tests NoCrashDialogs.cpp assets/content/ContentTests.cpp assets/content/EnvironmentTests.cpp
    assets/content/WeaponTests.cpp assets/content/MonsterTests.cpp assets/content/MonsterSpawnTests.cpp)
target_sources(vo_asset_content_tests PRIVATE assets/content/LootTests.cpp) # M10 loot bag + loot icons
target_sources(vo_asset_content_tests PRIVATE assets/content/ShopTests.cpp) # M14 merchants, stalls, potion icons, shop UI
target_link_libraries(vo_asset_content_tests PRIVATE
    vo::build_options
    vo::shared
    vo::assetc_lib
    tomlplusplus::tomlplusplus
    Catch2::Catch2WithMain
)
catch_discover_tests(vo_asset_content_tests
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "content."
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES
        TIMEOUT 300
        FIXTURES_REQUIRED vo_assets_built
        ENVIRONMENT "VO_TEST_ASSET_DIR=${VO_TEST_ASSET_DIR};VO_TEST_ASSET_SRC=${PROJECT_SOURCE_DIR}/assets_src"
)
# ---- end assets ----

# Smoke tests: executables start and exit cleanly.
add_test(NAME smoke.assetc COMMAND assetc)
add_test(NAME smoke.botclient COMMAND botclient --help)

# End-to-end tests: vo_server on an ephemeral 127.0.0.1 port with a temporary config and key pair
# (never the repo's config/ or port 15001), driven by botclient. The driver always stops the server
# (and the server also has a --run-seconds dead-man switch). smoke.vo_server is the old
# "--run-seconds 1" smoke test, now run against the temporary config.
find_program(VO_POWERSHELL NAMES powershell pwsh REQUIRED)
set(VO_IT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/net/integration/RunScenario.ps1")
foreach(scenario smoke nokeys genkeys ping login chars world badstate garbage wrongkey waitshutdown)
    if(scenario STREQUAL "smoke")
        set(test_name smoke.vo_server)
    else()
        set(test_name integration.${scenario})
    endif()
    add_test(NAME ${test_name}
        COMMAND "${VO_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${VO_IT_SCRIPT}"
            -ServerExe "$<TARGET_FILE:vo_server>"
            -BotExe "$<TARGET_FILE:botclient>"
            -Scenario ${scenario}
            -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/integration/${scenario}")
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 180)
endforeach()

# ---- M2 database: shared account-service suite (Memory + PostgreSQL), server/db tests ----
# PostgreSQL tests use vedatonline_test (VEDAT_TEST_DB_URL or db.test_connection_string in
# config/server.local.toml) and SKIP when none is configured; VEDAT_TEST_DB_URL=skip forces that.
target_sources(vo_tests PRIVATE
    server/AccountServiceTests.cpp
    db/TestDb.h
    db/TestDb.cpp
    db/ConnectionPoolTests.cpp
    db/MigrationTests.cpp
    db/PostgresAccountServiceTests.cpp
    db/WorldDbOutageTests.cpp
    db/PostgresItemTests.cpp
    db/PostgresCombatTests.cpp
)
# End-to-end against a vo_server backed by its own schema in vedatonline_test; exit 77 = skipped.
foreach(scenario login chars world restart)
    add_test(NAME integration.db.${scenario}
        COMMAND "${VO_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${VO_IT_SCRIPT}"
            -ServerExe "$<TARGET_FILE:vo_server>"
            -BotExe "$<TARGET_FILE:botclient>"
            -AdminExe "$<TARGET_FILE:vo_admin>"
            -Scenario ${scenario}
            -Database
            -RepoRoot "${PROJECT_SOURCE_DIR}"
            -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/integration/db_${scenario}")
    set_tests_properties(integration.db.${scenario} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77)
endforeach()
# ---- end M2 database ----

# ---- M6 server world: movement, AOI, relay, persistence (docs/SERVER_WORLD.md) ----
target_sources(vo_tests PRIVATE
    game/MovementTests.cpp
    server/world/FakeLink.h
    server/world/TestTerrain.h
    server/world/MoveValidatorTests.cpp
    server/world/WorldTests.cpp
    server/world/ZoneTests.cpp
    server/world/CombatZoneTests.cpp
    # M10 loot: shared rules, zone containers, the in-process server, PostgreSQL persistence
    game/LootTests.cpp
    server/world/LootZoneTests.cpp
    server/LootServerTests.cpp
    db/PostgresLootTests.cpp
    # M11 stats: PostgreSQL persistence (the shared rules, zone and server tests are in the files above)
    db/PostgresStatsTests.cpp
    # M12 skills: data + rules, balance simulation, zone mechanics, the in-process server, PostgreSQL
    game/SkillsTests.cpp
    # M13 zones: data, gate rules, v7 messages
    game/ZonesTests.cpp
    server/world/ZoneTestWorld.h
    server/world/ZoneChangeTests.cpp
    game/SkillBalanceTests.cpp
    server/world/SkillZoneTests.cpp
    server/SkillServerTests.cpp
    server/ZoneServerTests.cpp
    db/PostgresSkillsTests.cpp
    # M13 zones: home zones of new characters, move-character, a zone change stored before ZONE_CHANGE
    db/PostgresZonesTests.cpp
    # M14 shop: data + rules + v8 messages, zone NPCs / shops / consumables, the in-process server,
    # PostgreSQL (purchases, bar item slots, 0007)
    game/ShopTests.cpp
    server/world/ShopZoneTests.cpp
    server/ShopServerTests.cpp
    db/PostgresShopTests.cpp
    # M16 upgrades: tables, tiers, stones, planning, data, v10 messages, economy; the in-process server;
    # PostgreSQL (0009, the atomic attempt, set-upgrade, persistence across a relog)
    game/UpgradeTests.cpp
    server/UpgradeServerTests.cpp
    db/PostgresUpgradeTests.cpp
    # fix1: Akçe income by level against prices (DEVELOPMENT_PLAN.md §32; VO_ECONOMY_TABLE=1 prints it)
    game/EconomyTests.cpp
    # M15 levels 1-20: hunting slots (data, rules, zone), bosses, uniques, armour and the item pool
    game/HuntingTests.cpp
    server/world/SlotZoneTests.cpp
    # M17 quests: data + rules + v11 messages, the shipped line and its economy
    game/QuestTests.cpp
)
# The server needs zone 1's terrain now: every end-to-end test uses the assets built by the
# assets.build fixture (RunScenario.ps1 reads VO_TEST_ASSET_DIR into world.assets_dir and --terrain).
foreach(scenario speedhack wallhack floodmove leaverace dupworld aoi persist load noterrain items combat loot stats skills zones shop
                 zonesrace shoperrors hunt upgrade)
    add_test(NAME integration.${scenario}
        COMMAND "${VO_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${VO_IT_SCRIPT}"
            -ServerExe "$<TARGET_FILE:vo_server>"
            -BotExe "$<TARGET_FILE:botclient>"
            -Scenario ${scenario}
            -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/integration/${scenario}")
    set_tests_properties(integration.${scenario} PROPERTIES TIMEOUT 420)
endforeach()
# zonesrace walks to the gates of two zones a dozen times on two servers (~8 min alone, more in parallel)
set_tests_properties(integration.zonesrace PROPERTIES TIMEOUT 900)
foreach(scenario persist aoi items combat loot stats skills zones shop shoperrors hunt upgrade)
    add_test(NAME integration.db.${scenario}
        COMMAND "${VO_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${VO_IT_SCRIPT}"
            -ServerExe "$<TARGET_FILE:vo_server>"
            -BotExe "$<TARGET_FILE:botclient>"
            -AdminExe "$<TARGET_FILE:vo_admin>"
            -Scenario ${scenario}
            -Database
            -RepoRoot "${PROJECT_SOURCE_DIR}"
            -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/integration/db_${scenario}")
    set_tests_properties(integration.db.${scenario} PROPERTIES TIMEOUT 520 SKIP_RETURN_CODE 77)
endforeach()
set(vo_world_e2e_tests smoke.vo_server)
foreach(scenario nokeys genkeys ping login chars world badstate garbage wrongkey waitshutdown
                 speedhack wallhack floodmove leaverace dupworld aoi persist load noterrain items combat loot stats skills zones shop
                 zonesrace shoperrors hunt upgrade)
    list(APPEND vo_world_e2e_tests integration.${scenario})
endforeach()
foreach(scenario login chars world restart persist aoi items combat loot stats skills zones shop shoperrors hunt upgrade)
    list(APPEND vo_world_e2e_tests integration.db.${scenario})

endforeach()
set_tests_properties(${vo_world_e2e_tests} PROPERTIES
    FIXTURES_REQUIRED vo_assets_built
    ENVIRONMENT "VO_TEST_ASSET_DIR=${VO_TEST_ASSET_DIR}")
# ---- end M6 server world ----

# ---- M7: protocol fuzzing of an in-process server (tests/fuzz/ServerFuzzTests.cpp) ----
# ctest runs the short deterministic slice (VO_FUZZ_SCALE=1); a long run is
# `set VO_FUZZ_SCALE=20 & build\x64-release\bin\vo_fuzz_tests.exe` (docs/BUILDING.md).
add_executable(vo_fuzz_tests NoCrashDialogs.cpp fuzz/ServerFuzzTests.cpp server/TestServer.h)
target_link_libraries(vo_fuzz_tests PRIVATE
    vo::build_options
    vo::shared
    vo_server_lib
    Catch2::Catch2WithMain
    psapi
)
catch_discover_tests(vo_fuzz_tests
    DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "fuzz."
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES TIMEOUT 600 ENVIRONMENT "VO_FUZZ_SEED=1;VO_FUZZ_SCALE=1"
)
# ---- end M7 fuzzing ----