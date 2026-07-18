add_executable(prismdrake-pd1-performance-evidence
    PerformanceEvidenceRunner.cpp)
target_link_libraries(prismdrake-pd1-performance-evidence
    PRIVATE
        Prismdrake::launcher-model
        Prismdrake::settings-core
        Prismdrake::x11
        nlohmann_json::nlohmann_json)
target_compile_definitions(prismdrake-pd1-performance-evidence
    PRIVATE
        PRISMDRAKE_PERFORMANCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PRISMDRAKE_PERFORMANCE_ARCHITECTURE="${CMAKE_SYSTEM_PROCESSOR}"
        PRISMDRAKE_PERFORMANCE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        PRISMDRAKE_PERFORMANCE_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
        PRISMDRAKE_PERFORMANCE_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
        PRISMDRAKE_PERFORMANCE_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}")

prismdrake_configure_cpp_target(prismdrake-pd1-performance-evidence)
prismdrake_enable_warnings(prismdrake-pd1-performance-evidence)
prismdrake_enable_sanitizers(prismdrake-pd1-performance-evidence)

add_test(
    NAME Pd1PerformanceEvidenceHarnessTest
    COMMAND prismdrake-pd1-performance-evidence
        --revision 0000000000000000000000000000000000000000
        --environment-id contract-test
        --iterations 3)
set_tests_properties(Pd1PerformanceEvidenceHarnessTest PROPERTIES
    LABELS performance
    TIMEOUT 30)

add_test(
    NAME Pd1PerformanceEvidenceSchemaTest
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/validate_evidence.py"
        "$<TARGET_FILE:prismdrake-pd1-performance-evidence>"
        "${CMAKE_CURRENT_SOURCE_DIR}/performance-evidence.schema.json")
set_tests_properties(Pd1PerformanceEvidenceSchemaTest PROPERTIES
    LABELS performance
    TIMEOUT 30)

add_test(
    NAME Pd1PerformanceEvidenceRejectsInvalidEnvironmentId
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/assert_cli_failure.py"
        "$<TARGET_FILE:prismdrake-pd1-performance-evidence>"
        invalid_environment_id
        --revision 0000000000000000000000000000000000000000
        --environment-id Host-Name
        --iterations 3)
add_test(
    NAME Pd1PerformanceEvidenceRejectsIterationBelowBound
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/assert_cli_failure.py"
        "$<TARGET_FILE:prismdrake-pd1-performance-evidence>"
        invalid_iteration_count
        --revision 0000000000000000000000000000000000000000
        --environment-id contract-test
        --iterations 2)
set_tests_properties(
    Pd1PerformanceEvidenceRejectsInvalidEnvironmentId
    Pd1PerformanceEvidenceRejectsIterationBelowBound
    PROPERTIES
        LABELS performance
        TIMEOUT 5)

