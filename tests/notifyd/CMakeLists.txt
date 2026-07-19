include(GoogleTest)

add_executable(prismdrake-notification-authority-tests
    NotificationAuthorityTests.cpp)
target_link_libraries(prismdrake-notification-authority-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::notification-authority
        nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-notification-authority-tests)
prismdrake_enable_warnings(prismdrake-notification-authority-tests)
prismdrake_enable_sanitizers(prismdrake-notification-authority-tests)
gtest_discover_tests(prismdrake-notification-authority-tests DISCOVERY_MODE PRE_TEST)

find_program(PRISMDRAKE_NOTIFYD_DBUS_RUN_SESSION NAMES dbus-run-session REQUIRED)

add_executable(prismdrake-notification-service-tests NotificationServiceTests.cpp)
target_link_libraries(prismdrake-notification-service-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::notification-service
        Prismdrake::sd-bus
        nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-notification-service-tests)
prismdrake_enable_warnings(prismdrake-notification-service-tests)
prismdrake_enable_sanitizers(prismdrake-notification-service-tests)

add_test(
    NAME NotificationServiceIntegrationTest
    COMMAND
        "${PRISMDRAKE_NOTIFYD_DBUS_RUN_SESSION}" --
        "${CMAKE_COMMAND}" -E env --unset=DISPLAY
        "PRISMDRAKE_NOTIFYD_EXECUTABLE=$<TARGET_FILE:prismdrake-notifyd>"
        "$<TARGET_FILE:prismdrake-notification-service-tests>")
set_tests_properties(NotificationServiceIntegrationTest PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 60)
