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
