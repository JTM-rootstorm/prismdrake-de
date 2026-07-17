add_library(prismdrake-notification-model STATIC
    NotificationModel.cpp
    NotificationModel.hpp)
add_library(Prismdrake::notification-model ALIAS prismdrake-notification-model)

target_include_directories(prismdrake-notification-model
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-notification-model PUBLIC Prismdrake::foundation)

prismdrake_configure_cpp_target(prismdrake-notification-model)
prismdrake_enable_warnings(prismdrake-notification-model)
prismdrake_enable_sanitizers(prismdrake-notification-model)
