include_guard(GLOBAL)

function(prismdrake_enable_sanitizers target)
    set(prismdrake_sanitizers)
    if(PRISMDRAKE_ENABLE_ASAN)
        list(APPEND prismdrake_sanitizers address)
    endif()
    if(PRISMDRAKE_ENABLE_UBSAN)
        list(APPEND prismdrake_sanitizers undefined)
    endif()

    if(prismdrake_sanitizers)
        list(JOIN prismdrake_sanitizers "," prismdrake_sanitizer_list)
        target_compile_options("${target}" PRIVATE
            "-fsanitize=${prismdrake_sanitizer_list}"
            -fno-omit-frame-pointer)
        target_link_options("${target}" PRIVATE
            "-fsanitize=${prismdrake_sanitizer_list}")
    endif()
endfunction()
