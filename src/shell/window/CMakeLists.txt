add_library(prismdrake-shell-panel-window STATIC
    PanelWindowController.cpp
    PanelWindowController.hpp
    PanelWindowHost.cpp
    PanelWindowHost.hpp)
add_library(Prismdrake::shell-panel-window ALIAS prismdrake-shell-panel-window)

target_include_directories(prismdrake-shell-panel-window
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-panel-window
    PUBLIC
        Prismdrake::x11
        Qt6::Core
        Qt6::Gui)

prismdrake_configure_cpp_target(prismdrake-shell-panel-window)
prismdrake_enable_warnings(prismdrake-shell-panel-window)
prismdrake_enable_sanitizers(prismdrake-shell-panel-window)
