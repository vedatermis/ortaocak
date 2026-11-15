# vo_shared: static library shared by client, server, tools and tests.
add_library(vo_shared STATIC
    core/Assert.h
    core/Assert.cpp
    core/Clock.h
    core/Config.h
    core/Config.cpp
    core/Log.h
    core/Log.cpp
    core/Paths.h
    core/Paths.cpp
    net/Net.h
    game/Game.h
)
add_library(vo::shared ALIAS vo_shared)

# Includes are rooted at the repository: #include "shared/core/Log.h"
target_include_directories(vo_shared PUBLIC
    "${PROJECT_SOURCE_DIR}"
    "${VO_GENERATED_DIR}"
)

target_link_libraries(vo_shared
    PUBLIC
        vo::build_options
    PRIVATE
        tomlplusplus::tomlplusplus
)
