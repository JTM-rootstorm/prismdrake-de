add_library(prismdrake-shell-accessibility STATIC
    ExpandedStateAccessibility.cpp
    ExpandedStateAccessibility.hpp)
add_library(Prismdrake::shell-accessibility ALIAS prismdrake-shell-accessibility)
set_target_properties(prismdrake-shell-accessibility PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-shell-accessibility
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-accessibility
    PUBLIC Qt6::Gui
    PRIVATE
        Qt6::Core
        Qt6::Quick
        Qt6::QuickPrivate
        Qt6::QuickTemplates2Private)

prismdrake_configure_cpp_target(prismdrake-shell-accessibility)
prismdrake_enable_warnings(prismdrake-shell-accessibility)
prismdrake_enable_sanitizers(prismdrake-shell-accessibility)
