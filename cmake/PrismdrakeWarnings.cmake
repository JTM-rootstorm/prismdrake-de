include_guard(GLOBAL)

function(prismdrake_enable_warnings target)
    target_compile_options("${target}" PRIVATE -Wall -Wextra -Wpedantic)
    if(PRISMDRAKE_WARNINGS_AS_ERRORS)
        target_compile_options("${target}" PRIVATE -Werror)
    endif()
endfunction()
