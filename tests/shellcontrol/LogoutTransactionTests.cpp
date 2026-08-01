#include "LogoutTransaction.hpp"
#include "SessionControlService.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace prismdrake::session;

class ShutdownPort final : public ExactChildShutdownPort {
  public:
    // Test-only deadline-aware blocking probe; production admission is non-waiting.
    [[nodiscard]] ExactChildShutdownStart
    beginExactChildShutdown(std::uint64_t transactionId,
                            prismdrake::foundation::MonotonicClock::TimePoint deadline) override {
        if (block_until_deadline) {
            std::this_thread::sleep_until(deadline);
            return ExactChildShutdownStart::timed_out;
        }
        calls.push_back(transactionId);
        return response;
    }

    bool block_until_deadline = false;
    ExactChildShutdownStart response = ExactChildShutdownStart::started;
    std::vector<std::uint64_t> calls;
};

TEST(LogoutTransactionTest, RequestsAndCancelsOneExactTransaction) {
    ShutdownPort port;
    std::vector<LogoutSnapshot> transitions;
    LogoutTransaction transaction{port, [&](const auto &state) { transitions.push_back(state); }};

    const auto requested = transaction.request();
    ASSERT_TRUE(requested.accepted());
    ASSERT_NE(requested.transactionId, 0U);
    EXPECT_EQ(transaction.snapshot(),
              (LogoutSnapshot{requested.transactionId, LogoutState::confirmation_requested}));
    EXPECT_EQ(transaction.request().failure, LogoutRequestFailure::busy);
    EXPECT_EQ(transaction.cancel(requested.transactionId + 1U).failure,
              LogoutRequestFailure::transaction_mismatch);

    EXPECT_TRUE(transaction.cancel(requested.transactionId).accepted());
    EXPECT_EQ(transaction.snapshot(), (LogoutSnapshot{0, LogoutState::idle}));
    ASSERT_EQ(transitions.size(), 3U);
    EXPECT_EQ(transitions[0].state, LogoutState::confirmation_requested);
    EXPECT_EQ(transitions[1].state, LogoutState::cancelled);
    EXPECT_EQ(transitions[2], (LogoutSnapshot{requested.transactionId, LogoutState::idle}));
}

TEST(LogoutTransactionTest, ConfirmsBoundedExactChildOrder) {
    ShutdownPort port;
    std::vector<LogoutSnapshot> transitions;
    LogoutTransaction transaction{port, [&](const auto &state) { transitions.push_back(state); }};
    const auto id = transaction.request().transactionId;

    EXPECT_TRUE(transaction.confirm(id).accepted());
    ASSERT_EQ(port.calls, (std::vector<std::uint64_t>{id}));
    EXPECT_EQ(transaction.snapshot().state, LogoutState::stopping_shell);
    EXPECT_EQ(
        transaction.observeShutdownEvent(id, ExactChildShutdownEvent::notifyd_stopped).failure,
        LogoutRequestFailure::invalid_state);
    EXPECT_TRUE(
        transaction.observeShutdownEvent(id, ExactChildShutdownEvent::shell_stopped).accepted());
    EXPECT_EQ(transaction.snapshot().state, LogoutState::stopping_notifyd);
    EXPECT_TRUE(
        transaction.observeShutdownEvent(id, ExactChildShutdownEvent::notifyd_stopped).accepted());
    EXPECT_EQ(transaction.snapshot().state, LogoutState::stopping_settingsd);
    EXPECT_TRUE(transaction.observeShutdownEvent(id, ExactChildShutdownEvent::settingsd_stopped)
                    .accepted());
    EXPECT_EQ(transaction.snapshot(), (LogoutSnapshot{id, LogoutState::complete}));

    const std::vector expectedStates{
        LogoutState::confirmation_requested, LogoutState::confirmed,
        LogoutState::stopping_shell,         LogoutState::stopping_notifyd,
        LogoutState::stopping_settingsd,     LogoutState::complete,
    };
    ASSERT_EQ(transitions.size(), expectedStates.size());
    for (std::size_t index = 0; index < expectedStates.size(); ++index) {
        EXPECT_EQ(transitions[index].state, expectedStates[index]);
        EXPECT_EQ(transitions[index].transactionId, id);
    }
}

