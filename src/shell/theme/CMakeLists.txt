add_library(prismdrake-shell-theme STATIC
    ShellThemeSnapshotAdapter.cpp
    ShellThemeSnapshotAdapter.hpp)
add_library(Prismdrake::shell-theme ALIAS prismdrake-shell-theme)
set_target_properties(prismdrake-shell-theme PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-shell-theme
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-theme
    PUBLIC
        Prismdrake::settings-core
        Qt6::Core
        Qt6::Gui)

prismdrake_configure_cpp_target(prismdrake-shell-theme)
prismdrake_enable_warnings(prismdrake-shell-theme)
prismdrake_enable_sanitizers(prismdrake-shell-theme)
