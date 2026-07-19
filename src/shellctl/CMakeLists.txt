add_library(prismdrake-shellctl-core STATIC
    ShellCtl.cpp
    ShellCtl.hpp)
add_library(Prismdrake::shellctl-core ALIAS prismdrake-shellctl-core)

target_include_directories(prismdrake-shellctl-core
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shellctl-core
    PUBLIC Prismdrake::foundation
    PRIVATE Prismdrake::sd-bus)
prismdrake_configure_cpp_target(prismdrake-shellctl-core)
prismdrake_enable_warnings(prismdrake-shellctl-core)
prismdrake_enable_sanitizers(prismdrake-shellctl-core)

add_executable(prismdrake-shellctl main.cpp)
target_link_libraries(prismdrake-shellctl PRIVATE Prismdrake::shellctl-core)
prismdrake_configure_cpp_target(prismdrake-shellctl)
prismdrake_enable_warnings(prismdrake-shellctl)
prismdrake_enable_sanitizers(prismdrake-shellctl)

install(TARGETS prismdrake-shellctl
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime)
