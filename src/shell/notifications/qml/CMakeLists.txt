set(CMAKE_AUTOMOC ON)

qt_add_qml_module(prismdrake-shell-notification-qml
    STATIC
    URI org.prismdrake.shell.notifications
    OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/qml/org/prismdrake/shell/notifications"
    RESOURCE_PREFIX /qt/qml
    NO_PLUGIN
    QML_FILES
        NotificationCard.qml
        NotificationList.qml
        NotificationPresentationTokens.qml)
add_library(Prismdrake::shell-notification-qml ALIAS
    prismdrake-shell-notification-qml)
target_link_libraries(prismdrake-shell-notification-qml
    PRIVATE
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2)

prismdrake_configure_cpp_target(prismdrake-shell-notification-qml)
prismdrake_enable_warnings(prismdrake-shell-notification-qml)
prismdrake_enable_sanitizers(prismdrake-shell-notification-qml)
