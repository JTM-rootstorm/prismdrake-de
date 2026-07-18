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
