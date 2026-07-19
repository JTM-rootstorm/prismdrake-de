add_executable(prismdrake-icon-resolver-tests
    IconResolverTests.cpp
    TestMain.cpp)
target_link_libraries(prismdrake-icon-resolver-tests
    PRIVATE Prismdrake::icon-resolver GTest::gtest Qt6::Core Qt6::Gui)

prismdrake_configure_cpp_target(prismdrake-icon-resolver-tests)
prismdrake_enable_warnings(prismdrake-icon-resolver-tests)
prismdrake_enable_sanitizers(prismdrake-icon-resolver-tests)
gtest_discover_tests(prismdrake-icon-resolver-tests DISCOVERY_MODE PRE_TEST)
