add_executable(prismdrake-shell-task-controller-tests
    TaskControllerCoreTests.cpp)
target_link_libraries(prismdrake-shell-task-controller-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::shell-task-controller)
prismdrake_configure_cpp_target(prismdrake-shell-task-controller-tests)
prismdrake_enable_warnings(prismdrake-shell-task-controller-tests)
prismdrake_enable_sanitizers(prismdrake-shell-task-controller-tests)
include(GoogleTest)
gtest_discover_tests(prismdrake-shell-task-controller-tests
    DISCOVERY_MODE PRE_TEST)

add_executable(prismdrake-shell-task-controller-x11-tests
    TaskControllerX11Tests.cpp)
target_link_libraries(prismdrake-shell-task-controller-x11-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::shell-task-controller)
prismdrake_configure_cpp_target(prismdrake-shell-task-controller-x11-tests)
prismdrake_enable_warnings(prismdrake-shell-task-controller-x11-tests)
prismdrake_enable_sanitizers(prismdrake-shell-task-controller-x11-tests)

find_program(PRISMDRAKE_TASK_CONTROLLER_XVFB_EXECUTABLE NAMES Xvfb)
if(PRISMDRAKE_TASK_CONTROLLER_XVFB_EXECUTABLE)
    add_test(
        NAME TaskControllerX11IntegrationTest
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/integration/run_with_xvfb.py"
            "${PRISMDRAKE_TASK_CONTROLLER_XVFB_EXECUTABLE}"
            "$<TARGET_FILE:prismdrake-shell-task-controller-x11-tests>"
            --)
    set_tests_properties(TaskControllerX11IntegrationTest PROPERTIES TIMEOUT 30)
else()
    add_test(
        NAME TaskControllerX11IntegrationTest
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP: Xvfb is unavailable; task-controller ownership integration requires the VM")
    set_tests_properties(TaskControllerX11IntegrationTest PROPERTIES
        SKIP_REGULAR_EXPRESSION "^SKIP:"
        TIMEOUT 5)
endif()
