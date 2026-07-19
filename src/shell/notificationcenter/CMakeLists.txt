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

add_library(prismdrake-notification-center-client STATIC
    NotificationCenterClient.cpp
    NotificationCenterClient.hpp)
add_library(Prismdrake::notification-center-client ALIAS
    prismdrake-notification-center-client)
set_target_properties(prismdrake-notification-center-client PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-notification-center-client
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-notification-center-client
    PUBLIC
        Prismdrake::notification-center-state
        Prismdrake::sd-bus
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-notification-center-client)
prismdrake_enable_warnings(prismdrake-notification-center-client)
prismdrake_enable_sanitizers(prismdrake-notification-center-client)

add_library(prismdrake-notification-center-presentation STATIC
    NotificationCenterPresentation.cpp
    NotificationCenterPresentation.hpp
    NotificationSurfaceIntents.cpp
    NotificationSurfaceIntents.hpp)
add_library(Prismdrake::notification-center-presentation ALIAS
    prismdrake-notification-center-presentation)
set_target_properties(prismdrake-notification-center-presentation PROPERTIES
    AUTOMOC ON)

target_include_directories(prismdrake-notification-center-presentation
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-notification-center-presentation
    PUBLIC
        Prismdrake::notification-center-state
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-notification-center-presentation)
prismdrake_enable_warnings(prismdrake-notification-center-presentation)
prismdrake_enable_sanitizers(prismdrake-notification-center-presentation)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(FATAL_ERROR
            "Notification-center tests require system-packaged GoogleTest.")
    endif()
    add_subdirectory(tests)
endif()
