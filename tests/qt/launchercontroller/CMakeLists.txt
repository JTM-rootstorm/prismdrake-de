add_executable(prismdrake-shell-launcher-controller-tests
    LauncherControllerTests.cpp
    TestMain.cpp)

target_link_libraries(prismdrake-shell-launcher-controller-tests
    PRIVATE
        GTest::gtest
        Prismdrake::shell-launcher-controller
        Qt6::Test)

prismdrake_configure_cpp_target(prismdrake-shell-launcher-controller-tests)
prismdrake_enable_warnings(prismdrake-shell-launcher-controller-tests)
prismdrake_enable_sanitizers(prismdrake-shell-launcher-controller-tests)

include(GoogleTest)
gtest_discover_tests(prismdrake-shell-launcher-controller-tests
    DISCOVERY_MODE PRE_TEST)
