add_library(prismdrake-theme STATIC
    Theme.hpp
    ThemeParser.cpp
    ThemeParser.hpp
    ThemeResolver.cpp
    ThemeResolver.hpp)
add_library(Prismdrake::theme ALIAS prismdrake-theme)

target_include_directories(prismdrake-theme
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/config"
        "${PROJECT_SOURCE_DIR}/src/foundation")
target_link_libraries(prismdrake-theme
    PUBLIC
        Prismdrake::config
        Prismdrake::foundation
    PRIVATE
        nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-theme)
prismdrake_enable_warnings(prismdrake-theme)
prismdrake_enable_sanitizers(prismdrake-theme)
