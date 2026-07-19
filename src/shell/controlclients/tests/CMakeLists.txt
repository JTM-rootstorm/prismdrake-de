find_program(PRISMDRAKE_CONTROL_CLIENT_DBUS_RUN_SESSION_EXECUTABLE
    NAMES dbus-run-session)

if(PRISMDRAKE_CONTROL_CLIENT_DBUS_RUN_SESSION_EXECUTABLE)
    add_executable(prismdrake-shell-control-client-tests
        ControlClientIntegrationTests.cpp)
    target_link_libraries(prismdrake-shell-control-client-tests
        PRIVATE
            GTest::gtest
            Prismdrake::shell-control-clients
            Prismdrake::sd-bus
            Qt6::Core)
    prismdrake_configure_cpp_target(prismdrake-shell-control-client-tests)
    prismdrake_enable_warnings(prismdrake-shell-control-client-tests)
    prismdrake_enable_sanitizers(prismdrake-shell-control-client-tests)

    add_test(NAME ShellControlClientIntegrationTest
        COMMAND "${PRISMDRAKE_CONTROL_CLIENT_DBUS_RUN_SESSION_EXECUTABLE}"
            -- "$<TARGET_FILE:prismdrake-shell-control-client-tests>")
    set_tests_properties(ShellControlClientIntegrationTest PROPERTIES TIMEOUT 20)
else()
    add_test(NAME ShellControlClientIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: dbus-run-session is required for private shell control-client validation")
    set_tests_properties(ShellControlClientIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
endif()
