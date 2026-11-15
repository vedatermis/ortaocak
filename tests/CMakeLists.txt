# vo_tests: Catch2 unit tests for shared code.
add_executable(vo_tests
    core/ConfigTests.cpp
    core/ClockTests.cpp
    core/LogTests.cpp
)
target_link_libraries(vo_tests PRIVATE
    vo::build_options
    vo::shared
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

include(Catch)
catch_discover_tests(vo_tests
    DISCOVERY_MODE PRE_TEST
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)

# Smoke tests: executables start and exit cleanly.
add_test(NAME smoke.vo_server COMMAND vo_server --run-seconds 1 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME smoke.assetc COMMAND assetc)
add_test(NAME smoke.botclient COMMAND botclient)
set_tests_properties(smoke.vo_server PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "shutdown complete")
