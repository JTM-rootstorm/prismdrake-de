add_library(prismdrake-wallpaper-core STATIC
    WallpaperLayout.cpp
    WallpaperLayout.hpp
    WallpaperModel.cpp
    WallpaperModel.hpp)
add_library(Prismdrake::wallpaper-core ALIAS prismdrake-wallpaper-core)

target_include_directories(prismdrake-wallpaper-core
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-wallpaper-core
    PUBLIC Prismdrake::config Prismdrake::foundation Qt6::Core Qt6::Gui)

prismdrake_configure_cpp_target(prismdrake-wallpaper-core)
prismdrake_enable_warnings(prismdrake-wallpaper-core)
prismdrake_enable_sanitizers(prismdrake-wallpaper-core)
