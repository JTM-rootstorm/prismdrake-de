add_library(prismdrake-config STATIC
    Configuration.hpp
    ConfigurationLoader.cpp
    ConfigurationLoader.hpp
    ConfigurationParser.cpp
    ConfigurationParser.hpp)
add_library(Prismdrake::config ALIAS prismdrake-config)

target_include_directories(prismdrake-config
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/foundation")
target_link_libraries(prismdrake-config
    PUBLIC
        Prismdrake::foundation
    PRIVATE
        tomlplusplus::tomlplusplus)

prismdrake_configure_cpp_target(prismdrake-config)
prismdrake_enable_warnings(prismdrake-config)
prismdrake_enable_sanitizers(prismdrake-config)
