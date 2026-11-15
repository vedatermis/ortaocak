# vo_client: Win32 + Direct3D 11 game client (WIN32 subsystem).
add_executable(vo_client WIN32
    main.cpp
    platform/FrameTimer.h
    platform/Win32Util.h
    platform/Win32Util.cpp
    platform/Window.h
    platform/Window.cpp
    render/RenderDevice.h
    render/RenderDevice.cpp
    render/Screenshot.h
    render/Screenshot.cpp
    ui/Ui.h
    scenes/Scenes.h
    net/ClientNet.h
)

target_link_libraries(vo_client PRIVATE
    vo::build_options
    vo::shared
    d3d11
    dxgi
    dxguid
    windowscodecs
    ole32
)
