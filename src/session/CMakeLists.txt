add_library(prismdrake-session-core STATIC
    RestartPolicy.cpp
    RestartPolicy.hpp
    SessionEnvironment.cpp
    SessionEnvironment.hpp
    SettingsReadiness.cpp
    SettingsReadiness.hpp)
add_library(Prismdrake::session-core ALIAS prismdrake-session-core)

target_include_directories(prismdrake-session-core
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-session-core
    PUBLIC Prismdrake::foundation
    PRIVATE Prismdrake::sd-bus)

prismdrake_configure_cpp_target(prismdrake-session-core)
prismdrake_enable_warnings(prismdrake-session-core)
prismdrake_enable_sanitizers(prismdrake-session-core)
