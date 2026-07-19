add_executable(prismdrake-pin-state-tests PinStateTests.cpp)
target_link_libraries(prismdrake-pin-state-tests
    PRIVATE Prismdrake::pin-state GTest::gtest_main)

prismdrake_configure_cpp_target(prismdrake-pin-state-tests)
prismdrake_enable_warnings(prismdrake-pin-state-tests)
prismdrake_enable_sanitizers(prismdrake-pin-state-tests)
gtest_discover_tests(prismdrake-pin-state-tests DISCOVERY_MODE PRE_TEST)
