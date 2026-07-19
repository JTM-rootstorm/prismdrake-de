add_library(prismdrake-shell-composition STATIC
    ShellComposition.cpp
)

add_library(Prismdrake::shell-composition ALIAS prismdrake-shell-composition)

target_include_directories(prismdrake-shell-composition
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

target_link_libraries(prismdrake-shell-composition
    PUBLIC
        Prismdrake::x11
)

prismdrake_configure_cpp_target(prismdrake-shell-composition)
prismdrake_enable_warnings(prismdrake-shell-composition)
prismdrake_enable_sanitizers(prismdrake-shell-composition)
