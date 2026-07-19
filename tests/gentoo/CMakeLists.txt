function(prismdrake_add_gentoo_python_test test_name script_name)
    add_test(
        NAME "${test_name}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${script_name}")
    set_tests_properties("${test_name}" PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endfunction()

prismdrake_add_gentoo_python_test(GentooVmToolsTest test_vm_tools.py)
prismdrake_add_gentoo_python_test(
    GentooProductEbuildTest test_product_ebuild.py)
prismdrake_add_gentoo_python_test(
    GentooCMakeInstallContractTest test_cmake_install_contract.py)
prismdrake_add_gentoo_python_test(
    GentooPortageLifecycleEvidenceTest test_portage_lifecycle_evidence.py)
prismdrake_add_gentoo_python_test(
    GentooInstalledArtifactAttestationTest test_installed_artifact_attestation.py)
prismdrake_add_gentoo_python_test(
    GentooPortageLifecycleCollectorTest test_portage_lifecycle_collector.py)
prismdrake_add_gentoo_python_test(
    GentooPd2InstalledArtifactAttestationTest
    test_pd2_installed_artifact_attestation.py)
prismdrake_add_gentoo_python_test(
    GentooPd2PortageLifecycleEvidenceTest
    test_pd2_portage_lifecycle_evidence.py)
prismdrake_add_gentoo_python_test(
    GentooPd2PortageLifecycleCollectorTest
    test_pd2_portage_lifecycle_collector.py)
