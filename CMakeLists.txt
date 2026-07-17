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

find_package(tomlplusplus CONFIG QUIET)
if(NOT tomlplusplus_FOUND)
    message(FATAL_ERROR
        "Prismdrake configuration support requires system-packaged toml++. Install "
        "dev-cpp/tomlplusplus on Gentoo or the equivalent development package. "
        "Prismdrake does not download or vendor TOML parsers.")
endif()
add_subdirectory(src/config)

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
    src/config/Configuration.hpp
    src/config/ConfigurationLoader.cpp
    src/config/ConfigurationLoader.hpp
    src/config/ConfigurationParser.cpp
    src/config/ConfigurationParser.hpp
    src/foundation/AtomicFile.cpp
    src/foundation/AtomicFile.hpp
    src/foundation/BoundedFile.cpp
    src/foundation/BoundedFile.hpp
    src/foundation/BuildInfo.cpp
    src/foundation/BuildInfo.hpp
    src/foundation/BuildInfoConfig.hpp.in
    src/foundation/Cancellation.hpp
    src/foundation/Diagnostics.cpp
    src/foundation/Diagnostics.hpp
    src/foundation/ExitStatus.hpp
    src/foundation/Generation.hpp
    src/foundation/MonotonicClock.hpp
    src/foundation/Result.hpp
    src/foundation/XdgPaths.cpp
    src/foundation/XdgPaths.hpp
    tests/unit/AtomicFileTests.cpp
    tests/unit/BoundedFileTests.cpp
    tests/unit/BuildInfoTests.cpp
    tests/unit/ConfigurationParserTests.cpp
    tests/unit/ConfigurationLoaderTests.cpp
    tests/unit/DiagnosticsTests.cpp
    tests/unit/FoundationPrimitivesTests.cpp
    tests/unit/ResultTests.cpp
    tests/unit/XdgPathsTests.cpp)
