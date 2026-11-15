# botclient: headless test client used for end-to-end server tests. Skeleton only (M1).
add_executable(botclient main.cpp)
target_link_libraries(botclient PRIVATE vo::build_options vo::shared)
