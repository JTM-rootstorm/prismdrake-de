include(GoogleTest)
find_package(Threads REQUIRED)

add_executable(prismdrake-foundation-tests
    AtomicFileTests.cpp
    BoundedFileTests.cpp
    BuildInfoTests.cpp
    ChildProcessTests.cpp
    ConfigurationLoaderTests.cpp
    ConfigurationParserTests.cpp
    DiagnosticsTests.cpp
    DockPropertiesTests.cpp
    EwmhTaskListTests.cpp
    EwmhWindowRequestsTests.cpp
    FoundationPrimitivesTests.cpp
    OutputTopologyTests.cpp
    PanelStrutGeometryTests.cpp
    PropertyReaderTests.cpp
    RandrTopologyTests.cpp
    ResultTests.cpp
    RootEventStreamTests.cpp
    RestartPolicyTests.cpp
    SessionEnvironmentTests.cpp
    SessionOptionsTests.cpp
    SessionRuntimeTests.cpp
    SettingsReadinessTests.cpp
    ServiceOptionsTests.cpp
    ServiceWorkerTests.cpp
    SettingsEngineTests.cpp
    TaskModelTests.cpp
    TaskRequestStateTests.cpp
    ThemeParserTests.cpp
    ThemeResolverTests.cpp
    WindowMetadataTests.cpp
    X11ConnectionTests.cpp
    XdgPathsTests.cpp)
target_link_libraries(prismdrake-foundation-tests
    PRIVATE
        Prismdrake::foundation
        Prismdrake::session-core
        Prismdrake::settings-service
        Prismdrake::settings-core
        Prismdrake::config
        Prismdrake::theme
        Prismdrake::x11
        GTest::gtest_main
        Threads::Threads)
target_compile_definitions(prismdrake-foundation-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    PRISMDRAKE_TEST_DEVELOPER_OVERRIDES=$<BOOL:${PRISMDRAKE_ENABLE_DEVELOPER_OVERRIDES}>
    PRISMDRAKE_TEST_VERSION="${PRISMDRAKE_VERSION}")

prismdrake_configure_cpp_target(prismdrake-foundation-tests)
prismdrake_enable_warnings(prismdrake-foundation-tests)
prismdrake_enable_sanitizers(prismdrake-foundation-tests)

add_dependencies(prismdrake-foundation-tests prismdrake-session-child-fixture)
gtest_discover_tests(
    prismdrake-foundation-tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT
            "PRISMDRAKE_SESSION_CHILD_FIXTURE=$<TARGET_FILE:prismdrake-session-child-fixture>")
