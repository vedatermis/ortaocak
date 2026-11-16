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
    net/ClientSession.h
    net/ClientSession.cpp
    net/CloseReason.h
    net/Connection.h
    net/Connection.cpp
    net/Crypto.h
    net/Crypto.cpp
    net/FrameCodec.h
    net/FrameCodec.cpp
    net/Messages.h
    net/Messages.cpp
    net/Opcodes.h
    net/Opcodes.cpp
    net/PacketReader.h
    net/PacketReader.cpp
    net/PacketWriter.h
    net/PacketWriter.cpp
    net/Utf8.h
    net/Utf8.cpp
    game/Game.h
    game/Character.h
    game/Items.h
    game/Items.cpp
    game/Combat.h
    game/Combat.cpp
    game/Monsters.h
    game/Monsters.cpp
    game/Loot.h
    game/Loot.cpp
    game/Skills.h
    game/Skills.cpp
    game/Attachments.h
    game/Attachments.cpp
    game/Names.h
    game/Names.cpp
    game/PathFind.h
)
add_library(vo::shared ALIAS vo_shared)

# ---- assets (M5): runtime asset formats + loaders (docs/ASSET_FORMATS.md), no D3D ----
target_sources(vo_shared PRIVATE
    assets/Assets.h
    assets/AssetFile.h
    assets/AssetFile.cpp
    assets/AssetMath.h
    assets/AssetMath.cpp
    assets/Animation.h
    assets/Animation.cpp
    assets/Mesh.h
    assets/Mesh.cpp
    assets/NormalMapping.h
    assets/Texture.h
    assets/Texture.cpp
    assets/Pose.h
    assets/Pose.cpp
    assets/Terrain.h
    assets/Terrain.cpp
)
# ---- end assets ----

# Includes are rooted at the repository: #include "shared/core/Log.h"
target_include_directories(vo_shared PUBLIC
    "${PROJECT_SOURCE_DIR}"
    "${VO_GENERATED_DIR}"
)

# asio and libsodium are PUBLIC: shared/net/Connection.h exposes asio types, and every consumer of
# shared/net needs both at link time. Winsock is required by asio on Windows.
target_link_libraries(vo_shared
    PUBLIC
        vo::build_options
        asio::asio
        unofficial-sodium::sodium
        ws2_32
        mswsock
    PRIVATE
        tomlplusplus::tomlplusplus
)
