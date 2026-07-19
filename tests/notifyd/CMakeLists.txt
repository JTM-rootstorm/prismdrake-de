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
