#include "Result.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace prismdrake::foundation {
namespace {

TEST(ResultTest, CarriesSuccessfulValue) {
    auto result = Result<std::string>::success("ready");

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), "ready");
}

TEST(ResultTest, CarriesActionableFailure) {
    auto result = Result<int>::failure(
        {ErrorCode::invalid_argument, "limit must be positive", "choose a nonzero limit"});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(result.error().message, "limit must be positive");
    EXPECT_EQ(result.error().recovery, "choose a nonzero limit");
}

TEST(ResultTest, SupportsMoveOnlyValues) {
    auto result = Result<std::unique_ptr<int>>::success(std::make_unique<int>(42));

    ASSERT_TRUE(result);
    auto value = std::move(result).value();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 42);
}

TEST(ResultTest, RepresentsVoidSuccessAndFailure) {
    const auto success = Result<void>::success();
    const auto failure =
        Result<void>::failure({ErrorCode::cancelled, "operation cancelled", "retry if needed"});

    EXPECT_TRUE(success);
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().code, ErrorCode::cancelled);
}

} // namespace
} // namespace prismdrake::foundation
