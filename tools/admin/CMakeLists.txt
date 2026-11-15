# vo_admin: database administration CLI (migrate, status, accounts, characters). docs/DATABASE.md
add_executable(vo_admin
    main.cpp
)
target_link_libraries(vo_admin PRIVATE
    vo::build_options
    vo_server_lib
    libpqxx::pqxx
)
