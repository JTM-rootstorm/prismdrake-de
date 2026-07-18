add_library(prismdrake-shell-runtime-state STATIC
    ShellRuntimeState.cpp
    ShellRuntimeState.hpp)
add_library(Prismdrake::shell-runtime-state ALIAS prismdrake-shell-runtime-state)
target_include_directories(prismdrake-shell-runtime-state
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
prismdrake_configure_cpp_target(prismdrake-shell-runtime-state)
prismdrake_enable_warnings(prismdrake-shell-runtime-state)
prismdrake_enable_sanitizers(prismdrake-shell-runtime-state)

add_library(prismdrake-shell-runtime STATIC
    ShellRuntime.cpp
    ShellRuntime.hpp
    ShellRuntimeOptions.cpp
    ShellRuntimeOptions.hpp
    TerminationSignalBridge.cpp
    TerminationSignalBridge.hpp)
add_library(Prismdrake::shell-runtime ALIAS prismdrake-shell-runtime)
set_target_properties(prismdrake-shell-runtime PROPERTIES AUTOMOC ON)
target_include_directories(prismdrake-shell-runtime
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-runtime
    PUBLIC
        Prismdrake::shell-runtime-state
        Prismdrake::shell-launcher-controller
        Prismdrake::shell-panel-window
        Prismdrake::shell-settings-client
        Prismdrake::shell-task-controller
        Prismdrake::shell-theme
        Qt6::Core
    PRIVATE
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick)
prismdrake_configure_cpp_target(prismdrake-shell-runtime)
prismdrake_enable_warnings(prismdrake-shell-runtime)
prismdrake_enable_sanitizers(prismdrake-shell-runtime)

add_executable(prismdrake-shell main.cpp)
target_link_libraries(prismdrake-shell
    PRIVATE
        Prismdrake::shell-runtime
        Prismdrake::shell-launcher-qml
        Prismdrake::shell-panel-qml
        Qt6::Gui
        Qt6::Quick)
prismdrake_configure_cpp_target(prismdrake-shell)
prismdrake_enable_warnings(prismdrake-shell)
prismdrake_enable_sanitizers(prismdrake-shell)
qt_finalize_executable(prismdrake-shell)

install(
    TARGETS prismdrake-shell
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_executable(prismdrake-shell-runtime-state-tests
            tests/ShellRuntimeStateTests.cpp)
        target_link_libraries(prismdrake-shell-runtime-state-tests
            PRIVATE
                GTest::gtest_main
                Prismdrake::shell-runtime-state)
        prismdrake_configure_cpp_target(prismdrake-shell-runtime-state-tests)
        prismdrake_enable_warnings(prismdrake-shell-runtime-state-tests)
        prismdrake_enable_sanitizers(prismdrake-shell-runtime-state-tests)

        add_executable(prismdrake-shell-runtime-tests
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
endif()
