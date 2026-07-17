#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string_view>

namespace prismdrake::x11 {
namespace {

TEST(X11ConnectionIntegrationTest, CompletesARealServerRoundTrip) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    ASSERT_FALSE(std::string_view{display}.empty());

    auto connection = X11Connection::connect(display);

    ASSERT_TRUE(connection);
    EXPECT_TRUE(connection.value().healthy());
    EXPECT_GE(connection.value().eventFileDescriptor(), 0);
    EXPECT_NE(connection.value().screen().rootWindow.value(), 0U);
    EXPECT_GT(connection.value().screen().widthPx, 0U);
    EXPECT_GT(connection.value().screen().heightPx, 0U);
}

TEST(X11ConnectionIntegrationTest, RejectsADeadDisplayWithoutEchoingIt) {
    constexpr std::string_view sentinel = ":54321-private-sentinel";
    const auto connection = X11Connection::connect(sentinel);

    ASSERT_FALSE(connection);
    EXPECT_EQ(connection.error().message.find(sentinel), std::string::npos);
    EXPECT_EQ(connection.error().recovery.find(sentinel), std::string::npos);
}

} // namespace
} // namespace prismdrake::x11
