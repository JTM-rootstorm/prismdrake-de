add_executable(prismdrake-pin-state-tests PinStateTests.cpp)
target_link_libraries(prismdrake-pin-state-tests
    PRIVATE Prismdrake::pin-state GTest::gtest_main)

prismdrake_configure_cpp_target(prismdrake-pin-state-tests)
prismdrake_enable_warnings(prismdrake-pin-state-tests)
prismdrake_enable_sanitizers(prismdrake-pin-state-tests)
gtest_discover_tests(prismdrake-pin-state-tests DISCOVERY_MODE PRE_TEST)

add_executable(prismdrake-async-pin-state-controller-tests AsyncPinStateControllerTests.cpp)
target_link_libraries(prismdrake-async-pin-state-controller-tests
    PRIVATE Prismdrake::async-pin-state GTest::gtest Qt6::Core Qt6::Test)

prismdrake_configure_cpp_target(prismdrake-async-pin-state-controller-tests)
prismdrake_enable_warnings(prismdrake-async-pin-state-controller-tests)
prismdrake_enable_sanitizers(prismdrake-async-pin-state-controller-tests)
gtest_discover_tests(prismdrake-async-pin-state-controller-tests DISCOVERY_MODE PRE_TEST)
