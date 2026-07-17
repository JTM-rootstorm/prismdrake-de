find_package(Threads REQUIRED)

add_library(prismdrake-settings-service STATIC
    ServiceOptions.cpp
    ServiceOptions.hpp
    ServiceWorker.cpp
    ServiceWorker.hpp
    SettingsService.cpp
    SettingsService.hpp)
add_library(Prismdrake::settings-service ALIAS prismdrake-settings-service)

target_include_directories(prismdrake-settings-service
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-settings-service
    PUBLIC
        Prismdrake::foundation
        Prismdrake::settings-core
    PRIVATE
        Prismdrake::sd-bus
        Threads::Threads)

prismdrake_configure_cpp_target(prismdrake-settings-service)
prismdrake_enable_warnings(prismdrake-settings-service)
prismdrake_enable_sanitizers(prismdrake-settings-service)

add_executable(prismdrake-settingsd main.cpp)
target_link_libraries(prismdrake-settingsd PRIVATE Prismdrake::settings-service)
target_compile_definitions(prismdrake-settingsd
    PRIVATE
        PRISMDRAKE_PACKAGED_CONFIGURATION="${PROJECT_SOURCE_DIR}/data/defaults/config.toml"
        PRISMDRAKE_PACKAGED_THEME_DIRECTORY="${PROJECT_SOURCE_DIR}/themes")

prismdrake_configure_cpp_target(prismdrake-settingsd)
prismdrake_enable_warnings(prismdrake-settingsd)
prismdrake_enable_sanitizers(prismdrake-settingsd)
