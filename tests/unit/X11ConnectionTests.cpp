#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <string>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;

TEST(X11ConnectionTest, RejectsInvalidDisplayNamesWithoutAttemptingAConnection) {
    const auto empty = X11Connection::connect("");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::invalid_environment);

    const auto whitespace = X11Connection::connect(" :1");
    ASSERT_FALSE(whitespace);
    EXPECT_EQ(whitespace.error().message.find(":1"), std::string::npos);

    const std::string embeddedNull{"private:1\0suffix", 16U};
    EXPECT_FALSE(X11Connection::connect(embeddedNull));

    const std::string oversized(maximumDisplayNameBytes + 1U, 'x');
    EXPECT_FALSE(X11Connection::connect(oversized));
}

TEST(X11ConnectionTest, RejectsZeroWindowIdentifiers) {
    const auto zero = WindowId::fromProtocol(0U);
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().code, ErrorCode::invalid_argument);

    const auto valid = WindowId::fromProtocol(42U);
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid.value().value(), 42U);
}

TEST(X11ConnectionTest, RejectsNoneAsAnAtomIdentifier) {
    const auto none = AtomId::fromProtocol(0U);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, ErrorCode::invalid_argument);

    const auto valid = AtomId::fromProtocol(42U);
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid.value().value(), 42U);
}

} // namespace
} // namespace prismdrake::x11
