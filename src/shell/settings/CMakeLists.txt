add_library(prismdrake-runtime-snapshot-parser STATIC
    RuntimeSnapshotParser.cpp
    RuntimeSnapshotParser.hpp)
add_library(Prismdrake::runtime-snapshot-parser ALIAS
    prismdrake-runtime-snapshot-parser)

target_include_directories(prismdrake-runtime-snapshot-parser
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-runtime-snapshot-parser
    PUBLIC Prismdrake::settings-core
    PRIVATE nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-runtime-snapshot-parser)
prismdrake_enable_warnings(prismdrake-runtime-snapshot-parser)
prismdrake_enable_sanitizers(prismdrake-runtime-snapshot-parser)

add_library(prismdrake-shell-settings-client STATIC
    SettingsSnapshotClient.cpp
    SettingsSnapshotClient.hpp)
add_library(Prismdrake::shell-settings-client ALIAS
    prismdrake-shell-settings-client)
set_target_properties(prismdrake-shell-settings-client PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-shell-settings-client
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-settings-client
    PUBLIC
        Prismdrake::runtime-snapshot-parser
        Prismdrake::sd-bus
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-shell-settings-client)
prismdrake_enable_warnings(prismdrake-shell-settings-client)
prismdrake_enable_sanitizers(prismdrake-shell-settings-client)
