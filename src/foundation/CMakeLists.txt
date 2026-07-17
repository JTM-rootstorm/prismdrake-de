set(prismdrake_foundation_generated_dir
    "${CMAKE_CURRENT_BINARY_DIR}/generated/prismdrake/foundation")

configure_file(
    BuildInfoConfig.hpp.in
    "${prismdrake_foundation_generated_dir}/BuildInfoConfig.hpp"
    @ONLY)

add_library(prismdrake-foundation STATIC
    AtomicFile.cpp
    AtomicFile.hpp
    BoundedFile.cpp
    BoundedFile.hpp
    BuildInfo.cpp
    BuildInfo.hpp
    Cancellation.hpp
    Diagnostics.cpp
    Diagnostics.hpp
    ExitStatus.hpp
    Generation.hpp
    MonotonicClock.hpp
    Result.hpp
    XdgPaths.cpp
    XdgPaths.hpp
    "${prismdrake_foundation_generated_dir}/BuildInfoConfig.hpp")
add_library(Prismdrake::foundation ALIAS prismdrake-foundation)

target_include_directories(prismdrake-foundation
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")

prismdrake_configure_cpp_target(prismdrake-foundation)
prismdrake_enable_warnings(prismdrake-foundation)
prismdrake_enable_sanitizers(prismdrake-foundation)
