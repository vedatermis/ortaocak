# assetc: offline asset compiler (glTF -> .vmesh/.vanim, terrain description -> .vterrain, PNG copy).
# The conversion code lives in vo_assetc_lib so the unit tests can drive it in-process.

# cgltf and stb are header-only vcpkg packages without CMake configs.
find_path(VO_CGLTF_INCLUDE_DIR cgltf.h REQUIRED)
find_package(Stb REQUIRED)

add_library(vo_assetc_lib STATIC
    Build.h
    Build.cpp
    CgltfImpl.cpp
    GltfImport.h
    GltfImport.cpp
    Image.h
    Image.cpp
    Info.h
    Info.cpp
    StbImpl.cpp
    TerrainCompiler.h
    TerrainCompiler.cpp
)
add_library(vo::assetc_lib ALIAS vo_assetc_lib)
target_include_directories(vo_assetc_lib SYSTEM PRIVATE "${VO_CGLTF_INCLUDE_DIR}" "${Stb_INCLUDE_DIR}")
target_link_libraries(vo_assetc_lib
    PUBLIC vo::build_options vo::shared
    PRIVATE tomlplusplus::tomlplusplus)

add_executable(assetc main.cpp)
target_link_libraries(assetc PRIVATE vo::build_options vo::assetc_lib vo::shared)
