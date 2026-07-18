add_executable(prismdrake-launcher-surface-qml-tests
    LauncherSurfaceQmlFixture.cpp
    LauncherSurfaceQmlFixture.hpp
    LauncherSurfaceQmlTestRunner.cpp)
set_target_properties(prismdrake-launcher-surface-qml-tests PROPERTIES AUTOMOC ON)
target_compile_definitions(prismdrake-launcher-surface-qml-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

target_link_libraries(prismdrake-launcher-surface-qml-tests
    PRIVATE
        Prismdrake::settings-core
        Prismdrake::shell-launcher-presentation
        Prismdrake::shell-launcher-qml
        Prismdrake::shell-theme
        Qt6::Qml
        Qt6::QuickTest)

prismdrake_configure_cpp_target(prismdrake-launcher-surface-qml-tests)
prismdrake_enable_warnings(prismdrake-launcher-surface-qml-tests)
prismdrake_enable_sanitizers(prismdrake-launcher-surface-qml-tests)

add_test(NAME LauncherSurfaceQmlTest
    COMMAND prismdrake-launcher-surface-qml-tests
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml")
set_tests_properties(LauncherSurfaceQmlTest PROPERTIES
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
