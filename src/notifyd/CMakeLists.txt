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

add_library(prismdrake-notification-service STATIC
    NotificationService.cpp
    NotificationService.hpp
    ServiceOptions.cpp
    ServiceOptions.hpp)
add_library(Prismdrake::notification-service ALIAS prismdrake-notification-service)

target_include_directories(prismdrake-notification-service
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-notification-service
    PUBLIC Prismdrake::notification-authority
    PRIVATE
        Prismdrake::sd-bus
        nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-notification-service)
prismdrake_enable_warnings(prismdrake-notification-service)
prismdrake_enable_sanitizers(prismdrake-notification-service)

add_executable(prismdrake-notifyd main.cpp)
target_link_libraries(prismdrake-notifyd PRIVATE Prismdrake::notification-service)

prismdrake_configure_cpp_target(prismdrake-notifyd)
prismdrake_enable_warnings(prismdrake-notifyd)
prismdrake_enable_sanitizers(prismdrake-notifyd)

install(
    TARGETS prismdrake-notifyd
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime)
