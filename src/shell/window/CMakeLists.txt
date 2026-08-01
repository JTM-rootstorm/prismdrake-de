add_library(prismdrake-shell-x11-property-worker STATIC
    X11PropertyWorker.cpp
    X11PropertyWorker.hpp)
add_library(Prismdrake::shell-x11-property-worker ALIAS
    prismdrake-shell-x11-property-worker)
target_include_directories(prismdrake-shell-x11-property-worker
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-x11-property-worker
    PUBLIC Prismdrake::x11 Qt6::Core)
prismdrake_configure_cpp_target(prismdrake-shell-x11-property-worker)
prismdrake_enable_warnings(prismdrake-shell-x11-property-worker)
prismdrake_enable_sanitizers(prismdrake-shell-x11-property-worker)

add_library(prismdrake-shell-panel-window STATIC
    PanelSurfaceHost.cpp
    PanelSurfaceHost.hpp)
add_library(Prismdrake::shell-panel-window ALIAS prismdrake-shell-panel-window)

target_include_directories(prismdrake-shell-panel-window
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-panel-window
    PUBLIC
        Prismdrake::output-surfaces
        Prismdrake::shell-x11-property-worker
        Prismdrake::x11
        Qt6::Core
        Qt6::Gui
    PRIVATE Qt6::Quick)

prismdrake_configure_cpp_target(prismdrake-shell-panel-window)
prismdrake_enable_warnings(prismdrake-shell-panel-window)
prismdrake_enable_sanitizers(prismdrake-shell-panel-window)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(prismdrake-panel-surface-host-tests
        tests/PanelSurfaceHostTests.cpp)
    target_link_libraries(prismdrake-panel-surface-host-tests
        PRIVATE GTest::gtest Prismdrake::shell-panel-window Qt6::Quick)
    prismdrake_configure_cpp_target(prismdrake-panel-surface-host-tests)
    prismdrake_enable_warnings(prismdrake-panel-surface-host-tests)
    prismdrake_enable_sanitizers(prismdrake-panel-surface-host-tests)

    add_test(NAME PanelSurfaceHostTest
        COMMAND prismdrake-panel-surface-host-tests)
    set_tests_properties(PanelSurfaceHostTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 15)

    add_executable(prismdrake-x11-property-worker-tests
        tests/X11PropertyWorkerTests.cpp)
    target_link_libraries(prismdrake-x11-property-worker-tests
        PRIVATE GTest::gtest Prismdrake::shell-x11-property-worker Qt6::Core)
    prismdrake_configure_cpp_target(prismdrake-x11-property-worker-tests)
    prismdrake_enable_warnings(prismdrake-x11-property-worker-tests)
    prismdrake_enable_sanitizers(prismdrake-x11-property-worker-tests)

    add_test(NAME X11PropertyWorkerTest
        COMMAND prismdrake-x11-property-worker-tests)
    set_tests_properties(X11PropertyWorkerTest PROPERTIES TIMEOUT 5)
endif()
