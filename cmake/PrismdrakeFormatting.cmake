include_guard(GLOBAL)

function(prismdrake_register_format_sources)
    file(REAL_PATH "${PROJECT_SOURCE_DIR}" prismdrake_project_source_root)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" prismdrake_project_binary_root)
    set(prismdrake_format_sources)
    foreach(prismdrake_source IN LISTS ARGN)
        if(IS_ABSOLUTE "${prismdrake_source}")
            set(prismdrake_source_path "${prismdrake_source}")
        else()
            set(prismdrake_source_path
                "${CMAKE_CURRENT_SOURCE_DIR}/${prismdrake_source}")
        endif()
        if(NOT EXISTS "${prismdrake_source_path}")
            message(FATAL_ERROR
                "Cannot register missing formatting source '${prismdrake_source_path}'")
        endif()

        file(REAL_PATH "${prismdrake_source_path}" prismdrake_source_path)
        string(FIND "${prismdrake_source_path}"
            "${prismdrake_project_source_root}/" prismdrake_source_root_offset)
        if(NOT prismdrake_source_root_offset EQUAL 0)
            continue()
        endif()
        string(FIND "${prismdrake_source_path}"
            "${prismdrake_project_binary_root}/" prismdrake_binary_root_offset)
        if(prismdrake_binary_root_offset EQUAL 0)
            continue()
        endif()
        list(APPEND prismdrake_format_sources "${prismdrake_source_path}")
    endforeach()

    if(prismdrake_format_sources)
        set_property(GLOBAL APPEND PROPERTY
            PRISMDRAKE_REGISTERED_FORMAT_SOURCES ${prismdrake_format_sources})
    endif()
endfunction()

function(prismdrake_register_target_format_sources target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR
            "Cannot register formatting sources for missing Prismdrake target '${target}'")
    endif()

    get_target_property(prismdrake_target_source_dir "${target}" SOURCE_DIR)
    get_target_property(prismdrake_target_sources "${target}" SOURCES)
    if(NOT prismdrake_target_sources)
        return()
    endif()

    set(prismdrake_format_sources)
    foreach(prismdrake_source IN LISTS prismdrake_target_sources)
        if(prismdrake_source MATCHES "^\\$<")
            continue()
        endif()
        if(NOT prismdrake_source MATCHES
           "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|ipp|tpp)(\\.in)?$")
            continue()
        endif()

        if(IS_ABSOLUTE "${prismdrake_source}")
            set(prismdrake_source_path "${prismdrake_source}")
        else()
            set(prismdrake_source_path
                "${prismdrake_target_source_dir}/${prismdrake_source}")
        endif()
        if(EXISTS "${prismdrake_source_path}")
            list(APPEND prismdrake_format_sources "${prismdrake_source_path}")
        endif()
    endforeach()

    if(prismdrake_format_sources)
        prismdrake_register_format_sources(${prismdrake_format_sources})
    endif()
endfunction()

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

    get_property(prismdrake_format_sources GLOBAL PROPERTY
        PRISMDRAKE_REGISTERED_FORMAT_SOURCES)
    if(NOT prismdrake_format_sources)
        message(FATAL_ERROR
            "No project C++ sources were registered for the format-check target")
    endif()
    list(REMOVE_DUPLICATES prismdrake_format_sources)
    list(SORT prismdrake_format_sources)

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
