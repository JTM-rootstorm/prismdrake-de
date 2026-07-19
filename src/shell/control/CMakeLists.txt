add_library(prismdrake-shell-control STATIC
    ShellControlService.cpp
    ShellControlService.hpp)
add_library(Prismdrake::shell-control ALIAS prismdrake-shell-control)

target_include_directories(prismdrake-shell-control
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-control
    PUBLIC
        Prismdrake::foundation
        Prismdrake::sd-bus)

prismdrake_configure_cpp_target(prismdrake-shell-control)
prismdrake_enable_warnings(prismdrake-shell-control)
prismdrake_enable_sanitizers(prismdrake-shell-control)
