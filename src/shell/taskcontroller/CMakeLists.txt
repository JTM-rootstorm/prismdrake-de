add_library(prismdrake-shell-task-controller STATIC
    TaskController.cpp
    TaskController.hpp
    TaskControllerCore.cpp
    TaskControllerCore.hpp)
add_library(Prismdrake::shell-task-controller ALIAS
    prismdrake-shell-task-controller)

target_include_directories(prismdrake-shell-task-controller
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-task-controller
    PUBLIC
        Prismdrake::shell-task-presentation
        Prismdrake::x11
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-shell-task-controller)
prismdrake_enable_warnings(prismdrake-shell-task-controller)
prismdrake_enable_sanitizers(prismdrake-shell-task-controller)
