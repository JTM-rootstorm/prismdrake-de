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

find_package(PkgConfig REQUIRED)
pkg_check_modules(PRISMDRAKE_TEST_XCB REQUIRED IMPORTED_TARGET xcb)

add_executable(prismdrake-x11-integration-tests
    EwmhCapabilitiesTests.cpp
    PropertyReaderTests.cpp
    RootEventStreamTests.cpp
    X11ConnectionTests.cpp)
target_link_libraries(prismdrake-x11-integration-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::x11
        PkgConfig::PRISMDRAKE_TEST_XCB)

prismdrake_configure_cpp_target(prismdrake-x11-integration-tests)
prismdrake_enable_warnings(prismdrake-x11-integration-tests)
prismdrake_enable_sanitizers(prismdrake-x11-integration-tests)

find_program(PRISMDRAKE_XVFB_EXECUTABLE NAMES Xvfb)
if(PRISMDRAKE_XVFB_EXECUTABLE)
    add_test(
        NAME X11ConnectionIntegrationTest
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_with_xvfb.py"
            "${PRISMDRAKE_XVFB_EXECUTABLE}"
            "$<TARGET_FILE:prismdrake-x11-integration-tests>")
    set_tests_properties(X11ConnectionIntegrationTest PROPERTIES TIMEOUT 30)
else()
    add_test(
        NAME X11ConnectionIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: Xvfb is unavailable; X11 integration requires the VM or CI profile")
    set_tests_properties(X11ConnectionIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
    message(STATUS
        "Xvfb is unavailable; the X11 integration test is registered as skipped")
endif()
