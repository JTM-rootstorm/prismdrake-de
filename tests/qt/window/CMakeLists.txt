add_executable(prismdrake-panel-window-controller-tests
    PanelWindowControllerTests.cpp)
target_link_libraries(prismdrake-panel-window-controller-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::shell-panel-window)

prismdrake_configure_cpp_target(prismdrake-panel-window-controller-tests)
prismdrake_enable_warnings(prismdrake-panel-window-controller-tests)
prismdrake_enable_sanitizers(prismdrake-panel-window-controller-tests)

include(GoogleTest)
gtest_discover_tests(prismdrake-panel-window-controller-tests
    DISCOVERY_MODE PRE_TEST)

add_executable(prismdrake-panel-window-host-integration-tests
    PanelWindowHostIntegrationTests.cpp)
target_link_libraries(prismdrake-panel-window-host-integration-tests
    PRIVATE
        GTest::gtest
        Prismdrake::shell-panel-window
        Qt6::Gui)

prismdrake_configure_cpp_target(prismdrake-panel-window-host-integration-tests)
prismdrake_enable_warnings(prismdrake-panel-window-host-integration-tests)
prismdrake_enable_sanitizers(prismdrake-panel-window-host-integration-tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(PRISMDRAKE_PANEL_WINDOW_XVFB_EXECUTABLE NAMES Xvfb)
find_program(PRISMDRAKE_PANEL_WINDOW_OPENBOX_EXECUTABLE NAMES openbox)
find_program(PRISMDRAKE_PANEL_WINDOW_XPROP_EXECUTABLE NAMES xprop)
set(prismdrake_panel_window_xvfb_runner
    "${CMAKE_CURRENT_SOURCE_DIR}/../../integration/run_with_xvfb.py")

if(PRISMDRAKE_PANEL_WINDOW_XVFB_EXECUTABLE)
    add_test(
        NAME PanelWindowHostIntegrationTest
        COMMAND
            "${Python3_EXECUTABLE}" "${prismdrake_panel_window_xvfb_runner}"
            "${PRISMDRAKE_PANEL_WINDOW_XVFB_EXECUTABLE}"
            "$<TARGET_FILE:prismdrake-panel-window-host-integration-tests>"
            --)
    set_tests_properties(PanelWindowHostIntegrationTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=xcb"
        TIMEOUT 30)
else()
    add_test(
        NAME PanelWindowHostIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: Xvfb is unavailable; the Qt/X11 panel host requires the VM or CI profile")
    set_tests_properties(PanelWindowHostIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
endif()

if(PRISMDRAKE_PANEL_WINDOW_XVFB_EXECUTABLE
   AND PRISMDRAKE_PANEL_WINDOW_OPENBOX_EXECUTABLE
   AND PRISMDRAKE_PANEL_WINDOW_XPROP_EXECUTABLE)
    add_test(
        NAME PanelWindowHostOpenboxIntegrationTest
        COMMAND
            "${Python3_EXECUTABLE}" "${prismdrake_panel_window_xvfb_runner}"
            --openbox "${PRISMDRAKE_PANEL_WINDOW_OPENBOX_EXECUTABLE}"
            --xprop "${PRISMDRAKE_PANEL_WINDOW_XPROP_EXECUTABLE}"
            "${PRISMDRAKE_PANEL_WINDOW_XVFB_EXECUTABLE}"
            "$<TARGET_FILE:prismdrake-panel-window-host-integration-tests>"
            --)
    set_tests_properties(PanelWindowHostOpenboxIntegrationTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=xcb"
        TIMEOUT 30)
else()
    add_test(
        NAME PanelWindowHostOpenboxIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: Xvfb, Openbox, or xprop is unavailable; panel WM integration requires all three")
    set_tests_properties(PanelWindowHostOpenboxIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
endif()
