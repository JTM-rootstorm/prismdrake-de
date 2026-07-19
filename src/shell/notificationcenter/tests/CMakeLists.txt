include(GoogleTest)

add_executable(prismdrake-notification-center-state-tests
    NotificationCenterStateParserTests.cpp)
target_link_libraries(prismdrake-notification-center-state-tests
    PRIVATE
        Prismdrake::notification-center-state
        GTest::gtest_main)

prismdrake_configure_cpp_target(prismdrake-notification-center-state-tests)
prismdrake_enable_warnings(prismdrake-notification-center-state-tests)
prismdrake_enable_sanitizers(prismdrake-notification-center-state-tests)
gtest_discover_tests(prismdrake-notification-center-state-tests
    DISCOVERY_MODE PRE_TEST)

add_executable(prismdrake-notification-center-presentation-tests
    NotificationCenterPresentationTests.cpp)
target_link_libraries(prismdrake-notification-center-presentation-tests
    PRIVATE
        Prismdrake::notification-center-presentation
        GTest::gtest_main)

prismdrake_configure_cpp_target(prismdrake-notification-center-presentation-tests)
prismdrake_enable_warnings(prismdrake-notification-center-presentation-tests)
prismdrake_enable_sanitizers(prismdrake-notification-center-presentation-tests)
gtest_discover_tests(prismdrake-notification-center-presentation-tests
    DISCOVERY_MODE PRE_TEST)

find_program(PRISMDRAKE_NOTIFICATION_CLIENT_DBUS_RUN_SESSION_EXECUTABLE
    NAMES dbus-run-session REQUIRED)

add_executable(prismdrake-notification-center-client-tests
    NotificationCenterClientTests.cpp)
target_link_libraries(prismdrake-notification-center-client-tests
    PRIVATE
        Prismdrake::notification-center-client
        GTest::gtest
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-notification-center-client-tests)
prismdrake_enable_warnings(prismdrake-notification-center-client-tests)
prismdrake_enable_sanitizers(prismdrake-notification-center-client-tests)

add_test(NAME NotificationCenterClientDbusTest
    COMMAND
        "${PRISMDRAKE_NOTIFICATION_CLIENT_DBUS_RUN_SESSION_EXECUTABLE}" --
        "$<TARGET_FILE:prismdrake-notification-center-client-tests>")
set_tests_properties(NotificationCenterClientDbusTest PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 30)
