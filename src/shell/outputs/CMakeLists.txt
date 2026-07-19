add_library(prismdrake-output-surfaces STATIC
    OutputSurfaceRegistry.cpp
    OutputSurfaceRegistry.hpp
    ScreenReconciliation.cpp
    ScreenReconciliation.hpp)
add_library(Prismdrake::output-surfaces ALIAS prismdrake-output-surfaces)

target_include_directories(prismdrake-output-surfaces
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-output-surfaces
    PUBLIC Prismdrake::foundation Prismdrake::x11)

prismdrake_configure_cpp_target(prismdrake-output-surfaces)
prismdrake_enable_warnings(prismdrake-output-surfaces)
prismdrake_enable_sanitizers(prismdrake-output-surfaces)
