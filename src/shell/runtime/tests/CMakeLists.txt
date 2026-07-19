find_package(GTest QUIET)
if(GTest_FOUND)
    add_executable(prismdrake-shell-runtime-state-tests
        tests/ShellRuntimeCompositionTests.cpp
        tests/ShellRuntimeStateTests.cpp)
    target_link_libraries(prismdrake-shell-runtime-state-tests
        PRIVATE
            GTest::gtest_main
            Prismdrake::shell-runtime-state)
    prismdrake_configure_cpp_target(prismdrake-shell-runtime-state-tests)
    prismdrake_enable_warnings(prismdrake-shell-runtime-state-tests)
    prismdrake_enable_sanitizers(prismdrake-shell-runtime-state-tests)

    add_executable(prismdrake-shell-runtime-tests
        tests/DevelopmentNotificationOwnerTests.cpp
        tests/FatalDisplayShutdownTests.cpp
        tests/ShellControlBusOwnerTests.cpp
        tests/ShellOutputMonitorTests.cpp
        tests/ShellRuntimeOptionsTests.cpp
        tests/TerminationSignalBridgeTests.cpp)
    target_link_libraries(prismdrake-shell-runtime-tests
        PRIVATE
            GTest::gtest
            Prismdrake::shell-runtime
            Qt6::Core)
    prismdrake_configure_cpp_target(prismdrake-shell-runtime-tests)
    prismdrake_enable_warnings(prismdrake-shell-runtime-tests)
    prismdrake_enable_sanitizers(prismdrake-shell-runtime-tests)

    include(GoogleTest)
    gtest_discover_tests(prismdrake-shell-runtime-state-tests
        DISCOVERY_MODE PRE_TEST)
    gtest_discover_tests(prismdrake-shell-runtime-tests
        DISCOVERY_MODE PRE_TEST)
endif()
