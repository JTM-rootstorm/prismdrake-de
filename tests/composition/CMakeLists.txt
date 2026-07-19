include(GoogleTest)

add_executable(prismdrake-shell-composition-tests
    ShellCompositionTests.cpp
)

target_link_libraries(prismdrake-shell-composition-tests
    PRIVATE
        Prismdrake::shell-composition
        GTest::gtest_main
)

prismdrake_configure_cpp_target(prismdrake-shell-composition-tests)
prismdrake_enable_warnings(prismdrake-shell-composition-tests)
prismdrake_enable_sanitizers(prismdrake-shell-composition-tests)

gtest_discover_tests(prismdrake-shell-composition-tests
    TEST_PREFIX "composition."
    DISCOVERY_MODE PRE_TEST
)
