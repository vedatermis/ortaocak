# vo_server_lib: everything except main(), so the unit/integration tests and vo_admin can link the
# server code.
add_library(vo_server_lib STATIC
    auth/IAccountService.h
    auth/LoginRateLimiter.h
    auth/LoginRateLimiter.cpp
    auth/MemoryAccountService.h
    auth/MemoryAccountService.cpp
    auth/PostgresAccountService.h
    auth/PostgresAccountService.cpp
    db/ConnectionPool.h
    db/ConnectionPool.cpp
    db/Db.h
    db/Db.cpp
    db/DbSettings.h
    db/DbSettings.cpp
    db/Migrations.h
    db/Migrations.cpp
    db/WorkerPool.h
    db/WorkerPool.cpp
    net/ServerContext.h
    net/Server.h
    net/Server.cpp
    net/Session.h
    net/Session.cpp
    world/AoiGrid.h
    world/AoiGrid.cpp
    world/MoveRateLimiter.h
    world/MoveValidator.h
    world/MoveValidator.cpp
    world/TickStats.h
    world/TickStats.cpp
    world/World.h
    world/World.cpp
    world/WorldTypes.h
    world/Zone.h
    world/Zone.cpp
)
# libpqxx is PUBLIC: tests and vo_admin talk to the database through it as well.
target_link_libraries(vo_server_lib PUBLIC
    vo::build_options
    vo::shared
    libpqxx::pqxx
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
