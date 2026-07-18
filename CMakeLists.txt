cmake_minimum_required(VERSION 3.24)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PRISMDRAKE_VERSION LIMIT_COUNT 1)
if(NOT PRISMDRAKE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR
        "VERSION must contain one semantic development version such as 0.1.0-dev; "
        "found '${PRISMDRAKE_VERSION}'")
endif()
set(PRISMDRAKE_PROJECT_VERSION
    "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

project(
    Prismdrake
    VERSION "${PRISMDRAKE_PROJECT_VERSION}"
    DESCRIPTION "Prismdrake Desktop Environment"
    HOMEPAGE_URL "https://github.com/JTM-rootstorm/prismdrake-de"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(GNUInstallDirs)
include(PrismdrakeOptions)
include(PrismdrakeCompiler)
include(PrismdrakeWarnings)
include(PrismdrakeSanitizers)
include(PrismdrakeFormatting)
include(PrismdrakeSdBus)

prismdrake_validate_toolchain()

add_library(prismdrake-cpp-policy INTERFACE)
add_library(Prismdrake::cpp-policy ALIAS prismdrake-cpp-policy)
target_compile_features(prismdrake-cpp-policy INTERFACE cxx_std_20)
set_target_properties(prismdrake-cpp-policy PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO)

add_subdirectory(src/foundation)
add_subdirectory(src/launcher)
add_subdirectory(src/notifications)

set(prismdrake_qt_components Core Gui Qml Quick QuickControls2)
if(BUILD_TESTING)
    list(APPEND prismdrake_qt_components QuickTest)
endif()
find_package(Qt6 6.11 CONFIG QUIET COMPONENTS ${prismdrake_qt_components})
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "Prismdrake shell presentation requires system-packaged Qt Core, GUI, QML, Quick, "
        "and Quick Controls 6.11 or newer; tests also require Quick Test. Install "
        ">=dev-qt/qtbase-6.11:6 and >=dev-qt/qtdeclarative-6.11:6 on Gentoo, "
        "or equivalent Qt 6.11-or-newer system packages on another supported host. "
        "Prismdrake does not download or vendor Qt.")
endif()
find_package(tomlplusplus CONFIG QUIET)
if(NOT tomlplusplus_FOUND)
    message(FATAL_ERROR
        "Prismdrake configuration support requires system-packaged toml++. Install "
        "dev-cpp/tomlplusplus on Gentoo or the equivalent development package. "
        "Prismdrake does not download or vendor TOML parsers.")
endif()
add_subdirectory(src/config)

find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    message(FATAL_ERROR
        "Prismdrake theme support requires system-packaged nlohmann-json. Install "
        "dev-cpp/nlohmann_json on Gentoo or the equivalent development package. "
        "Prismdrake does not download or vendor JSON parsers.")
endif()
add_subdirectory(src/theme)
add_subdirectory(src/settings)
add_subdirectory(src/ipc)
add_subdirectory(src/x11)
add_subdirectory(src/shell)
add_subdirectory(src/settingsd)
add_subdirectory(src/session)

configure_file(
    data/xsessions/prismdrake.desktop.in
    "${CMAKE_CURRENT_BINARY_DIR}/data/xsessions/prismdrake.desktop"
    @ONLY)

install(
    FILES data/defaults/config.toml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prismdrake/defaults"
    COMPONENT Runtime)
install(
    FILES
        themes/accessibility.tokens.json
        themes/base.tokens.json
        themes/forge.tokens.json
        themes/lustre.tokens.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prismdrake/themes"
    COMPONENT Runtime)
install(
    FILES
        schemas/prismdrake-config.schema.json
        schemas/prismdrake-runtime-snapshot.schema.json
        schemas/prismdrake-theme-tokens.schema.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/prismdrake/schemas"
    COMPONENT Runtime)
install(
    FILES interfaces/dbus/org.prismdrake.Settings1.xml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces"
    COMPONENT Runtime)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/data/xsessions/prismdrake.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/xsessions"
    COMPONENT Runtime)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(FATAL_ERROR
            "Prismdrake tests require system-packaged GoogleTest. Install the "
            "GoogleTest development package (dev-cpp/gtest on Gentoo), or configure "
            "with -DBUILD_TESTING=OFF. Prismdrake does not download test dependencies.")
    endif()
    add_subdirectory(tests)
endif()

prismdrake_add_format_check()
