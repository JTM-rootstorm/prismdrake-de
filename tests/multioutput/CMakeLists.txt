include(GoogleTest)

add_executable(prismdrake-multioutput-tests
    OutputSurfaceRegistryTests.cpp)
target_link_libraries(prismdrake-multioutput-tests
    PRIVATE
        Prismdrake::output-surfaces
        GTest::gtest_main)

prismdrake_configure_cpp_target(prismdrake-multioutput-tests)
prismdrake_enable_warnings(prismdrake-multioutput-tests)
prismdrake_enable_sanitizers(prismdrake-multioutput-tests)
gtest_discover_tests(prismdrake-multioutput-tests
    DISCOVERY_MODE PRE_TEST)
