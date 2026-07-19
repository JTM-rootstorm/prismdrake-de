find_package(Python3 3.11 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR
        "Prismdrake tests require Python 3.11 or newer for the hermetic Gentoo VM "
        "tool checks. Install Python 3.11+, or configure with -DBUILD_TESTING=OFF.")
endif()

add_subdirectory(fixtures)
add_subdirectory(gentoo)
add_subdirectory(accessibility)
add_subdirectory(applicationidentity)
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(performance)
add_subdirectory(pins)
add_subdirectory(qt)
add_subdirectory(visual)
