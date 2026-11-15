# assetc: offline asset compiler (glTF/PNG -> .vmesh/.vanim/.vtex). Skeleton only (M5).
add_executable(assetc main.cpp)
target_link_libraries(assetc PRIVATE vo::build_options vo::shared)
