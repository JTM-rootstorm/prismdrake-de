set(prismdrake_visual_artifact_directory
    "${CMAKE_BINARY_DIR}/test-artifacts/visual")

find_program(prismdrake_fc_match_executable fc-match)
if(NOT prismdrake_fc_match_executable)
    message(FATAL_ERROR
        "The deterministic visual harness requires fontconfig's fc-match tool to lock "
        "the generic sans-serif font. Install fontconfig or configure with "
        "-DBUILD_TESTING=OFF.")
endif()
execute_process(
    COMMAND "${prismdrake_fc_match_executable}"
        --format=%{family[0]}\\n%{file} sans-serif
    RESULT_VARIABLE prismdrake_visual_font_result
    OUTPUT_VARIABLE prismdrake_visual_font_record
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT prismdrake_visual_font_result EQUAL 0 OR
   prismdrake_visual_font_record STREQUAL "")
    message(FATAL_ERROR
        "The deterministic visual harness could not resolve the required generic "
        "sans-serif font through fontconfig.")
endif()
string(REPLACE "\n" ";" prismdrake_visual_font_record
    "${prismdrake_visual_font_record}")
list(LENGTH prismdrake_visual_font_record prismdrake_visual_font_record_length)
if(NOT prismdrake_visual_font_record_length EQUAL 2)
    message(FATAL_ERROR
        "The deterministic visual harness received an incomplete fontconfig record.")
endif()
list(GET prismdrake_visual_font_record 0 prismdrake_visual_font_family)
list(GET prismdrake_visual_font_record 1 prismdrake_visual_font_file)
get_filename_component(prismdrake_visual_font_source
    "${prismdrake_visual_font_file}" NAME)

set(prismdrake_visual_test_environment
    "LANG=C.UTF-8;LC_ALL=C.UTF-8;TZ=UTC;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QSG_RENDER_LOOP=basic;QT_QUICK_CONTROLS_STYLE=Basic;QT_SCALE_FACTOR=1")

add_library(prismdrake-visual-baseline-recorder STATIC
    PanelVisualBaselineRecorder.cpp
    PanelVisualBaselineRecorder.hpp)
set_target_properties(prismdrake-visual-baseline-recorder PROPERTIES AUTOMOC ON)
target_compile_definitions(prismdrake-visual-baseline-recorder PRIVATE
    PRISMDRAKE_VISUAL_ARTIFACT_DIR="${prismdrake_visual_artifact_directory}"
    PRISMDRAKE_VISUAL_EXPECTED_FONT_FAMILY="${prismdrake_visual_font_family}"
    PRISMDRAKE_VISUAL_EXPECTED_FONT_SOURCE="${prismdrake_visual_font_source}")
target_include_directories(prismdrake-visual-baseline-recorder PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(prismdrake-visual-baseline-recorder
    PUBLIC Prismdrake::shell-theme
    PRIVATE Qt6::Gui)

prismdrake_configure_cpp_target(prismdrake-visual-baseline-recorder)
prismdrake_enable_warnings(prismdrake-visual-baseline-recorder)
prismdrake_enable_sanitizers(prismdrake-visual-baseline-recorder)

add_executable(prismdrake-panel-visual-baseline-tests
    PanelVisualBaselineTestRunner.cpp
    ../qt/PanelSurfaceQmlFixture.cpp
    ../qt/PanelSurfaceQmlFixture.hpp)
set_target_properties(prismdrake-panel-visual-baseline-tests PROPERTIES AUTOMOC ON)
target_compile_definitions(prismdrake-panel-visual-baseline-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(prismdrake-panel-visual-baseline-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../qt")
target_link_libraries(prismdrake-panel-visual-baseline-tests
    PRIVATE
        Prismdrake::settings-core
        Prismdrake::shell-task-presentation
        Prismdrake::shell-panel-qml
        prismdrake-visual-baseline-recorder
        Qt6::Gui
        Qt6::Qml
        Qt6::QuickTest)

prismdrake_configure_cpp_target(prismdrake-panel-visual-baseline-tests)
prismdrake_enable_warnings(prismdrake-panel-visual-baseline-tests)
prismdrake_enable_sanitizers(prismdrake-panel-visual-baseline-tests)

add_test(NAME PanelVisualBaselineTest
    COMMAND prismdrake-panel-visual-baseline-tests
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/panel")
set_tests_properties(PanelVisualBaselineTest PROPERTIES
    ENVIRONMENT "${prismdrake_visual_test_environment}")

add_executable(prismdrake-launcher-visual-baseline-tests
    LauncherVisualBaselineTestRunner.cpp
    ../qt/launcherqml/LauncherSurfaceQmlFixture.cpp
    ../qt/launcherqml/LauncherSurfaceQmlFixture.hpp)
set_target_properties(prismdrake-launcher-visual-baseline-tests PROPERTIES AUTOMOC ON)
target_compile_definitions(prismdrake-launcher-visual-baseline-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(prismdrake-launcher-visual-baseline-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../qt/launcherqml")
target_link_libraries(prismdrake-launcher-visual-baseline-tests
    PRIVATE
        Prismdrake::settings-core
        Prismdrake::shell-launcher-presentation
        Prismdrake::shell-launcher-qml
        prismdrake-visual-baseline-recorder
        Qt6::Gui
        Qt6::Qml
        Qt6::QuickTest)

prismdrake_configure_cpp_target(prismdrake-launcher-visual-baseline-tests)
prismdrake_enable_warnings(prismdrake-launcher-visual-baseline-tests)
prismdrake_enable_sanitizers(prismdrake-launcher-visual-baseline-tests)

add_test(NAME LauncherVisualBaselineTest
    COMMAND prismdrake-launcher-visual-baseline-tests
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/launcher")
set_tests_properties(LauncherVisualBaselineTest PROPERTIES
    ENVIRONMENT "${prismdrake_visual_test_environment}")

add_executable(prismdrake-notification-visual-baseline-tests
    NotificationVisualBaselineTestRunner.cpp
    VisualThemeFixture.cpp
    VisualThemeFixture.hpp
    ../qt/NotificationListQmlFixture.cpp
    ../qt/NotificationListQmlFixture.hpp)
set_target_properties(prismdrake-notification-visual-baseline-tests PROPERTIES AUTOMOC ON)
target_compile_definitions(prismdrake-notification-visual-baseline-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(prismdrake-notification-visual-baseline-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../qt")
target_link_libraries(prismdrake-notification-visual-baseline-tests
    PRIVATE
        Prismdrake::settings-core
        Prismdrake::shell-notification-presentation
        Prismdrake::shell-notification-qml
        prismdrake-visual-baseline-recorder
        Qt6::Gui
        Qt6::Qml
        Qt6::QuickTest)

prismdrake_configure_cpp_target(prismdrake-notification-visual-baseline-tests)
prismdrake_enable_warnings(prismdrake-notification-visual-baseline-tests)
prismdrake_enable_sanitizers(prismdrake-notification-visual-baseline-tests)

add_test(NAME NotificationVisualBaselineTest
    COMMAND prismdrake-notification-visual-baseline-tests
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/notification")
set_tests_properties(NotificationVisualBaselineTest PROPERTIES
    ENVIRONMENT "${prismdrake_visual_test_environment}")
