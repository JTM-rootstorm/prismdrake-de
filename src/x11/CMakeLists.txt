find_package(PkgConfig REQUIRED)
pkg_check_modules(PRISMDRAKE_XCB REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(PRISMDRAKE_XCB_RANDR REQUIRED IMPORTED_TARGET xcb-randr)

add_library(prismdrake-x11 STATIC
    AtomCache.cpp
    AtomCache.hpp
    DockProperties.cpp
    DockProperties.hpp
    DockPropertiesInternal.hpp
    EwmhCapabilities.cpp
    EwmhCapabilities.hpp
    EwmhCapabilitiesInternal.hpp
    EwmhWindowRequests.cpp
    EwmhWindowRequests.hpp
    EwmhWindowRequestsInternal.hpp
    OutputTopology.cpp
    OutputTopology.hpp
    PanelStrutGeometry.cpp
    PanelStrutGeometry.hpp
    PropertyReader.cpp
    PropertyReader.hpp
    RandrTopology.cpp
    RandrTopology.hpp
    RootEventStream.cpp
    RootEventStream.hpp
    X11Connection.cpp
    X11Connection.hpp
    X11ConnectionPrivate.hpp
    X11Types.cpp
    X11Types.hpp)
add_library(Prismdrake::x11 ALIAS prismdrake-x11)

target_include_directories(prismdrake-x11
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(prismdrake-x11
    PUBLIC Prismdrake::foundation
    PRIVATE
        PkgConfig::PRISMDRAKE_XCB
        PkgConfig::PRISMDRAKE_XCB_RANDR)

prismdrake_configure_cpp_target(prismdrake-x11)
prismdrake_enable_warnings(prismdrake-x11)
prismdrake_enable_sanitizers(prismdrake-x11)
