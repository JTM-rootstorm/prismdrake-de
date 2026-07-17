add_library(prismdrake-settings-core STATIC
    RuntimeSnapshot.cpp
    RuntimeSnapshot.hpp
    SettingsEngine.cpp
    SettingsEngine.hpp
    SettingsSnapshot.cpp
    SettingsSnapshot.hpp)
add_library(Prismdrake::settings-core ALIAS prismdrake-settings-core)

target_include_directories(prismdrake-settings-core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/config"
        "${PROJECT_SOURCE_DIR}/src/foundation"
        "${PROJECT_SOURCE_DIR}/src/theme")
target_link_libraries(prismdrake-settings-core
    PUBLIC
        Prismdrake::config
        Prismdrake::foundation
        Prismdrake::theme)
target_link_libraries(prismdrake-settings-core PRIVATE nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-settings-core)
prismdrake_enable_warnings(prismdrake-settings-core)
prismdrake_enable_sanitizers(prismdrake-settings-core)
