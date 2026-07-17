find_program(PRISMDRAKE_DBUS_RUN_SESSION_EXECUTABLE NAMES dbus-run-session REQUIRED)

add_executable(prismdrake-settingsd-integration-tests
    SettingsServiceTests.cpp)
target_link_libraries(prismdrake-settingsd-integration-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::session-core
        Prismdrake::sd-bus)
target_compile_definitions(prismdrake-settingsd-integration-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

prismdrake_configure_cpp_target(prismdrake-settingsd-integration-tests)
prismdrake_enable_warnings(prismdrake-settingsd-integration-tests)
prismdrake_enable_sanitizers(prismdrake-settingsd-integration-tests)

add_test(
    NAME SettingsServiceIntegrationTest
    COMMAND
        "${PRISMDRAKE_DBUS_RUN_SESSION_EXECUTABLE}" --
        "${CMAKE_COMMAND}" -E env --unset=DISPLAY
        "PRISMDRAKE_SETTINGSD_EXECUTABLE=$<TARGET_FILE:prismdrake-settingsd>"
        "$<TARGET_FILE:prismdrake-settingsd-integration-tests>")
set_tests_properties(SettingsServiceIntegrationTest PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 60)
