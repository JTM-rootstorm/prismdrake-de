include(GoogleTest)
find_package(Threads REQUIRED)

add_executable(prismdrake-foundation-tests
    ApplicationCatalogTests.cpp
    ApplicationPathsTests.cpp
    ApplicationSearchTests.cpp
    AtomicFileTests.cpp
    BoundedFileTests.cpp
    BuildInfoTests.cpp
    ChildProcessTests.cpp
    ConfigurationLoaderTests.cpp
    ConfigurationParserTests.cpp
    DiagnosticsTests.cpp
    DesktopEntryDiscoveryTests.cpp
    DesktopEntryVisibilityTests.cpp
    DesktopEntryParserTests.cpp
    DesktopExecutableTests.cpp
    DesktopExecTests.cpp
    DesktopFileIdTests.cpp
    DockPropertiesTests.cpp
    EwmhTaskListTests.cpp
    EwmhWindowRequestsTests.cpp
    FoundationPrimitivesTests.cpp
    LauncherPipelineTests.cpp
    OutputTopologyTests.cpp
    PanelStrutGeometryTests.cpp
    PropertyReaderTests.cpp
    ProcessLaunchTests.cpp
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
        Prismdrake::launcher-model
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

add_dependencies(prismdrake-foundation-tests
    prismdrake-application-launch-fixture
    prismdrake-session-child-fixture)
gtest_discover_tests(
    prismdrake-foundation-tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT
            "PRISMDRAKE_APPLICATION_LAUNCH_FIXTURE=$<TARGET_FILE:prismdrake-application-launch-fixture>"
            "PRISMDRAKE_SESSION_CHILD_FIXTURE=$<TARGET_FILE:prismdrake-session-child-fixture>")

add_executable(prismdrake-detached-application-tests
    "${PROJECT_SOURCE_DIR}/src/launcher/DetachedApplication.cpp"
    DetachedApplicationTests.cpp)
target_include_directories(prismdrake-detached-application-tests
    PRIVATE "${PROJECT_SOURCE_DIR}/src/launcher")
target_link_libraries(prismdrake-detached-application-tests
    PRIVATE Prismdrake::foundation GTest::gtest_main Threads::Threads)
target_compile_definitions(prismdrake-detached-application-tests
    PRIVATE PRISMDRAKE_DETACHED_APPLICATION_TEST_HOOK=1)

prismdrake_configure_cpp_target(prismdrake-detached-application-tests)
prismdrake_enable_warnings(prismdrake-detached-application-tests)
prismdrake_enable_sanitizers(prismdrake-detached-application-tests)

add_dependencies(prismdrake-detached-application-tests prismdrake-application-launch-fixture)
gtest_discover_tests(
    prismdrake-detached-application-tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT
            "PRISMDRAKE_APPLICATION_LAUNCH_FIXTURE=$<TARGET_FILE:prismdrake-application-launch-fixture>")
