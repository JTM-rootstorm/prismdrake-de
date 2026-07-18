find_package(Python3 3.11 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR
        "Prismdrake tests require Python 3.11 or newer for the hermetic Gentoo VM "
        "tool checks. Install Python 3.11+, or configure with -DBUILD_TESTING=OFF.")
endif()

add_test(
    NAME GentooVmToolsTest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gentoo/test_vm_tools.py")
set_tests_properties(GentooVmToolsTest PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(
    NAME GentooPortageLifecycleEvidenceTest
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/gentoo/test_portage_lifecycle_evidence.py")
set_tests_properties(GentooPortageLifecycleEvidenceTest PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(
    NAME GentooInstalledArtifactAttestationTest
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/gentoo/test_installed_artifact_attestation.py")
set_tests_properties(GentooInstalledArtifactAttestationTest PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(
    NAME GentooPortageLifecycleCollectorTest
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/gentoo/test_portage_lifecycle_collector.py")
set_tests_properties(GentooPortageLifecycleCollectorTest PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_subdirectory(fixtures)
add_subdirectory(accessibility)
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(performance)
add_subdirectory(qt)
add_subdirectory(visual)