TEST(LogoutTransactionTest, RejectsZeroStaleAndPostConfirmationChanges) {
    ShutdownPort port;
    LogoutTransaction transaction{port, {}};
    EXPECT_EQ(transaction.confirm(0).failure, LogoutRequestFailure::no_transaction);
    const auto first = transaction.request().transactionId;
    EXPECT_TRUE(transaction.cancel(first).accepted());
    const auto second = transaction.request().transactionId;
    EXPECT_NE(first, second);
    EXPECT_EQ(transaction.confirm(first).failure, LogoutRequestFailure::transaction_mismatch);
    EXPECT_TRUE(transaction.confirm(second).accepted());
    EXPECT_EQ(transaction.cancel(second).failure, LogoutRequestFailure::invalid_state);
}

TEST(LogoutTransactionTest, FailsClosedWhenShutdownCannotBeginOrReportsFailure) {
    ShutdownPort port;
    port.response = ExactChildShutdownStart::unavailable;
    LogoutTransaction transaction{port, {}};
    const auto id = transaction.request().transactionId;
    EXPECT_EQ(transaction.confirm(id).failure, LogoutRequestFailure::internal);
    EXPECT_EQ(transaction.snapshot().state, LogoutState::failed);

    ShutdownPort secondPort;
    LogoutTransaction second{secondPort, {}};
    const auto secondId = second.request().transactionId;
    ASSERT_TRUE(second.confirm(secondId).accepted());
    EXPECT_TRUE(second.observeShutdownEvent(secondId, ExactChildShutdownEvent::failed).accepted());
    EXPECT_EQ(second.snapshot().state, LogoutState::failed);
}

TEST(LogoutTransactionTest, ServiceStopRejectsNewOrCancellableWork) {
    ShutdownPort port;
    LogoutTransaction transaction{port, {}};
    const auto id = transaction.request().transactionId;
    transaction.beginServiceStop();
    EXPECT_EQ(transaction.cancel(id).failure, LogoutRequestFailure::service_stopping);

    ShutdownPort idlePort;
    LogoutTransaction idle{idlePort, {}};
    idle.beginServiceStop();
    EXPECT_EQ(idle.request().failure, LogoutRequestFailure::service_stopping);
}

TEST(LogoutTransactionTest, OwnerEpochMayBeginAtANonzeroDistinctIdentifier) {
    ShutdownPort firstPort;
    ShutdownPort secondPort;
    LogoutTransaction first{firstPort, {}, 0x1000U};
    LogoutTransaction second{secondPort, {}, 0x2000U};
    EXPECT_EQ(first.request().transactionId, 0x1000U);
    EXPECT_EQ(second.request().transactionId, 0x2000U);
}

TEST(LogoutTransactionTest, TimesOutBeforeAnExpiredMutationCanPublishState) {
    auto clock = std::make_shared<prismdrake::foundation::TestMonotonicClock>();
    ShutdownPort port;
    std::vector<LogoutSnapshot> transitions;
    LogoutTransaction transaction{port, [&](const auto &state) { transitions.push_back(state); },
                                  clock, prismdrake::foundation::MonotonicClock::Duration::zero()};

    const auto result = transaction.request();
    EXPECT_EQ(result.failure, LogoutRequestFailure::timed_out);
    EXPECT_EQ(transaction.snapshot(), (LogoutSnapshot{0, LogoutState::idle}));
    EXPECT_TRUE(transitions.empty());
}

