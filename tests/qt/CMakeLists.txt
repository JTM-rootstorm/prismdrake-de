add_executable(prismdrake-shell-notification-presentation-tests
    NotificationPresentationModelTests.cpp)

target_link_libraries(prismdrake-shell-notification-presentation-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::shell-notification-presentation)

prismdrake_configure_cpp_target(prismdrake-shell-notification-presentation-tests)
prismdrake_enable_warnings(prismdrake-shell-notification-presentation-tests)
prismdrake_enable_sanitizers(prismdrake-shell-notification-presentation-tests)

include(GoogleTest)
gtest_discover_tests(prismdrake-shell-notification-presentation-tests
    DISCOVERY_MODE PRE_TEST)

add_executable(prismdrake-shell-theme-snapshot-tests
    ShellThemeSnapshotAdapterTests.cpp)
target_compile_definitions(prismdrake-shell-theme-snapshot-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(prismdrake-shell-theme-snapshot-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::settings-core
        Prismdrake::shell-theme)

prismdrake_configure_cpp_target(prismdrake-shell-theme-snapshot-tests)
prismdrake_enable_warnings(prismdrake-shell-theme-snapshot-tests)
prismdrake_enable_sanitizers(prismdrake-shell-theme-snapshot-tests)
gtest_discover_tests(prismdrake-shell-theme-snapshot-tests
    DISCOVERY_MODE PRE_TEST)

add_executable(prismdrake-shell-launcher-presentation-tests
    LauncherPresentationModelTests.cpp)
target_link_libraries(prismdrake-shell-launcher-presentation-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::shell-launcher-presentation
        Qt6::Test)

prismdrake_configure_cpp_target(prismdrake-shell-launcher-presentation-tests)
prismdrake_enable_warnings(prismdrake-shell-launcher-presentation-tests)
prismdrake_enable_sanitizers(prismdrake-shell-launcher-presentation-tests)
gtest_discover_tests(prismdrake-shell-launcher-presentation-tests
    DISCOVERY_MODE PRE_TEST)

add_executable(prismdrake-shell-task-presentation-tests
    TaskPresentationModelTests.cpp)
target_link_libraries(prismdrake-shell-task-presentation-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::shell-task-presentation)

prismdrake_configure_cpp_target(prismdrake-shell-task-presentation-tests)
prismdrake_enable_warnings(prismdrake-shell-task-presentation-tests)
prismdrake_enable_sanitizers(prismdrake-shell-task-presentation-tests)
gtest_discover_tests(prismdrake-shell-task-presentation-tests
    DISCOVERY_MODE PRE_TEST)

add_test(NAME NotificationCardQmlTest
    COMMAND $<TARGET_FILE:Qt6::qmltestrunner>
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml")
set_tests_properties(NotificationCardQmlTest PROPERTIES
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")

add_executable(prismdrake-notification-list-qml-tests
    NotificationListQmlFixture.cpp
    NotificationListQmlFixture.hpp
    NotificationListQmlTestRunner.cpp)
set_target_properties(prismdrake-notification-list-qml-tests PROPERTIES AUTOMOC ON)

target_link_libraries(prismdrake-notification-list-qml-tests
    PRIVATE
        Prismdrake::shell-notification-presentation
        Qt6::Qml
        Qt6::QuickTest)

prismdrake_configure_cpp_target(prismdrake-notification-list-qml-tests)
prismdrake_enable_warnings(prismdrake-notification-list-qml-tests)
prismdrake_enable_sanitizers(prismdrake-notification-list-qml-tests)

add_test(NAME NotificationListQmlTest
    COMMAND prismdrake-notification-list-qml-tests
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml-real")
set_tests_properties(NotificationListQmlTest PROPERTIES
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")

add_executable(prismdrake-panel-surface-qml-tests
    PanelSurfaceQmlFixture.cpp
    PanelSurfaceQmlFixture.hpp
    PanelSurfaceQmlTestRunner.cpp)
set_target_properties(prismdrake-panel-surface-qml-tests PROPERTIES AUTOMOC ON)
target_compile_definitions(prismdrake-panel-surface-qml-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

target_link_libraries(prismdrake-panel-surface-qml-tests
    PRIVATE
        Prismdrake::settings-core
        Prismdrake::shell-task-presentation
        Prismdrake::shell-theme
        Prismdrake::shell-panel-qml
        Qt6::Qml
        Qt6::QuickTest)

prismdrake_configure_cpp_target(prismdrake-panel-surface-qml-tests)
prismdrake_enable_warnings(prismdrake-panel-surface-qml-tests)
prismdrake_enable_sanitizers(prismdrake-panel-surface-qml-tests)

add_test(NAME PanelSurfaceQmlTest
    COMMAND prismdrake-panel-surface-qml-tests
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml-panel")
set_tests_properties(PanelSurfaceQmlTest PROPERTIES
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")

add_subdirectory(window)
add_subdirectory(taskcontroller)
