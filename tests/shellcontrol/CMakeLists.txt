include(GoogleTest)

add_executable(prismdrake-shell-control-tests
    LogoutTransactionTests.cpp
    ShellControlTests.cpp
    ShellCtlTests.cpp)
target_link_libraries(prismdrake-shell-control-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::session-core
        Prismdrake::shell-control
        Prismdrake::shellctl-core)

prismdrake_configure_cpp_target(prismdrake-shell-control-tests)
prismdrake_enable_warnings(prismdrake-shell-control-tests)
prismdrake_enable_sanitizers(prismdrake-shell-control-tests)
gtest_discover_tests(prismdrake-shell-control-tests DISCOVERY_MODE PRE_TEST)

find_program(PRISMDRAKE_DBUS_RUN_SESSION_EXECUTABLE NAMES dbus-run-session)
if(PRISMDRAKE_DBUS_RUN_SESSION_EXECUTABLE)
    add_executable(prismdrake-control-bus-tests ControlBusIntegrationTests.cpp)
    target_link_libraries(prismdrake-control-bus-tests
        PRIVATE
            GTest::gtest_main
            Prismdrake::session-core
            Prismdrake::shell-control
            Prismdrake::shellctl-core
            Prismdrake::sd-bus)
    prismdrake_configure_cpp_target(prismdrake-control-bus-tests)
    prismdrake_enable_warnings(prismdrake-control-bus-tests)
    prismdrake_enable_sanitizers(prismdrake-control-bus-tests)
    add_test(NAME ControlBusIntegrationTest
        COMMAND "${PRISMDRAKE_DBUS_RUN_SESSION_EXECUTABLE}"
            -- "$<TARGET_FILE:prismdrake-control-bus-tests>")
    set_tests_properties(ControlBusIntegrationTest PROPERTIES TIMEOUT 20)
else()
    add_test(NAME ControlBusIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: dbus-run-session is required for isolated control-service validation")
    set_tests_properties(ControlBusIntegrationTest PROPERTIES SKIP_RETURN_CODE 0)
endif()
