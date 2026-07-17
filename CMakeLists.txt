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
include(PrismdrakeOptions)
include(PrismdrakeCompiler)
include(PrismdrakeWarnings)
include(PrismdrakeSanitizers)
include(PrismdrakeFormatting)

prismdrake_validate_toolchain()

add_library(prismdrake-cpp-policy INTERFACE)
add_library(Prismdrake::cpp-policy ALIAS prismdrake-cpp-policy)
target_compile_features(prismdrake-cpp-policy INTERFACE cxx_std_20)
set_target_properties(prismdrake-cpp-policy PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO)

add_subdirectory(src/foundation)

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

prismdrake_add_format_check(
    src/foundation/BuildInfo.cpp
    src/foundation/BuildInfo.hpp
    src/foundation/BuildInfoConfig.hpp.in
    tests/unit/BuildInfoTests.cpp)
