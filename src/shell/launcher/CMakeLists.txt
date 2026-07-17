add_library(prismdrake-shell-launcher-presentation STATIC
    LauncherPresentationModel.cpp
    LauncherPresentationModel.hpp)
add_library(Prismdrake::shell-launcher-presentation ALIAS
    prismdrake-shell-launcher-presentation)
set_target_properties(prismdrake-shell-launcher-presentation PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-shell-launcher-presentation
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-launcher-presentation
    PUBLIC
        Prismdrake::launcher-model
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-shell-launcher-presentation)
prismdrake_enable_warnings(prismdrake-shell-launcher-presentation)
prismdrake_enable_sanitizers(prismdrake-shell-launcher-presentation)
