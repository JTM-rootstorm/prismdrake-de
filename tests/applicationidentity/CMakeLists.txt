add_executable(prismdrake-application-identity-tests ApplicationIdentityTests.cpp)
target_link_libraries(prismdrake-application-identity-tests
    PRIVATE Prismdrake::launcher-model GTest::gtest_main)

prismdrake_configure_cpp_target(prismdrake-application-identity-tests)
prismdrake_enable_warnings(prismdrake-application-identity-tests)
prismdrake_enable_sanitizers(prismdrake-application-identity-tests)
gtest_discover_tests(prismdrake-application-identity-tests DISCOVERY_MODE PRE_TEST)
