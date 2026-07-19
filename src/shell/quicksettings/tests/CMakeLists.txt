add_executable(prismdrake-quick-settings-model-tests
    QuickSettingsModelTests.cpp)
target_compile_definitions(prismdrake-quick-settings-model-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(prismdrake-quick-settings-model-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::settings-core
        Prismdrake::shell-quick-settings
        Qt6::Test)

prismdrake_configure_cpp_target(prismdrake-quick-settings-model-tests)
prismdrake_enable_warnings(prismdrake-quick-settings-model-tests)
prismdrake_enable_sanitizers(prismdrake-quick-settings-model-tests)

include(GoogleTest)
gtest_discover_tests(prismdrake-quick-settings-model-tests
    DISCOVERY_MODE PRE_TEST)

add_test(NAME QuickSettingsSurfaceQmlTest
    COMMAND $<TARGET_FILE:Qt6::qmltestrunner>
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml")
set_tests_properties(QuickSettingsSurfaceQmlTest PROPERTIES
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
