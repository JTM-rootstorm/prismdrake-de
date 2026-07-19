#include "LogoutTransaction.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace prismdrake::session;

class ShutdownPort final : public ExactChildShutdownPort {
  public:
    [[nodiscard]] bool beginExactChildShutdown(std::uint64_t transactionId) override {
        calls.push_back(transactionId);
        return accepts;
    }

    bool accepts = true;
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
    port.accepts = false;
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

} // namespace
