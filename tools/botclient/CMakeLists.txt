# botclient: headless test client used for end-to-end server tests (tests/net/integration) and the
# world load test (--bots N --duration S).
add_executable(botclient
    main.cpp
    Bot.h
    Nav.h
    WorldScenarios.cpp
    LoadTest.cpp
)
target_link_libraries(botclient PRIVATE vo::build_options vo::shared)
