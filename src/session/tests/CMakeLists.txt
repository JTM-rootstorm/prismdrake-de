include(GoogleTest)

add_executable(prismdrake-session-supervisor-child-fixture SupervisorChildFixture.cpp)
target_link_libraries(prismdrake-session-supervisor-child-fixture
    PRIVATE Prismdrake::foundation)
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

find_program(PRISMDRAKE_SESSION_DBUS_RUN_SESSION NAMES dbus-run-session REQUIRED)

add_executable(prismdrake-session-composition-runtime-tests
    SessionCompositionRuntimeTests.cpp)
target_link_libraries(prismdrake-session-composition-runtime-tests
    PRIVATE
        Prismdrake::session-core
        GTest::gtest_main)
prismdrake_configure_cpp_target(prismdrake-session-composition-runtime-tests)
prismdrake_enable_warnings(prismdrake-session-composition-runtime-tests)
prismdrake_enable_sanitizers(prismdrake-session-composition-runtime-tests)

add_test(
    NAME session-composition-runtime
    COMMAND
        "${PRISMDRAKE_SESSION_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:prismdrake-session-composition-runtime-tests>")
