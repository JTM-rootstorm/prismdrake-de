add_executable(prismdrake-wallpaper-tests
    TestMain.cpp
    WallpaperLayoutTests.cpp
    WallpaperLoaderTests.cpp
    WallpaperModelTests.cpp)
target_link_libraries(prismdrake-wallpaper-tests
    PRIVATE Prismdrake::wallpaper-core GTest::gtest Qt6::Core Qt6::Gui)

prismdrake_configure_cpp_target(prismdrake-wallpaper-tests)
prismdrake_enable_warnings(prismdrake-wallpaper-tests)
prismdrake_enable_sanitizers(prismdrake-wallpaper-tests)
gtest_discover_tests(prismdrake-wallpaper-tests DISCOVERY_MODE PRE_TEST)
