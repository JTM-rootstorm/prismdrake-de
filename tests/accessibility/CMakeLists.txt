add_test(
    NAME AccessibilityEvidenceContractTest
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_live_atspi.py")
set_tests_properties(AccessibilityEvidenceContractTest PROPERTIES TIMEOUT 10)

find_program(PRISMDRAKE_ATSPI_DBUS_RUN_SESSION_EXECUTABLE NAMES dbus-run-session)
find_program(PRISMDRAKE_ATSPI_GDBUS_EXECUTABLE NAMES gdbus)
find_program(PRISMDRAKE_ATSPI_OPENBOX_EXECUTABLE NAMES openbox)
find_program(PRISMDRAKE_ATSPI_XDOTOOL_EXECUTABLE NAMES xdotool)
find_program(PRISMDRAKE_ATSPI_XVFB_EXECUTABLE NAMES Xvfb)

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
        "import gi; gi.require_version('Atspi', '2.0'); from gi.repository import Atspi"
    RESULT_VARIABLE prismdrake_atspi_python_result
    OUTPUT_QUIET
    ERROR_QUIET)

if(PRISMDRAKE_ATSPI_DBUS_RUN_SESSION_EXECUTABLE
   AND PRISMDRAKE_ATSPI_GDBUS_EXECUTABLE
   AND PRISMDRAKE_ATSPI_OPENBOX_EXECUTABLE
   AND PRISMDRAKE_ATSPI_XDOTOOL_EXECUTABLE
   AND PRISMDRAKE_ATSPI_XVFB_EXECUTABLE
   AND prismdrake_atspi_python_result EQUAL 0)
    add_test(
        NAME LiveAtspiAccessibilityTest
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/live_atspi.py"
            --dbus-run-session "${PRISMDRAKE_ATSPI_DBUS_RUN_SESSION_EXECUTABLE}"
            --gdbus "${PRISMDRAKE_ATSPI_GDBUS_EXECUTABLE}"
            --openbox "${PRISMDRAKE_ATSPI_OPENBOX_EXECUTABLE}"
            --xdotool "${PRISMDRAKE_ATSPI_XDOTOOL_EXECUTABLE}"
            --xvfb "${PRISMDRAKE_ATSPI_XVFB_EXECUTABLE}"
            --settingsd "$<TARGET_FILE:prismdrake-settingsd>"
            --shell "$<TARGET_FILE:prismdrake-shell>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/live-atspi-evidence.json")
    set_tests_properties(LiveAtspiAccessibilityTest PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 45)
elseif(PRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST)
    message(FATAL_ERROR
        "PRISMDRAKE_REQUIRE_LIVE_ATSPI_TEST is enabled, but live AT-SPI inspection "
        "requires Xvfb, Openbox, xdotool, dbus-run-session, gdbus, and Python Atspi bindings")
else()
    add_test(
        NAME LiveAtspiAccessibilityTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: live AT-SPI inspection requires Xvfb, Openbox, xdotool, D-Bus, and Python Atspi bindings")
    set_tests_properties(LiveAtspiAccessibilityTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
    message(STATUS
        "Live AT-SPI dependencies are incomplete; the accessibility test is registered as skipped")
endif()
