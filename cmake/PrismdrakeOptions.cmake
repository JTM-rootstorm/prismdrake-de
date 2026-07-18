include_guard(GLOBAL)

option(PRISMDRAKE_ENABLE_ASAN "Enable AddressSanitizer on project-owned targets" OFF)
option(PRISMDRAKE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer on project-owned targets" OFF)
option(PRISMDRAKE_ENABLE_LTO "Enable interprocedural optimization on project-owned targets" OFF)
option(PRISMDRAKE_ENABLE_CLANG_TIDY "Run Clang-Tidy on project-owned targets" OFF)
option(PRISMDRAKE_WARNINGS_AS_ERRORS "Treat project-owned target warnings as errors" OFF)
option(PRISMDRAKE_ENABLE_DEVELOPER_OVERRIDES
    "Enable non-production diagnostics and mock-capability overrides" OFF)
option(PRISMDRAKE_USE_INSTALL_PATHS
    "Embed configured install locations for packaged Prismdrake runtime data" OFF)

set(PRISMDRAKE_CLANG_FORMAT_EXECUTABLE "" CACHE FILEPATH
    "Path to the clang-format executable used by the format-check target")
set(PRISMDRAKE_CLANG_TIDY_EXECUTABLE "" CACHE FILEPATH
    "Path to the clang-tidy executable used when Clang-Tidy is enabled")
