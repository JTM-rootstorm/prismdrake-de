add_executable(prismdrake-shell-notification-presentation-tests
    NotificationPresentationModelTests.cpp)

target_link_libraries(prismdrake-shell-notification-presentation-tests
    PRIVATE
        GTest::gtest_main
        Prismdrake::shell-notification-presentation)

prismdrake_configure_cpp_target(prismdrake-shell-notification-presentation-tests)
prismdrake_enable_warnings(prismdrake-shell-notification-presentation-tests)
prismdrake_enable_sanitizers(prismdrake-shell-notification-presentation-tests)

include(GoogleTest)
gtest_discover_tests(prismdrake-shell-notification-presentation-tests
    DISCOVERY_MODE PRE_TEST)

add_test(NAME NotificationCardQmlTest
    COMMAND $<TARGET_FILE:Qt6::qmltestrunner>
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml")
set_tests_properties(NotificationCardQmlTest PROPERTIES
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")

add_executable(prismdrake-notification-list-qml-tests
    NotificationListQmlFixture.cpp
    NotificationListQmlFixture.hpp
    NotificationListQmlTestRunner.cpp)
set_target_properties(prismdrake-notification-list-qml-tests PROPERTIES AUTOMOC ON)

target_link_libraries(prismdrake-notification-list-qml-tests
    PRIVATE
        Prismdrake::shell-notification-presentation
        Qt6::Qml
        Qt6::QuickTest)

prismdrake_configure_cpp_target(prismdrake-notification-list-qml-tests)
prismdrake_enable_warnings(prismdrake-notification-list-qml-tests)
prismdrake_enable_sanitizers(prismdrake-notification-list-qml-tests)

add_test(NAME NotificationListQmlTest
    COMMAND prismdrake-notification-list-qml-tests
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml-real")
set_tests_properties(NotificationListQmlTest PROPERTIES
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic")
