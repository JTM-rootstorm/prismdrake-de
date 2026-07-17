set(CMAKE_AUTOMOC ON)

qt_add_qml_module(prismdrake-shell-panel-qml
    STATIC
    URI org.prismdrake.shell.panel
    VERSION 0.1
    OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/qml/org/prismdrake/shell/panel"
    RESOURCE_PREFIX /qt/qml
    NO_PLUGIN
    QML_FILES
        PanelSurface.qml
        TaskButton.qml)
add_library(Prismdrake::shell-panel-qml ALIAS prismdrake-shell-panel-qml)

target_link_libraries(prismdrake-shell-panel-qml
    PRIVATE
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2)

prismdrake_configure_cpp_target(prismdrake-shell-panel-qml)
prismdrake_enable_warnings(prismdrake-shell-panel-qml)
prismdrake_enable_sanitizers(prismdrake-shell-panel-qml)
