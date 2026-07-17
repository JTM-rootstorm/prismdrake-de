include(GoogleTest)
find_package(Threads REQUIRED)

add_executable(prismdrake-foundation-tests
    AtomicFileTests.cpp
    BoundedFileTests.cpp
    BuildInfoTests.cpp
    ConfigurationLoaderTests.cpp
    ConfigurationParserTests.cpp
    DiagnosticsTests.cpp
    FoundationPrimitivesTests.cpp
    ResultTests.cpp
    RestartPolicyTests.cpp
    SessionEnvironmentTests.cpp
    ServiceOptionsTests.cpp
    ServiceWorkerTests.cpp
    SettingsEngineTests.cpp
    ThemeParserTests.cpp
    ThemeResolverTests.cpp
    XdgPathsTests.cpp)
target_link_libraries(prismdrake-foundation-tests
    PRIVATE
        Prismdrake::foundation
        Prismdrake::session-core
        Prismdrake::settings-service
        Prismdrake::settings-core
        Prismdrake::config
        Prismdrake::theme
        GTest::gtest_main
        Threads::Threads)
target_compile_definitions(prismdrake-foundation-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    PRISMDRAKE_TEST_DEVELOPER_OVERRIDES=$<BOOL:${PRISMDRAKE_ENABLE_DEVELOPER_OVERRIDES}>
    PRISMDRAKE_TEST_VERSION="${PRISMDRAKE_VERSION}")

prismdrake_configure_cpp_target(prismdrake-foundation-tests)
prismdrake_enable_warnings(prismdrake-foundation-tests)
prismdrake_enable_sanitizers(prismdrake-foundation-tests)

gtest_discover_tests(prismdrake-foundation-tests DISCOVERY_MODE PRE_TEST)
