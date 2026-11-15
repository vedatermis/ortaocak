cmake_minimum_required(VERSION 3.25)

project(VedatOnline
    VERSION 0.1.0
    DESCRIPTION "Engine-less C++20 MMORPG"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "VedatOnline currently builds on Windows (MSVC) only.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Debug info embedded in .obj (/Z7): avoids PDB contention under parallel Ninja builds.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

# All executables land in one directory so they can find each other and config/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

option(VO_BUILD_TESTS "Build unit tests" ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(VoBuildOptions)

# ---- Third-party packages (vcpkg manifest: vcpkg.json) ----
find_package(asio CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
if(VO_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

# ---- Generated headers ----
set(VO_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file(shared/core/Version.h.in "${VO_GENERATED_DIR}/shared/core/Version.h" @ONLY)

# ---- Targets ----
add_subdirectory(shared)
add_subdirectory(server)
add_subdirectory(client)
add_subdirectory(tools)

if(VO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
