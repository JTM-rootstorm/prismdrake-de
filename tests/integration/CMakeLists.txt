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
    DockPropertiesTests.cpp
    EwmhCapabilitiesTests.cpp
    EwmhTaskSourceTests.cpp
    EwmhWindowRequestsTests.cpp
    PropertyReaderTests.cpp
    RandrTopologyTests.cpp
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
find_program(PRISMDRAKE_OPENBOX_EXECUTABLE NAMES openbox)
find_program(PRISMDRAKE_XPROP_EXECUTABLE NAMES xprop)
if(PRISMDRAKE_XVFB_EXECUTABLE)
    add_test(
        NAME X11ConnectionIntegrationTest
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_with_xvfb.py"
            "${PRISMDRAKE_XVFB_EXECUTABLE}"
            "$<TARGET_FILE:prismdrake-x11-integration-tests>"
            --)
    set_tests_properties(X11ConnectionIntegrationTest PROPERTIES TIMEOUT 30)
    add_test(
        NAME X11RandrUnavailableIntegrationTest
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_with_xvfb.py"
            --xvfb-disable-randr
            "${PRISMDRAKE_XVFB_EXECUTABLE}"
            "$<TARGET_FILE:prismdrake-x11-integration-tests>"
            --
            --gtest_filter=RandrTopologyIntegrationTest.UsesFreshCoreRootFallbackWithoutRandr)
    set_tests_properties(X11RandrUnavailableIntegrationTest PROPERTIES TIMEOUT 30)
else()
    add_test(
        NAME X11ConnectionIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: Xvfb is unavailable; X11 integration requires the VM or CI profile")
    set_tests_properties(X11ConnectionIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
    add_test(
        NAME X11RandrUnavailableIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: Xvfb is unavailable; RandR fallback integration requires the VM or CI profile")
    set_tests_properties(X11RandrUnavailableIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
    message(STATUS
        "Xvfb is unavailable; the X11 integration test is registered as skipped")
endif()

if(PRISMDRAKE_XVFB_EXECUTABLE AND PRISMDRAKE_OPENBOX_EXECUTABLE
   AND PRISMDRAKE_XPROP_EXECUTABLE)
    add_test(
        NAME X11DockOpenboxIntegrationTest
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_with_xvfb.py"
            --openbox "${PRISMDRAKE_OPENBOX_EXECUTABLE}"
            --xprop "${PRISMDRAKE_XPROP_EXECUTABLE}"
            "${PRISMDRAKE_XVFB_EXECUTABLE}"
            "$<TARGET_FILE:prismdrake-x11-integration-tests>"
            --
            --gtest_filter=DockPropertiesIntegrationTest.VerifiedWindowManagerAppliesCurrentDesktopWorkarea)
    set_tests_properties(X11DockOpenboxIntegrationTest PROPERTIES TIMEOUT 30)
elseif(NOT PRISMDRAKE_XVFB_EXECUTABLE)
    add_test(
        NAME X11DockOpenboxIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: Xvfb is unavailable; dock work-area integration requires the VM or CI profile")
    set_tests_properties(X11DockOpenboxIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
else()
    add_test(
        NAME X11DockOpenboxIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: Openbox or xprop is unavailable; WM-applied work-area verification requires both")
    set_tests_properties(X11DockOpenboxIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
endif()
