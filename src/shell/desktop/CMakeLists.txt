set(CMAKE_AUTOMOC ON)

qt_add_qml_module(prismdrake-shell-desktop
    STATIC
    URI org.prismdrake.shell.desktop
    VERSION 0.1
    OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/qml/org/prismdrake/shell/desktop"
    RESOURCE_PREFIX /qt/qml
    NO_PLUGIN
    SOURCES
        DesktopSurfaceController.cpp
        DesktopSurfaceController.hpp
        DesktopWallpaperItem.cpp
        DesktopWallpaperItem.hpp
        DesktopWindowHost.cpp
        DesktopWindowHost.hpp
    QML_FILES
        DesktopSurface.qml)
add_library(Prismdrake::shell-desktop ALIAS prismdrake-shell-desktop)

target_include_directories(prismdrake-shell-desktop
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-desktop
    PUBLIC
        Prismdrake::output-surfaces
        Prismdrake::wallpaper-core
        Prismdrake::x11
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick)

prismdrake_configure_cpp_target(prismdrake-shell-desktop)
prismdrake_enable_warnings(prismdrake-shell-desktop)
prismdrake_enable_sanitizers(prismdrake-shell-desktop)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(prismdrake-desktop-surface-controller-tests
        tests/DesktopSurfaceControllerTests.cpp)
    target_link_libraries(prismdrake-desktop-surface-controller-tests
        PRIVATE GTest::gtest_main Prismdrake::shell-desktop)
    prismdrake_configure_cpp_target(prismdrake-desktop-surface-controller-tests)
    prismdrake_enable_warnings(prismdrake-desktop-surface-controller-tests)
    prismdrake_enable_sanitizers(prismdrake-desktop-surface-controller-tests)

    include(GoogleTest)
    gtest_discover_tests(prismdrake-desktop-surface-controller-tests
        DISCOVERY_MODE PRE_TEST)

    add_executable(prismdrake-desktop-window-host-integration-tests
        tests/DesktopWindowHostIntegrationTests.cpp)
    target_link_libraries(prismdrake-desktop-window-host-integration-tests
        PRIVATE GTest::gtest Prismdrake::shell-desktop)
    prismdrake_configure_cpp_target(prismdrake-desktop-window-host-integration-tests)
    prismdrake_enable_warnings(prismdrake-desktop-window-host-integration-tests)
    prismdrake_enable_sanitizers(prismdrake-desktop-window-host-integration-tests)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(PRISMDRAKE_DESKTOP_XVFB_EXECUTABLE NAMES Xvfb)
    find_program(PRISMDRAKE_DESKTOP_OPENBOX_EXECUTABLE NAMES openbox)
    find_program(PRISMDRAKE_DESKTOP_XPROP_EXECUTABLE NAMES xprop)
    set(prismdrake_desktop_xvfb_runner
        "${PROJECT_SOURCE_DIR}/tests/integration/run_with_xvfb.py")

    if(PRISMDRAKE_DESKTOP_XVFB_EXECUTABLE)
        add_test(
            NAME DesktopWindowHostIntegrationTest
            COMMAND
                "${Python3_EXECUTABLE}" "${prismdrake_desktop_xvfb_runner}"
                "${PRISMDRAKE_DESKTOP_XVFB_EXECUTABLE}"
                "$<TARGET_FILE:prismdrake-desktop-window-host-integration-tests>"
                --)
        set_tests_properties(DesktopWindowHostIntegrationTest PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=xcb"
            TIMEOUT 30)
    else()
        add_test(
            NAME DesktopWindowHostIntegrationTest
            COMMAND "${CMAKE_COMMAND}" -E echo
                "SKIP: Xvfb is unavailable; the desktop host requires the VM profile")
        set_tests_properties(DesktopWindowHostIntegrationTest PROPERTIES
            SKIP_REGULAR_EXPRESSION "^SKIP:"
            TIMEOUT 5)
    endif()

    if(PRISMDRAKE_DESKTOP_XVFB_EXECUTABLE
       AND PRISMDRAKE_DESKTOP_OPENBOX_EXECUTABLE
       AND PRISMDRAKE_DESKTOP_XPROP_EXECUTABLE)
        add_test(
            NAME DesktopWindowHostOpenboxIntegrationTest
            COMMAND
                "${Python3_EXECUTABLE}" "${prismdrake_desktop_xvfb_runner}"
                --openbox "${PRISMDRAKE_DESKTOP_OPENBOX_EXECUTABLE}"
                --xprop "${PRISMDRAKE_DESKTOP_XPROP_EXECUTABLE}"
                "${PRISMDRAKE_DESKTOP_XVFB_EXECUTABLE}"
                "$<TARGET_FILE:prismdrake-desktop-window-host-integration-tests>"
                --)
        set_tests_properties(DesktopWindowHostOpenboxIntegrationTest PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=xcb"
            TIMEOUT 30)
    else()
        add_test(
            NAME DesktopWindowHostOpenboxIntegrationTest
            COMMAND "${CMAKE_COMMAND}" -E echo
                "SKIP: Xvfb, Openbox, or xprop is unavailable; desktop WM integration requires all three")
        set_tests_properties(DesktopWindowHostOpenboxIntegrationTest PROPERTIES
            SKIP_REGULAR_EXPRESSION "^SKIP:"
            TIMEOUT 5)
    endif()
endif()
