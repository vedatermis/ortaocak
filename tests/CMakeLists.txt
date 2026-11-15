# vo_tests: Catch2 unit tests for shared code and the server library.
add_executable(vo_tests
    core/ConfigTests.cpp
    core/ClockTests.cpp
    core/LogTests.cpp
    game/NamesTests.cpp
    net/CryptoTests.cpp
    net/FrameCodecTests.cpp
    net/MessagesTests.cpp
    net/PacketTests.cpp
    server/MemoryAccountServiceTests.cpp
    server/ServerTests.cpp
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

# ---- assets (M5): formats, loaders, glTF conversion, terrain, assetc build ----
target_sources(vo_tests PRIVATE
    assets/AssetTestUtil.h
    assets/AssetFormatTests.cpp
    assets/GltfConversionTests.cpp
    assets/TerrainTests.cpp
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
add_executable(vo_asset_content_tests assets/content/ContentTests.cpp)
target_link_libraries(vo_asset_content_tests PRIVATE
    vo::build_options
    vo::shared
    vo::assetc_lib
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
