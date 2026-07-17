find_program(PRISMDRAKE_SETTINGS_CLIENT_DBUS_RUN_SESSION_EXECUTABLE
    NAMES dbus-run-session REQUIRED)

add_executable(prismdrake-settings-snapshot-client-tests
    SettingsSnapshotClientTests.cpp)
target_compile_definitions(prismdrake-settings-snapshot-client-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(prismdrake-settings-snapshot-client-tests
    PRIVATE
        GTest::gtest
        Prismdrake::shell-settings-client
        Prismdrake::shell-theme
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-settings-snapshot-client-tests)
prismdrake_enable_warnings(prismdrake-settings-snapshot-client-tests)
prismdrake_enable_sanitizers(prismdrake-settings-snapshot-client-tests)

add_test(NAME SettingsSnapshotClientDbusTest
    COMMAND
        "${PRISMDRAKE_SETTINGS_CLIENT_DBUS_RUN_SESSION_EXECUTABLE}" --
        "$<TARGET_FILE:prismdrake-settings-snapshot-client-tests>")
set_tests_properties(SettingsSnapshotClientDbusTest PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 30)
