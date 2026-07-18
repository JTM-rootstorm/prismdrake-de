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

add_executable(prismdrake-panel-visual-baseline-tests
    PanelVisualBaselineRecorder.cpp
    PanelVisualBaselineRecorder.hpp
    PanelVisualBaselineTestRunner.cpp
    ../qt/PanelSurfaceQmlFixture.cpp
    ../qt/PanelSurfaceQmlFixture.hpp)
set_target_properties(prismdrake-panel-visual-baseline-tests PROPERTIES AUTOMOC ON)
target_compile_definitions(prismdrake-panel-visual-baseline-tests PRIVATE
    PRISMDRAKE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    PRISMDRAKE_VISUAL_ARTIFACT_DIR="${prismdrake_visual_artifact_directory}"
    PRISMDRAKE_VISUAL_EXPECTED_FONT_FAMILY="${prismdrake_visual_font_family}"
    PRISMDRAKE_VISUAL_EXPECTED_FONT_SOURCE="${prismdrake_visual_font_source}")
target_include_directories(prismdrake-panel-visual-baseline-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../qt")
target_link_libraries(prismdrake-panel-visual-baseline-tests
    PRIVATE
        Prismdrake::settings-core
        Prismdrake::shell-task-presentation
        Prismdrake::shell-theme
        Prismdrake::shell-panel-qml
        Qt6::Gui
        Qt6::Qml
        Qt6::QuickTest)

prismdrake_configure_cpp_target(prismdrake-panel-visual-baseline-tests)
prismdrake_enable_warnings(prismdrake-panel-visual-baseline-tests)
prismdrake_enable_sanitizers(prismdrake-panel-visual-baseline-tests)

add_test(NAME PanelVisualBaselineTest
    COMMAND prismdrake-panel-visual-baseline-tests
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml")
set_tests_properties(PanelVisualBaselineTest PROPERTIES
    ENVIRONMENT
        "LANG=C.UTF-8;LC_ALL=C.UTF-8;TZ=UTC;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QSG_RENDER_LOOP=basic;QT_QUICK_CONTROLS_STYLE=Basic;QT_SCALE_FACTOR=1")
