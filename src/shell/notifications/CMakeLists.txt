add_library(prismdrake-shell-notification-presentation STATIC
    NotificationPresentationModel.cpp
    NotificationPresentationModel.hpp)
add_library(Prismdrake::shell-notification-presentation ALIAS
    prismdrake-shell-notification-presentation)
set_target_properties(prismdrake-shell-notification-presentation PROPERTIES
    AUTOMOC ON)

target_include_directories(prismdrake-shell-notification-presentation
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-notification-presentation
    PUBLIC
        Prismdrake::notification-model
        Qt6::Core)

add_subdirectory(qml)

prismdrake_configure_cpp_target(prismdrake-shell-notification-presentation)
prismdrake_enable_warnings(prismdrake-shell-notification-presentation)
prismdrake_enable_sanitizers(prismdrake-shell-notification-presentation)
