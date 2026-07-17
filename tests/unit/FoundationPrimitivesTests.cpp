#include "Cancellation.hpp"
#include "ExitStatus.hpp"
#include "Generation.hpp"
#include "MonotonicClock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace prismdrake::foundation {
namespace {

using namespace std::chrono_literals;

TEST(GenerationTest, RejectsZeroAndAdvancesMonotonically) {
    EXPECT_EQ(Generation::firstPublished().value(), 1U);

    const auto unpublished = Generation::fromPublished(Generation::unpublishedValue);
    ASSERT_FALSE(unpublished);
    EXPECT_EQ(unpublished.error().code, ErrorCode::invalid_argument);

    const auto first = Generation::fromPublished(1);
    ASSERT_TRUE(first);
    const auto second = first.value().next();
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().value(), 2U);
    EXPECT_GT(second.value(), first.value());
}

TEST(GenerationTest, ReportsOverflowWithoutWrapping) {
    const auto maximum = Generation::fromPublished(std::numeric_limits<Generation::Value>::max());
    ASSERT_TRUE(maximum);

    const auto overflow = maximum.value().next();
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, ErrorCode::too_large);
}

TEST(MonotonicClockTest, AdvancesDeterministicallyAndNeverMovesBackward) {
    const MonotonicClock::TimePoint start{10s};
    TestMonotonicClock clock(start);
    EXPECT_EQ(clock.now(), start);

    EXPECT_TRUE(clock.advance(25ms));
    EXPECT_EQ(clock.now(), start + 25ms);

    const auto backward = clock.advance(-1ms);
    ASSERT_FALSE(backward);
    EXPECT_EQ(backward.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(clock.now(), start + 25ms);
}

TEST(CancellationTest, SharesOneExplicitThreadSafeState) {
    CancellationSource source;
    const CancellationToken token = source.token();
    // This intentionally exercises value-copy sharing rather than another source lookup.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const CancellationToken token_copy = token;
    EXPECT_FALSE(token.isCancellationRequested());
    EXPECT_FALSE(token_copy.isCancellationRequested());

    std::atomic_uint transitions{0};
    std::vector<std::thread> callers;
    callers.reserve(8);
    for (std::size_t index = 0; index < 8; ++index) {
        callers.emplace_back([&source, &transitions] {
            if (source.requestCancellation()) {
                transitions.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &caller : callers) {
        caller.join();
    }

    EXPECT_EQ(transitions.load(std::memory_order_relaxed), 1U);
    EXPECT_TRUE(token.isCancellationRequested());
    EXPECT_TRUE(token_copy.isCancellationRequested());
    EXPECT_FALSE(source.requestCancellation());
}

TEST(ExitStatusTest, MapsEveryFoundationErrorToAStableProcessStatus) {
    EXPECT_EQ(exitStatusFor(ErrorCode::invalid_argument), ExitStatus::invalid_usage);
    EXPECT_EQ(exitStatusFor(ErrorCode::syntax_error), ExitStatus::invalid_usage);
    EXPECT_EQ(exitStatusFor(ErrorCode::validation_error), ExitStatus::invalid_usage);
    EXPECT_EQ(exitStatusFor(ErrorCode::invalid_environment), ExitStatus::unavailable);
    EXPECT_EQ(exitStatusFor(ErrorCode::not_found), ExitStatus::unavailable);
    EXPECT_EQ(exitStatusFor(ErrorCode::permission_denied), ExitStatus::permission_denied);
    EXPECT_EQ(exitStatusFor(ErrorCode::too_large), ExitStatus::resource_limit);
    EXPECT_EQ(exitStatusFor(ErrorCode::io_error), ExitStatus::io_failure);
    EXPECT_EQ(exitStatusFor(ErrorCode::durability_uncertain), ExitStatus::io_failure);
    EXPECT_EQ(exitStatusFor(ErrorCode::unsupported), ExitStatus::unavailable);
    EXPECT_EQ(exitStatusFor(ErrorCode::cancelled), ExitStatus::cancelled);

    EXPECT_EQ(processExitCode(ExitStatus::success), 0);
    EXPECT_EQ(processExitCode(ExitStatus::cancelled), 7);
}

} // namespace
} // namespace prismdrake::foundation
