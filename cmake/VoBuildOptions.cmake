# Global compile settings for all first-party targets.
# Link `vo::build_options` (PRIVATE) into every first-party target.
# Third-party include directories come from imported targets and are treated as
# SYSTEM (/external:I + /external:W0), so their warnings never trip /WX.

add_library(vo_build_options INTERFACE)
add_library(vo::build_options ALIAS vo_build_options)

target_compile_features(vo_build_options INTERFACE cxx_std_20)

target_compile_definitions(vo_build_options INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    ASIO_STANDALONE
    ASIO_NO_DEPRECATED
    $<$<CONFIG:Debug>:VO_DEBUG=1>
)

if(MSVC)
    target_compile_options(vo_build_options INTERFACE
        /W4
        /WX
        /permissive-
        /utf-8
        /MP
        /Zc:__cplusplus
        /Zc:preprocessor
        /external:W0
        /external:anglebrackets
        /EHsc
    )
    target_link_options(vo_build_options INTERFACE /WX)
endif()

# Make headers from imported (third-party) targets SYSTEM includes.
set(CMAKE_NO_SYSTEM_FROM_IMPORTED OFF)
