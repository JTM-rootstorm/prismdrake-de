#include "RestartPolicy.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace prismdrake::session {
namespace {

using namespace std::chrono_literals;
using foundation::ErrorCode;
using foundation::MonotonicClock;

TEST(RestartPolicyTest, UsesDistinctReviewVisibleBudgetsAndIncreasingShellBackoff) {
    auto policy = SessionRecoveryPolicy::create();
    ASSERT_TRUE(policy);
    const MonotonicClock::TimePoint start{};

    const auto first = policy.value().observeExit(ComponentRole::shell, ChildExitReason::failure,
                                                  start, start + 1s);
    const auto second = policy.value().observeExit(ComponentRole::shell, ChildExitReason::signal,
                                                   start + 2s, start + 3s);
    const auto third = policy.value().observeExit(
        ComponentRole::shell, ChildExitReason::readiness_timeout, start + 4s, start + 5s);
    const auto exhausted = policy.value().observeExit(
        ComponentRole::shell, ChildExitReason::failure, start + 6s, start + 7s);

    ASSERT_TRUE(first);
    EXPECT_EQ(first.value(), (RecoveryDecision{RecoveryAction::restart_component, 250ms, 1U}));
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value(), (RecoveryDecision{RecoveryAction::restart_component, 500ms, 2U}));
    ASSERT_TRUE(third);
    EXPECT_EQ(third.value(), (RecoveryDecision{RecoveryAction::restart_component, 1s, 3U}));
    ASSERT_TRUE(exhausted);
    EXPECT_EQ(exhausted.value().action, RecoveryAction::enter_safe_mode);

    auto settingsPolicy = SessionRecoveryPolicy::create();
    ASSERT_TRUE(settingsPolicy);
    const auto settingsRestart = settingsPolicy.value().observeExit(
        ComponentRole::settingsd, ChildExitReason::failure, start, start + 1s);
    const auto settingsExhausted = settingsPolicy.value().observeExit(
        ComponentRole::settingsd, ChildExitReason::failure, start + 2s, start + 3s);
    ASSERT_TRUE(settingsRestart);
    EXPECT_EQ(settingsRestart.value(),
              (RecoveryDecision{RecoveryAction::restart_component, 500ms, 1U}));
    ASSERT_TRUE(settingsExhausted);
    EXPECT_EQ(settingsExhausted.value().action, RecoveryAction::enter_safe_mode);
}

TEST(RestartPolicyTest, TreatsSafeModeAsOneFinalLaunchAndNeverRestartsCleanExit) {
    auto policy = SessionRecoveryPolicy::create();
    ASSERT_TRUE(policy);
    const MonotonicClock::TimePoint start{};

    const auto clean =
        policy.value().observeExit(ComponentRole::shell, ChildExitReason::clean, start, start + 1s);
    ASSERT_TRUE(clean);
    EXPECT_EQ(clean.value().action, RecoveryAction::stop_clean);

    for (std::size_t attempt = 0U; attempt < 3U; ++attempt) {
        const auto offset = std::chrono::seconds{static_cast<long long>(attempt * 2U)};
        ASSERT_TRUE(policy.value().observeExit(ComponentRole::shell, ChildExitReason::failure,
                                               start + offset, start + offset + 1s));
    }
    const auto safeMode = policy.value().observeExit(ComponentRole::shell, ChildExitReason::failure,
                                                     start + 6s, start + 7s);
    ASSERT_TRUE(safeMode);
    ASSERT_EQ(safeMode.value().action, RecoveryAction::enter_safe_mode);
    ASSERT_TRUE(policy.value().activateSafeMode());
    EXPECT_TRUE(policy.value().safeModeActive());

    const auto safeModeFailure = policy.value().observeExit(
        ComponentRole::shell, ChildExitReason::failure, start + 8s, start + 9s);
    ASSERT_TRUE(safeModeFailure);
    EXPECT_EQ(safeModeFailure.value().action, RecoveryAction::stop_failed);
    EXPECT_FALSE(policy.value().activateSafeMode());
}

TEST(RestartPolicyTest, UsesAnInclusiveWindowAndResetsAfterAHealthyInterval) {
    auto policy = SessionRecoveryPolicy::create();
    ASSERT_TRUE(policy);
    const MonotonicClock::TimePoint start{};

    ASSERT_TRUE(policy.value().observeExit(ComponentRole::settingsd, ChildExitReason::failure,
                                           start, start + 1s));
    const auto inclusiveBoundary = policy.value().observeExit(
        ComponentRole::settingsd, ChildExitReason::failure, start + 30s, start + 31s);
    ASSERT_TRUE(inclusiveBoundary);
    EXPECT_EQ(inclusiveBoundary.value().action, RecoveryAction::enter_safe_mode);

    auto healthyPolicy = SessionRecoveryPolicy::create();
    ASSERT_TRUE(healthyPolicy);
    ASSERT_TRUE(healthyPolicy.value().observeExit(ComponentRole::settingsd,
                                                  ChildExitReason::failure, start, start + 1s));
    const auto afterHealthyInterval = healthyPolicy.value().observeExit(
        ComponentRole::settingsd, ChildExitReason::failure, start + 2s, start + 32s);
    ASSERT_TRUE(afterHealthyInterval);
    EXPECT_EQ(afterHealthyInterval.value().action, RecoveryAction::restart_component);
    EXPECT_EQ(afterHealthyInterval.value().restartOrdinal, 1U);
}

TEST(RestartPolicyTest, RejectsInvalidPolicyAndNonMonotonicObservations) {
    auto config = pd1SessionRecoveryConfig();
    config.shell.maximumRestarts = 0U;
    const auto invalid = SessionRecoveryPolicy::create(config);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::invalid_argument);

    auto policy = SessionRecoveryPolicy::create();
    ASSERT_TRUE(policy);
    const MonotonicClock::TimePoint start{10s};
    const auto reversed = policy.value().observeExit(ComponentRole::shell, ChildExitReason::failure,
                                                     start, start - 1ms);
    ASSERT_FALSE(reversed);
    EXPECT_EQ(reversed.error().code, ErrorCode::invalid_argument);
}

TEST(RestartPolicyTest, SaturatesBackoffAtTheConfiguredMaximum) {
    using namespace std::chrono_literals;
    const RestartBudget budget{5U, 30s, 30s, 400ms, 500ms};
    auto policy = SessionRecoveryPolicy::create(SessionRecoveryConfig{budget, budget});
    ASSERT_TRUE(policy);
    const MonotonicClock::TimePoint start{};

    const auto first = policy.value().observeExit(ComponentRole::shell, ChildExitReason::failure,
                                                  start, start + 1ms);
    const auto second = policy.value().observeExit(ComponentRole::shell, ChildExitReason::failure,
                                                   start + 2ms, start + 3ms);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value().backoff, 400ms);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().backoff, 500ms);
}

} // namespace
} // namespace prismdrake::session
