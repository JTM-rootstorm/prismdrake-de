include(GNUInstallDirs)

add_library(prismdrake-session-core STATIC
    ChildProcess.cpp
    ChildProcess.hpp
    RestartPolicy.cpp
    RestartPolicy.hpp
    SessionEnvironment.cpp
    SessionEnvironment.hpp
    SessionOptions.cpp
    SessionOptions.hpp
    SessionRuntime.cpp
    SessionRuntime.hpp
    SessionSupervisor.cpp
    SessionSupervisor.hpp
    SettingsReadiness.cpp
    SettingsReadiness.hpp)
add_library(Prismdrake::session-core ALIAS prismdrake-session-core)

target_include_directories(prismdrake-session-core
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-session-core
    PUBLIC Prismdrake::foundation
    PRIVATE Prismdrake::sd-bus)

prismdrake_configure_cpp_target(prismdrake-session-core)
prismdrake_enable_warnings(prismdrake-session-core)
prismdrake_enable_sanitizers(prismdrake-session-core)

add_executable(prismdrake-session main.cpp)
target_link_libraries(prismdrake-session
    PRIVATE
        Prismdrake::session-core
        Prismdrake::x11)
set(PRISMDRAKE_SESSION_DEFAULT_SETTINGSD_EXECUTABLE
    "${CMAKE_INSTALL_FULL_BINDIR}/prismdrake-settingsd"
    CACHE FILEPATH "Absolute packaged prismdrake-settingsd executable path")
set(PRISMDRAKE_SESSION_DEFAULT_SHELL_EXECUTABLE
    "${CMAKE_INSTALL_FULL_BINDIR}/prismdrake-shell"
    CACHE FILEPATH "Absolute packaged prismdrake-shell executable path")
target_compile_definitions(prismdrake-session
    PRIVATE
        "PRISMDRAKE_SESSION_DEFAULT_SETTINGSD_EXECUTABLE=\"${PRISMDRAKE_SESSION_DEFAULT_SETTINGSD_EXECUTABLE}\""
        "PRISMDRAKE_SESSION_DEFAULT_SHELL_EXECUTABLE=\"${PRISMDRAKE_SESSION_DEFAULT_SHELL_EXECUTABLE}\"")

prismdrake_configure_cpp_target(prismdrake-session)
prismdrake_enable_warnings(prismdrake-session)
prismdrake_enable_sanitizers(prismdrake-session)

install(
    TARGETS prismdrake-session
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_subdirectory(tests)
    endif()
endif()