add_test(
    NAME Pd1ExternalPerformanceCollectorUnitTest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_external_collectors.py")
set_tests_properties(Pd1ExternalPerformanceCollectorUnitTest PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    LABELS performance
    TIMEOUT 10)

find_program(prismdrake_performance_fc_match_executable fc-match REQUIRED)
execute_process(
    COMMAND "${prismdrake_performance_fc_match_executable}"
        --format=%{family[0]}\n sans-serif
    RESULT_VARIABLE prismdrake_performance_font_result
    OUTPUT_VARIABLE prismdrake_performance_font_family
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT prismdrake_performance_font_result EQUAL 0 OR
   prismdrake_performance_font_family STREQUAL "")
    message(FATAL_ERROR
        "The cadence collector could not lock the generic sans-serif font through fontconfig.")
endif()
execute_process(
    COMMAND "${prismdrake_performance_fc_match_executable}"
        --format=%{file}\n sans-serif
    RESULT_VARIABLE prismdrake_performance_font_file_result
    OUTPUT_VARIABLE prismdrake_performance_font_file
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT prismdrake_performance_font_file_result EQUAL 0 OR
   prismdrake_performance_font_file STREQUAL "" OR
   NOT EXISTS "${prismdrake_performance_font_file}")
    message(FATAL_ERROR
        "The cadence collector could not resolve the generic sans-serif font source.")
endif()
get_filename_component(
    prismdrake_performance_font_source_basename
    "${prismdrake_performance_font_file}"
    NAME)
if(NOT prismdrake_performance_font_source_basename MATCHES
   "^[A-Za-z0-9][A-Za-z0-9._+-]*$")
    message(FATAL_ERROR "The cadence font source basename is not artifact-safe.")
endif()
file(SHA256 "${prismdrake_performance_font_file}"
    prismdrake_performance_font_source_sha256)

add_executable(prismdrake-visual-cadence-evidence
    VisualCadenceRunner.cpp
    ../qt/PanelSurfaceQmlFixture.cpp
    ../qt/PanelSurfaceQmlFixture.hpp)
set_target_properties(prismdrake-visual-cadence-evidence PROPERTIES AUTOMOC ON)
target_compile_definitions(prismdrake-visual-cadence-evidence PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    PRISMDRAKE_PERFORMANCE_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
    PRISMDRAKE_VISUAL_EXPECTED_FONT_FAMILY="${prismdrake_performance_font_family}"
    PRISMDRAKE_VISUAL_FONT_SOURCE_BASENAME="${prismdrake_performance_font_source_basename}"
    PRISMDRAKE_VISUAL_FONT_SOURCE_SHA256="${prismdrake_performance_font_source_sha256}")
target_include_directories(prismdrake-visual-cadence-evidence PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../qt")
target_link_libraries(prismdrake-visual-cadence-evidence
    PRIVATE
        Prismdrake::settings-core
        Prismdrake::shell-task-presentation
        Prismdrake::shell-panel-qml
        Prismdrake::shell-theme
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick)
prismdrake_configure_cpp_target(prismdrake-visual-cadence-evidence)
prismdrake_enable_warnings(prismdrake-visual-cadence-evidence)
prismdrake_enable_sanitizers(prismdrake-visual-cadence-evidence)

add_test(
    NAME Pd1VisualCadenceEvidenceTest
    COMMAND prismdrake-visual-cadence-evidence
        --revision 0000000000000000000000000000000000000000
        --environment-id contract-test)
set_tests_properties(Pd1VisualCadenceEvidenceTest PROPERTIES
    ENVIRONMENT
        "LANG=C.UTF-8;LC_ALL=C.UTF-8;TZ=UTC;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QSG_RENDER_LOOP=basic;QT_QUICK_CONTROLS_STYLE=Basic;QT_SCALE_FACTOR=1"
    LABELS performance
    TIMEOUT 60)

add_test(
    NAME Pd1ExternalPerformanceEvidenceSchemaTest
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/validate_external_evidence.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/external-performance-evidence.schema.json"
        "$<TARGET_FILE:prismdrake-visual-cadence-evidence>")
set_tests_properties(Pd1ExternalPerformanceEvidenceSchemaTest PROPERTIES
    ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1;LANG=C.UTF-8;LC_ALL=C.UTF-8;TZ=UTC;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QSG_RENDER_LOOP=basic;QT_QUICK_CONTROLS_STYLE=Basic;QT_SCALE_FACTOR=1"
    LABELS performance
    TIMEOUT 60)

find_program(prismdrake_performance_xvfb Xvfb)
find_program(prismdrake_performance_openbox openbox)
find_program(prismdrake_performance_xprop xprop)
find_program(prismdrake_performance_xev xev)
find_program(prismdrake_performance_stdbuf stdbuf)
find_program(prismdrake_performance_dbus_run_session dbus-run-session)
if(prismdrake_performance_xvfb AND prismdrake_performance_openbox AND
   prismdrake_performance_xprop AND prismdrake_performance_xev AND
   prismdrake_performance_stdbuf AND prismdrake_performance_dbus_run_session)
    add_test(
        NAME Pd1StartupToMappedPanelCollectorIntegrationTest
        COMMAND
            "${prismdrake_performance_dbus_run_session}" --
            "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/integration/run_with_xvfb.py"
            --openbox "${prismdrake_performance_openbox}"
            --xprop "${prismdrake_performance_xprop}"
            "${prismdrake_performance_xvfb}"
            "${Python3_EXECUTABLE}"
            --
            "${CMAKE_CURRENT_SOURCE_DIR}/validate_live_startup.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/external-performance-evidence.schema.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/collect_startup_to_panel.py"
            --
            --session "$<TARGET_FILE:prismdrake-session>"
            --settingsd "$<TARGET_FILE:prismdrake-settingsd>"
            --shell "$<TARGET_FILE:prismdrake-shell>"
            --xev "${prismdrake_performance_xev}"
            --xprop "${prismdrake_performance_xprop}"
            --stdbuf "${prismdrake_performance_stdbuf}"
            --revision 0000000000000000000000000000000000000000
            --environment-id contract-test)
    set_tests_properties(Pd1StartupToMappedPanelCollectorIntegrationTest PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        LABELS performance
        RUN_SERIAL TRUE
        TIMEOUT 30)
else()
    add_test(
        NAME Pd1StartupToMappedPanelCollectorIntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: the startup collector requires Xvfb, Openbox, xprop, xev, stdbuf, and dbus-run-session")
    set_tests_properties(Pd1StartupToMappedPanelCollectorIntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        LABELS performance
        TIMEOUT 5)
endif()
