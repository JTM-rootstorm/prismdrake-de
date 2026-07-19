set(CMAKE_AUTOMOC ON)

qt_add_qml_module(prismdrake-shell-quick-settings-qml
    STATIC
    URI org.prismdrake.shell.quicksettings
    VERSION 0.1
    OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/qml/org/prismdrake/shell/quicksettings"
    RESOURCE_PREFIX /qt/qml
    NO_PLUGIN
    QML_FILES
        LogoutConfirmationSurface.qml
        QuickSettingsSurface.qml)
add_library(Prismdrake::shell-quick-settings-qml ALIAS
    prismdrake-shell-quick-settings-qml)

target_link_libraries(prismdrake-shell-quick-settings-qml
    PRIVATE
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2)

prismdrake_configure_cpp_target(prismdrake-shell-quick-settings-qml)
prismdrake_enable_warnings(prismdrake-shell-quick-settings-qml)
prismdrake_enable_sanitizers(prismdrake-shell-quick-settings-qml)
