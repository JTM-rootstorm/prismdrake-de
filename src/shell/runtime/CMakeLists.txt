add_library(prismdrake-shell-runtime-composition STATIC
    SingletonTaskPolicyGate.cpp
    SingletonTaskPolicyGate.hpp
    ShellReadinessGate.cpp
    ShellReadinessGate.hpp
    ShellRuntimeComposition.cpp
    ShellRuntimeComposition.hpp)
add_library(Prismdrake::shell-runtime-composition ALIAS
    prismdrake-shell-runtime-composition)
target_include_directories(prismdrake-shell-runtime-composition
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
prismdrake_configure_cpp_target(prismdrake-shell-runtime-composition)
target_link_libraries(prismdrake-shell-runtime-composition
    PUBLIC
        Prismdrake::shell-composition
        Prismdrake::shell-control
        Prismdrake::output-surfaces
        Prismdrake::shell-quick-settings)
prismdrake_enable_warnings(prismdrake-shell-runtime-composition)
prismdrake_enable_sanitizers(prismdrake-shell-runtime-composition)

add_library(prismdrake-shell-runtime STATIC
    FatalDisplayShutdown.cpp
    FatalDisplayShutdown.hpp
    NotificationRuntimeAdapter.cpp
    NotificationRuntimeAdapter.hpp
    Pd2ShellRuntime.cpp
    Pd2ShellRuntime.hpp
    ShellControlBusOwner.cpp
    ShellControlBusOwner.hpp
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
        Prismdrake::shell-runtime-composition
        Prismdrake::async-pin-state
        Prismdrake::shell-launcher-controller
        Prismdrake::notification-center-client
        Prismdrake::notification-center-presentation
        Prismdrake::shell-control-clients
        Prismdrake::shell-desktop
        Prismdrake::grouped-task-presentation
        Prismdrake::shell-control
        Prismdrake::shell-panel-window
        Prismdrake::shell-x11-property-worker
        Prismdrake::shell-settings-client
        Prismdrake::shell-task-controller
        Prismdrake::shell-theme
        Qt6::Core
    PRIVATE
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick)
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
