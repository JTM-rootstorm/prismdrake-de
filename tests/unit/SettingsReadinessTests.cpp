#include "SettingsReadiness.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace prismdrake::session {
namespace {

using namespace std::chrono_literals;
using foundation::ErrorCode;

TEST(SettingsReadinessTest, RejectsUnboundedTimeoutsBeforeConnectingToTheBus) {
    const auto zero = probeSettingsReadiness(0ms);
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().code, ErrorCode::invalid_argument);

    const auto excessive = probeSettingsReadiness(maximumSettingsReadinessTimeout + 1ms);
    ASSERT_FALSE(excessive);
    EXPECT_EQ(excessive.error().code, ErrorCode::invalid_argument);
}

} // namespace
} // namespace prismdrake::session
