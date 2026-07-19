add_library(prismdrake-icon-resolver STATIC
    IconRequest.hpp
    IconResolver.cpp
    IconResolver.hpp)
add_library(Prismdrake::icon-resolver ALIAS prismdrake-icon-resolver)

qt_add_resources(prismdrake-icon-resolver prismdrake_icons
    PREFIX "/org/prismdrake/icons"
    BASE assets
    FILES assets/prismdrake-generic.svg)

target_include_directories(prismdrake-icon-resolver
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-icon-resolver
    PUBLIC Prismdrake::foundation Prismdrake::launcher-model Qt6::Core Qt6::Gui Qt6::Svg)

prismdrake_configure_cpp_target(prismdrake-icon-resolver)
prismdrake_enable_warnings(prismdrake-icon-resolver)
prismdrake_enable_sanitizers(prismdrake-icon-resolver)
