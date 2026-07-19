add_library(prismdrake-shell-runtime-state STATIC
    ShellRuntimeComposition.cpp
    ShellRuntimeComposition.hpp
    ShellRuntimeState.cpp
    ShellRuntimeState.hpp)
add_library(Prismdrake::shell-runtime-state ALIAS prismdrake-shell-runtime-state)
target_include_directories(prismdrake-shell-runtime-state
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
prismdrake_configure_cpp_target(prismdrake-shell-runtime-state)
target_link_libraries(prismdrake-shell-runtime-state
    PUBLIC
        Prismdrake::shell-composition
        Prismdrake::shell-control
        Prismdrake::output-surfaces
        Prismdrake::shell-quick-settings)
prismdrake_enable_warnings(prismdrake-shell-runtime-state)
prismdrake_enable_sanitizers(prismdrake-shell-runtime-state)

add_library(prismdrake-shell-runtime STATIC
    DevelopmentNotificationOwner.cpp
    DevelopmentNotificationOwner.hpp
    FatalDisplayShutdown.cpp
    FatalDisplayShutdown.hpp
    NotificationRuntimeAdapter.cpp
    NotificationRuntimeAdapter.hpp
    Pd2ShellRuntime.cpp
    Pd2ShellRuntime.hpp
    ShellControlBusOwner.cpp
    ShellControlBusOwner.hpp
    ShellRuntime.cpp
    ShellRuntime.hpp
    ShellRuntimeOptions.cpp
    ShellRuntimeOptions.hpp
    ShellOutputMonitor.cpp
    ShellOutputMonitor.hpp
    TranslationCatalog.cpp
    TranslationCatalog.hpp
    TerminationSignalBridge.cpp
    TerminationSignalBridge.hpp)
add_library(Prismdrake::shell-runtime ALIAS prismdrake-shell-runtime)
set_target_properties(prismdrake-shell-runtime PROPERTIES AUTOMOC ON)
target_include_directories(prismdrake-shell-runtime
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-shell-runtime
    PUBLIC
        Prismdrake::shell-runtime-state
        Prismdrake::async-pin-state
        Prismdrake::shell-launcher-controller
        Prismdrake::shell-notification-presentation
        Prismdrake::notification-center-client
        Prismdrake::notification-center-presentation
        Prismdrake::shell-control-clients
        Prismdrake::shell-desktop
        Prismdrake::grouped-task-presentation
        Prismdrake::shell-control
        Prismdrake::shell-panel-window
        Prismdrake::shell-settings-client
        Prismdrake::shell-task-controller
        Prismdrake::shell-theme
        Qt6::Core
    PRIVATE
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick)
if(PRISMDRAKE_ENABLE_SHELL_TEST_FIXTURES)
    target_compile_definitions(prismdrake-shell-runtime
        PRIVATE PRISMDRAKE_ENABLE_SHELL_TEST_FIXTURES=1)
endif()
if(PRISMDRAKE_USE_INSTALL_PATHS)
    set(prismdrake_translation_directory
        "${CMAKE_INSTALL_FULL_DATADIR}/prismdrake/translations")
else()
    set(prismdrake_translation_directory
        "${prismdrake_translation_build_directory}")
endif()
target_compile_definitions(prismdrake-shell-runtime
    PRIVATE PRISMDRAKE_TRANSLATION_DIRECTORY="${prismdrake_translation_directory}")
prismdrake_configure_cpp_target(prismdrake-shell-runtime)
prismdrake_enable_warnings(prismdrake-shell-runtime)
prismdrake_enable_sanitizers(prismdrake-shell-runtime)

add_executable(prismdrake-shell main.cpp)
add_dependencies(prismdrake-shell prismdrake-translations)
target_link_libraries(prismdrake-shell
    PRIVATE
        Prismdrake::shell-accessibility
        Prismdrake::shell-runtime
        Prismdrake::shell-launcher-qml
        Prismdrake::shell-notification-qml
        Prismdrake::shell-panel-qml
        Prismdrake::shell-quick-settings-qml
        Qt6::Gui
        Qt6::Quick)
prismdrake_configure_cpp_target(prismdrake-shell)
prismdrake_enable_warnings(prismdrake-shell)
prismdrake_enable_sanitizers(prismdrake-shell)
qt_finalize_executable(prismdrake-shell)

install(
    TARGETS prismdrake-shell
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime)

if(BUILD_TESTING)
    include(tests/CMakeLists.txt)
endif()
