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

# ---- client M4 (login / character screens): socket- and GPU-free logic from vo_client_core ----
target_sources(vo_tests PRIVATE
    client/CharacterSlotsTests.cpp
    client/NetTextTests.cpp
    client/RequestTrackerTests.cpp
    client/UserSettingsTests.cpp
    client/ValidationTests.cpp
)
# ---- end client M4 ----

include(Catch)
catch_discover_tests(vo_tests
    DISCOVERY_MODE PRE_TEST
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    PROPERTIES TIMEOUT 120
)

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
