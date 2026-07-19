add_library(prismdrake-notification-center-state STATIC
    NotificationCenterState.hpp
    NotificationCenterStateParser.cpp
    NotificationCenterStateParser.hpp)
add_library(Prismdrake::notification-center-state ALIAS
    prismdrake-notification-center-state)

target_include_directories(prismdrake-notification-center-state
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-notification-center-state
    PUBLIC
        Prismdrake::foundation
        Prismdrake::notification-model
    PRIVATE nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-notification-center-state)
prismdrake_enable_warnings(prismdrake-notification-center-state)
prismdrake_enable_sanitizers(prismdrake-notification-center-state)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
