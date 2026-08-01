find_package(GTest QUIET)
if(GTest_FOUND)
    add_executable(prismdrake-shell-runtime-composition-tests
        tests/ShellReadinessGateTests.cpp
        tests/ShellRuntimeCompositionTests.cpp)
    target_link_libraries(prismdrake-shell-runtime-composition-tests
        PRIVATE
            GTest::gtest_main
            Prismdrake::shell-runtime-composition)
    prismdrake_configure_cpp_target(prismdrake-shell-runtime-composition-tests)
    prismdrake_enable_warnings(prismdrake-shell-runtime-composition-tests)
    prismdrake_enable_sanitizers(prismdrake-shell-runtime-composition-tests)

    add_executable(prismdrake-shell-runtime-tests
        tests/FatalDisplayShutdownTests.cpp
        tests/Pd2ShellRuntimeTests.cpp
        tests/ShellControlBusOwnerTests.cpp
        tests/ShellOutputMonitorTests.cpp
        tests/ShellRuntimeOptionsTests.cpp
        tests/TranslationCatalogTests.cpp
        tests/TerminationSignalBridgeTests.cpp)
    add_dependencies(prismdrake-shell-runtime-tests prismdrake-translations)
    target_link_libraries(prismdrake-shell-runtime-tests
        PRIVATE
            GTest::gtest
            Prismdrake::shell-runtime
            Qt6::Core)
    prismdrake_configure_cpp_target(prismdrake-shell-runtime-tests)
    prismdrake_enable_warnings(prismdrake-shell-runtime-tests)
    prismdrake_enable_sanitizers(prismdrake-shell-runtime-tests)

    include(GoogleTest)
    gtest_discover_tests(prismdrake-shell-runtime-composition-tests
        DISCOVERY_MODE PRE_TEST)
    gtest_discover_tests(prismdrake-shell-runtime-tests
        DISCOVERY_MODE PRE_TEST)
endif()
