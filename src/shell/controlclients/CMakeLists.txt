add_library(prismdrake-shell-control-clients STATIC
    QtSessionBusDispatcher.cpp
    QtSessionBusDispatcher.hpp
    SessionControlClient.cpp
    SessionControlClient.hpp
    SettingsProfileControlClient.cpp
    SettingsProfileControlClient.hpp)
add_library(Prismdrake::shell-control-clients ALIAS
    prismdrake-shell-control-clients)
set_target_properties(prismdrake-shell-control-clients PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-shell-control-clients
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-control-clients
    PUBLIC
        Prismdrake::shell-quick-settings
        Prismdrake::sd-bus
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-shell-control-clients)
prismdrake_enable_warnings(prismdrake-shell-control-clients)
prismdrake_enable_sanitizers(prismdrake-shell-control-clients)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(FATAL_ERROR
            "Shell control-client tests require system-packaged GoogleTest.")
    endif()
    add_subdirectory(tests)
endif()
