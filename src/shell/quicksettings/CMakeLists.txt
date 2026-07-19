add_library(prismdrake-shell-quick-settings STATIC
    QuickSettingsModel.cpp
    QuickSettingsModel.hpp)
add_library(Prismdrake::shell-quick-settings ALIAS
    prismdrake-shell-quick-settings)
set_target_properties(prismdrake-shell-quick-settings PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-shell-quick-settings
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-quick-settings
    PUBLIC
        Prismdrake::settings-core
        Qt6::Core)

add_subdirectory(qml)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(FATAL_ERROR
            "Quick-settings tests require system-packaged GoogleTest.")
    endif()
    add_subdirectory(tests)
endif()

prismdrake_configure_cpp_target(prismdrake-shell-quick-settings)
prismdrake_enable_warnings(prismdrake-shell-quick-settings)
prismdrake_enable_sanitizers(prismdrake-shell-quick-settings)
