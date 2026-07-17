add_library(prismdrake-launcher-model STATIC
    ApplicationPaths.cpp
    ApplicationPaths.hpp
    DesktopEntry.hpp
    DesktopEntryParser.cpp
    DesktopEntryParser.hpp
    DesktopEntryVisibility.cpp
    DesktopEntryVisibility.hpp
    DesktopFileId.cpp
    DesktopFileId.hpp)
add_library(Prismdrake::launcher-model ALIAS prismdrake-launcher-model)

target_include_directories(prismdrake-launcher-model
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-launcher-model PUBLIC Prismdrake::foundation)

prismdrake_configure_cpp_target(prismdrake-launcher-model)
prismdrake_enable_warnings(prismdrake-launcher-model)
prismdrake_enable_sanitizers(prismdrake-launcher-model)