TEST(LogoutTransactionTest, PropagatesARealAdmissionDeadlineWithoutLateShutdown) {
    using namespace std::chrono_literals;

    auto clock = std::make_shared<prismdrake::foundation::SystemMonotonicClock>();
    ShutdownPort port;
    port.block_until_deadline = true;
    std::vector<LogoutSnapshot> transitions;
    LogoutTransaction transaction{port, [&](const auto &state) { transitions.push_back(state); },
                                  clock, 40ms};
    const auto id = transaction.request().transactionId;

    const auto started = std::chrono::steady_clock::now();
    const auto confirmed = transaction.confirm(id);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(confirmed.failure, LogoutRequestFailure::timed_out);
    EXPECT_GE(elapsed, 40ms);
    EXPECT_TRUE(port.calls.empty());
    std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(port.calls.empty());
    EXPECT_EQ(transaction.snapshot(), (LogoutSnapshot{id, LogoutState::confirmation_requested}));
    ASSERT_EQ(transitions.size(), 1U);
    EXPECT_EQ(transitions.back(), (LogoutSnapshot{id, LogoutState::confirmation_requested}));
    const auto transitionCountAtReply = transitions.size();

    EXPECT_EQ(transaction.observeShutdownEvent(id, ExactChildShutdownEvent::shell_stopped).failure,
              LogoutRequestFailure::invalid_state);
    EXPECT_EQ(transaction.snapshot(), (LogoutSnapshot{id, LogoutState::confirmation_requested}));
    EXPECT_EQ(transitions.size(), transitionCountAtReply);
    EXPECT_TRUE(transaction.cancel(id).accepted());
    EXPECT_EQ(transaction.snapshot(), (LogoutSnapshot{0U, LogoutState::idle}));
}

TEST(LogoutTransactionTest, KeepsAnAcceptedRequestInTheFrozenStateGraph) {
    using namespace std::chrono_literals;

    auto clock = std::make_shared<prismdrake::foundation::TestMonotonicClock>();
    ShutdownPort port;
    std::vector<LogoutSnapshot> transitions;
    LogoutTransaction transaction{port,
                                  [&](const auto &state) {
                                      transitions.push_back(state);
                                      if (state.state == LogoutState::confirmation_requested) {
                                          (void)clock->advance(5s);
                                      }
                                  },
                                  clock};

    const auto result = transaction.request();
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(transaction.snapshot(),
              (LogoutSnapshot{result.transactionId, LogoutState::confirmation_requested}));
    ASSERT_EQ(transitions.size(), 1U);
}

TEST(LogoutTransactionTest, CompletesAcceptedCancellationWithoutInventingFailure) {
    using namespace std::chrono_literals;

    auto clock = std::make_shared<prismdrake::foundation::TestMonotonicClock>();
    ShutdownPort port;
    std::vector<LogoutSnapshot> transitions;
    LogoutTransaction transaction{port,
                                  [&](const auto &state) {
                                      transitions.push_back(state);
                                      if (state.state == LogoutState::idle) {
                                          (void)clock->advance(5s);
                                      }
                                  },
                                  clock};
    const auto id = transaction.request().transactionId;

    const auto result = transaction.cancel(id);
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(transaction.snapshot(), (LogoutSnapshot{0, LogoutState::idle}));
    ASSERT_EQ(transitions.size(), 3U);
    EXPECT_EQ(transitions[1], (LogoutSnapshot{id, LogoutState::cancelled}));
    EXPECT_EQ(transitions[2], (LogoutSnapshot{id, LogoutState::idle}));
    EXPECT_TRUE(transaction.request().accepted());
}

TEST(SessionControlServiceTest, AppliesTheGetterBudgetWithAnInjectableClock) {
    auto clock = std::make_shared<prismdrake::foundation::TestMonotonicClock>();
    ShutdownPort port;
    SessionControlService service{port, clock,
                                  prismdrake::foundation::MonotonicClock::Duration::zero()};

    const auto result = service.getSessionState();
    EXPECT_EQ(result.failure, LogoutRequestFailure::timed_out);
}

} // namespace
