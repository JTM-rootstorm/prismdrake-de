add_library(prismdrake-pin-state STATIC PinState.cpp PinState.hpp)
add_library(Prismdrake::pin-state ALIAS prismdrake-pin-state)

target_include_directories(prismdrake-pin-state
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-pin-state
    PUBLIC Prismdrake::foundation Prismdrake::launcher-model
    PRIVATE nlohmann_json::nlohmann_json)

prismdrake_configure_cpp_target(prismdrake-pin-state)
prismdrake_enable_warnings(prismdrake-pin-state)
prismdrake_enable_sanitizers(prismdrake-pin-state)
