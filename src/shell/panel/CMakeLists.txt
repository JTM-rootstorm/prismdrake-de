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

add_library(prismdrake-grouped-task-model STATIC
    GroupedTaskModel.cpp
    GroupedTaskModel.hpp)
add_library(Prismdrake::grouped-task-model ALIAS prismdrake-grouped-task-model)

target_include_directories(prismdrake-grouped-task-model
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-grouped-task-model
    PUBLIC
        Prismdrake::config
        Prismdrake::launcher-model
        Prismdrake::pin-state
        Prismdrake::x11)

prismdrake_configure_cpp_target(prismdrake-grouped-task-model)
prismdrake_enable_warnings(prismdrake-grouped-task-model)
prismdrake_enable_sanitizers(prismdrake-grouped-task-model)

add_library(prismdrake-grouped-task-presentation STATIC
    GroupedTaskPresentationModel.cpp
    GroupedTaskPresentationModel.hpp
    PanelClock.cpp
    PanelClock.hpp)
add_library(Prismdrake::grouped-task-presentation ALIAS
    prismdrake-grouped-task-presentation)
set_target_properties(prismdrake-grouped-task-presentation PROPERTIES AUTOMOC ON)

target_include_directories(prismdrake-grouped-task-presentation
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-grouped-task-presentation
    PUBLIC
        Prismdrake::grouped-task-model
        Qt6::Core)

prismdrake_configure_cpp_target(prismdrake-grouped-task-presentation)
prismdrake_enable_warnings(prismdrake-grouped-task-presentation)
prismdrake_enable_sanitizers(prismdrake-grouped-task-presentation)
