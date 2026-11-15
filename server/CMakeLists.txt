# vo_server: single server executable (login + world modules; may be split later).
add_executable(vo_server
    main.cpp
)

target_link_libraries(vo_server PRIVATE
    vo::build_options
    vo::shared
    asio::asio
    unofficial-sodium::sodium
    libpqxx::pqxx
)

# Winsock is required by asio on Windows.
target_link_libraries(vo_server PRIVATE ws2_32 mswsock)
