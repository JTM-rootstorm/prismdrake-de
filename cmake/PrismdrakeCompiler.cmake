include_guard(GLOBAL)

include(CheckIPOSupported)

function(prismdrake_validate_toolchain)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR
            "Prismdrake PD1 supports GCC and Clang on Linux. The selected compiler "
            "reports '${CMAKE_CXX_COMPILER_ID}'. Select g++ or clang++ and configure again.")
    endif()

    if(PRISMDRAKE_ENABLE_LTO)
        check_ipo_supported(RESULT prismdrake_ipo_supported OUTPUT prismdrake_ipo_error)
        if(NOT prismdrake_ipo_supported)
            message(FATAL_ERROR
                "PRISMDRAKE_ENABLE_LTO=ON, but interprocedural optimization is not "
                "supported by this toolchain: ${prismdrake_ipo_error}")
        endif()
    endif()

    if(PRISMDRAKE_ENABLE_CLANG_TIDY)
        if(PRISMDRAKE_CLANG_TIDY_EXECUTABLE)
            if(NOT EXISTS "${PRISMDRAKE_CLANG_TIDY_EXECUTABLE}")
                message(FATAL_ERROR
                    "PRISMDRAKE_CLANG_TIDY_EXECUTABLE does not exist: "
                    "${PRISMDRAKE_CLANG_TIDY_EXECUTABLE}")
            endif()
            set(prismdrake_clang_tidy "${PRISMDRAKE_CLANG_TIDY_EXECUTABLE}")
        else()
            find_program(prismdrake_clang_tidy NAMES clang-tidy)
        endif()
        if(NOT prismdrake_clang_tidy)
            message(FATAL_ERROR
                "PRISMDRAKE_ENABLE_CLANG_TIDY=ON requires clang-tidy. Install it or "
                "set PRISMDRAKE_CLANG_TIDY_EXECUTABLE to the reviewed executable.")
        endif()
        set_property(GLOBAL PROPERTY PRISMDRAKE_CLANG_TIDY_PATH
            "${prismdrake_clang_tidy}")
    endif()
endfunction()

function(prismdrake_configure_cpp_target target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot configure missing Prismdrake target '${target}'")
    endif()

    set_target_properties("${target}" PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    target_link_libraries("${target}" PRIVATE Prismdrake::cpp-policy)

    if(PRISMDRAKE_ENABLE_LTO)
        set_property(TARGET "${target}" PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    if(PRISMDRAKE_ENABLE_CLANG_TIDY)
        get_property(prismdrake_clang_tidy GLOBAL PROPERTY PRISMDRAKE_CLANG_TIDY_PATH)
        set_property(TARGET "${target}" PROPERTY CXX_CLANG_TIDY
            "${prismdrake_clang_tidy}")
    endif()
endfunction()
