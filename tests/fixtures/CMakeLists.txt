add_executable(prismdrake-session-child-fixture SessionChildFixture.cpp)

prismdrake_configure_cpp_target(prismdrake-session-child-fixture)
prismdrake_enable_warnings(prismdrake-session-child-fixture)
prismdrake_enable_sanitizers(prismdrake-session-child-fixture)
