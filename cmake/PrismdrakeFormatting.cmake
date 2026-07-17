include_guard(GLOBAL)

function(prismdrake_add_format_check)
    if(PRISMDRAKE_CLANG_FORMAT_EXECUTABLE)
        if(NOT EXISTS "${PRISMDRAKE_CLANG_FORMAT_EXECUTABLE}")
            message(FATAL_ERROR
                "PRISMDRAKE_CLANG_FORMAT_EXECUTABLE does not exist: "
                "${PRISMDRAKE_CLANG_FORMAT_EXECUTABLE}")
        endif()
        set(prismdrake_clang_format "${PRISMDRAKE_CLANG_FORMAT_EXECUTABLE}")
    else()
        find_program(prismdrake_clang_format NAMES clang-format)
    endif()

    set(prismdrake_format_sources)
    foreach(prismdrake_source IN LISTS ARGN)
        list(APPEND prismdrake_format_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/${prismdrake_source}")
    endforeach()

    if(prismdrake_clang_format)
        add_custom_target(format-check
            COMMAND "${prismdrake_clang_format}"
                --dry-run --Werror ${prismdrake_format_sources}
            COMMENT "Checking C++ formatting"
            VERBATIM)
    else()
        add_custom_target(format-check
            COMMAND "${CMAKE_COMMAND}" -E echo
                "clang-format is required for format-check; install it or set PRISMDRAKE_CLANG_FORMAT_EXECUTABLE"
            COMMAND "${CMAKE_COMMAND}" -E false
            COMMENT "clang-format is unavailable"
            VERBATIM)
    endif()
endfunction()
