cmake_minimum_required(VERSION 3.24)

project(prismdrake_pd1_toolkit_spike LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)

qt_standard_project_setup(REQUIRES 6.11)
qt_policy(SET QTP0004 NEW)

add_executable(prismdrake-pd1-toolkit-spike
    src/main.cpp
    src/PresentationModel.cpp
    src/PresentationModel.hpp
    src/ThemeTokens.cpp
    src/ThemeTokens.hpp
    src/X11DockAdapter.cpp
    src/X11DockAdapter.hpp
)

qt_add_qml_module(prismdrake-pd1-toolkit-spike
    URI org.prismdrake.experiments.toolkitspike
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_compile_definitions(prismdrake-pd1-toolkit-spike PRIVATE
    PRISMDRAKE_SPIKE_DEFAULT_THEME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../themes"
)

target_link_libraries(prismdrake-pd1-toolkit-spike PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    PkgConfig::XCB
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.11 REQUIRED COMPONENTS Test)

    add_executable(prismdrake-pd1-toolkit-spike-tests
        tests/PresentationModelTest.cpp
        src/PresentationModel.cpp
        src/PresentationModel.hpp
        src/ThemeTokens.cpp
        src/ThemeTokens.hpp
    )

    target_include_directories(prismdrake-pd1-toolkit-spike-tests PRIVATE src)
    target_compile_definitions(prismdrake-pd1-toolkit-spike-tests PRIVATE
        PRISMDRAKE_SPIKE_TEST_THEME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../themes"
    )
    target_link_libraries(prismdrake-pd1-toolkit-spike-tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Test
    )

    add_test(NAME prismdrake-pd1-toolkit-spike-model
        COMMAND prismdrake-pd1-toolkit-spike-tests
    )
endif()

# This experiment deliberately has no install() rules. It is evidence, not a
# production Prismdrake target.
