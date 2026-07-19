add_library(prismdrake-notification-authority STATIC
    NotificationAuthority.cpp
    NotificationAuthority.hpp
    NotificationHistory.cpp
    NotificationHistory.hpp
    NotificationStateSerializer.cpp
    NotificationStateSerializer.hpp)
add_library(Prismdrake::notification-authority ALIAS prismdrake-notification-authority)

target_include_directories(prismdrake-notification-authority
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-notification-authority
    PUBLIC
        Prismdrake::foundation
        Prismdrake::notification-model
    PRIVATE nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-notification-authority)
prismdrake_enable_warnings(prismdrake-notification-authority)
prismdrake_enable_sanitizers(prismdrake-notification-authority)
