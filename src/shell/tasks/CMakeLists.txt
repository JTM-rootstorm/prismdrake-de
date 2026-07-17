add_library(prismdrake-shell-task-presentation STATIC
    TaskPresentationModel.cpp
    TaskPresentationModel.hpp)
add_library(Prismdrake::shell-task-presentation ALIAS
    prismdrake-shell-task-presentation)
set_target_properties(prismdrake-shell-task-presentation PROPERTIES
    AUTOMOC ON)

target_include_directories(prismdrake-shell-task-presentation
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-task-presentation
    PUBLIC
        Prismdrake::x11
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-shell-task-presentation)
prismdrake_enable_warnings(prismdrake-shell-task-presentation)
prismdrake_enable_sanitizers(prismdrake-shell-task-presentation)
