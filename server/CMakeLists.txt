# vo_server_lib: everything except main(), so the unit/integration tests can link the server code.
add_library(vo_server_lib STATIC
    auth/IAccountService.h
    auth/LoginRateLimiter.h
    auth/LoginRateLimiter.cpp
    auth/MemoryAccountService.h
    auth/MemoryAccountService.cpp
    net/ServerContext.h
    net/Server.h
    net/Server.cpp
    net/Session.h
    net/Session.cpp
)
target_link_libraries(vo_server_lib PUBLIC
    vo::build_options
    vo::shared
)

# vo_server: single server executable (login + world modules; may be split later).
add_executable(vo_server
    main.cpp
)

target_link_libraries(vo_server PRIVATE
    vo::build_options
    vo_server_lib
    unofficial-sodium::sodium
    libpqxx::pqxx
)
