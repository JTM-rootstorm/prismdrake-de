include(GoogleTest)

add_executable(prismdrake-session-supervisor-child-fixture SupervisorChildFixture.cpp)
prismdrake_configure_cpp_target(prismdrake-session-supervisor-child-fixture)
prismdrake_enable_warnings(prismdrake-session-supervisor-child-fixture)
prismdrake_enable_sanitizers(prismdrake-session-supervisor-child-fixture)

add_executable(prismdrake-session-supervisor-tests SessionSupervisorTests.cpp)
target_link_libraries(prismdrake-session-supervisor-tests
    PRIVATE
        Prismdrake::session-core
        GTest::gtest_main)

prismdrake_configure_cpp_target(prismdrake-session-supervisor-tests)
prismdrake_enable_warnings(prismdrake-session-supervisor-tests)
prismdrake_enable_sanitizers(prismdrake-session-supervisor-tests)

add_dependencies(prismdrake-session-supervisor-tests prismdrake-session-supervisor-child-fixture)
gtest_discover_tests(
    prismdrake-session-supervisor-tests
    PROPERTIES
        ENVIRONMENT
            "PRISMDRAKE_SESSION_SUPERVISOR_CHILD_FIXTURE=$<TARGET_FILE:prismdrake-session-supervisor-child-fixture>")
