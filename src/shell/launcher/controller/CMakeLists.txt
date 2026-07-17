add_library(prismdrake-shell-launcher-controller STATIC
    LauncherController.cpp
    LauncherController.hpp)
add_library(Prismdrake::shell-launcher-controller ALIAS
    prismdrake-shell-launcher-controller)
set_target_properties(prismdrake-shell-launcher-controller PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-shell-launcher-controller
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-launcher-controller
    PUBLIC
        Prismdrake::shell-launcher-presentation
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-shell-launcher-controller)
prismdrake_enable_warnings(prismdrake-shell-launcher-controller)
prismdrake_enable_sanitizers(prismdrake-shell-launcher-controller)
