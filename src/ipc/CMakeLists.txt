prismdrake_configure_sd_bus_provider()

add_library(prismdrake-sd-bus STATIC
    SdBusApi.cpp
    SdBusApi.hpp)
add_library(Prismdrake::sd-bus ALIAS prismdrake-sd-bus)

target_include_directories(prismdrake-sd-bus
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-sd-bus
    PUBLIC Prismdrake::sd-bus-provider)

prismdrake_configure_cpp_target(prismdrake-sd-bus)
prismdrake_enable_warnings(prismdrake-sd-bus)
prismdrake_enable_sanitizers(prismdrake-sd-bus)
