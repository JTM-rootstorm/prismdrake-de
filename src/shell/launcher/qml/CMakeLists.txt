set(CMAKE_AUTOMOC ON)

qt_add_qml_module(prismdrake-shell-launcher-qml
    STATIC
    URI org.prismdrake.shell.launcher
    VERSION 0.1
    OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/qml/org/prismdrake/shell/launcher"
    RESOURCE_PREFIX /qt/qml
    NO_PLUGIN
    QML_FILES
        LauncherResultButton.qml
        LauncherSurface.qml)
add_library(Prismdrake::shell-launcher-qml ALIAS prismdrake-shell-launcher-qml)

target_link_libraries(prismdrake-shell-launcher-qml
    PRIVATE
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2)

prismdrake_configure_cpp_target(prismdrake-shell-launcher-qml)
prismdrake_enable_warnings(prismdrake-shell-launcher-qml)
prismdrake_enable_sanitizers(prismdrake-shell-launcher-qml)
