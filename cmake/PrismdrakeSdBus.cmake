include_guard(GLOBAL)

function(prismdrake_configure_sd_bus_provider)
    if(TARGET prismdrake-sd-bus-provider)
        return()
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PRISMDRAKE_BASU QUIET IMPORTED_TARGET basu)

    add_library(prismdrake-sd-bus-provider INTERFACE)
    add_library(Prismdrake::sd-bus-provider ALIAS prismdrake-sd-bus-provider)

    if(TARGET PkgConfig::PRISMDRAKE_BASU)
        target_link_libraries(prismdrake-sd-bus-provider
            INTERFACE PkgConfig::PRISMDRAKE_BASU)
        target_compile_definitions(prismdrake-sd-bus-provider
            INTERFACE PRISMDRAKE_SD_BUS_PROVIDER_BASU=1)
        return()
    endif()

    pkg_check_modules(PRISMDRAKE_LIBSYSTEMD QUIET IMPORTED_TARGET libsystemd)
    if(TARGET PkgConfig::PRISMDRAKE_LIBSYSTEMD)
        target_link_libraries(prismdrake-sd-bus-provider
            INTERFACE PkgConfig::PRISMDRAKE_LIBSYSTEMD)
        target_compile_definitions(prismdrake-sd-bus-provider
            INTERFACE PRISMDRAKE_SD_BUS_PROVIDER_LIBSYSTEMD=1)
        return()
    endif()

    message(FATAL_ERROR
        "Prismdrake settings service requires an sd-bus provider. Install basu "
        "(preferred) or libsystemd development files with pkg-config metadata. "
        "Prismdrake does not download or vendor D-Bus providers.")
endfunction()
