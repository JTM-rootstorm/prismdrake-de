#include "LogoutTransaction.hpp"

#include <limits>
#include <memory>
#include <utility>

namespace prismdrake::session {

std::string_view logoutStateName(LogoutState state) noexcept {
    switch (state) {
    case LogoutState::idle:
        return "idle";
    case LogoutState::confirmation_requested:
        return "confirmation_requested";
    case LogoutState::confirmed:
        return "confirmed";
    case LogoutState::stopping_shell:
        return "stopping_shell";
    case LogoutState::stopping_notifyd:
        return "stopping_notifyd";
    case LogoutState::stopping_settingsd:
        return "stopping_settingsd";
    case LogoutState::complete:
        return "complete";
    case LogoutState::cancelled:
        return "cancelled";
    case LogoutState::failed:
        return "failed";
    }
    return "failed";
}

LogoutTransaction::LogoutTransaction(ExactChildShutdownPort &shutdownPort,
                                     TransitionObserver observer, std::uint64_t firstTransactionId)
    : LogoutTransaction(shutdownPort, std::move(observer),
                        std::make_shared<foundation::SystemMonotonicClock>(),
                        std::chrono::seconds{5}, firstTransactionId) {}

LogoutTransaction::LogoutTransaction(ExactChildShutdownPort &shutdownPort,
                                     TransitionObserver observer,
                                     std::shared_ptr<const foundation::MonotonicClock> clock,
                                     foundation::MonotonicClock::Duration mutationBudget,
                                     std::uint64_t firstTransactionId)
    : shutdown_port_(&shutdownPort), observer_(std::move(observer)),
      clock_(clock ? std::move(clock) : std::make_shared<foundation::SystemMonotonicClock>()),
      mutation_budget_(mutationBudget),
      next_transaction_id_(firstTransactionId == 0 ? 1 : firstTransactionId) {}

LogoutSnapshot LogoutTransaction::snapshot() const noexcept { return snapshot_; }

LogoutRequestResult LogoutTransaction::request() {
    const auto deadline = mutationDeadline();
    if (!deadline) {
        return {LogoutRequestFailure::timed_out, snapshot_.transactionId};
    }
    if (service_stopping_) {
        return {LogoutRequestFailure::service_stopping, snapshot_.transactionId};
    }
    if (snapshot_.state != LogoutState::idle) {
        return {LogoutRequestFailure::busy, snapshot_.transactionId};
    }
    if (clock_->now() >= *deadline) {
        return {LogoutRequestFailure::timed_out, snapshot_.transactionId};
    }
    const auto transactionId = allocateTransactionId();
    snapshot_.transactionId = transactionId;
    transition(LogoutState::confirmation_requested, transactionId);
    return {LogoutRequestFailure::none, transactionId};
}

LogoutRequestResult LogoutTransaction::confirm(std::uint64_t transactionId) {
    const auto deadline = mutationDeadline();
    if (!deadline) {
        return {LogoutRequestFailure::timed_out, snapshot_.transactionId};
    }
    const auto failure = validateCancellable(transactionId);
    if (failure != LogoutRequestFailure::none) {
        return {failure, snapshot_.transactionId};
    }
    if (clock_->now() >= *deadline) {
        return {LogoutRequestFailure::timed_out, snapshot_.transactionId};
    }
    const auto shutdownStart = shutdown_port_->beginExactChildShutdown(transactionId, *deadline);
    if (shutdownStart == ExactChildShutdownStart::timed_out) {
        return {LogoutRequestFailure::timed_out, transactionId};
    }
    if (shutdownStart == ExactChildShutdownStart::unavailable) {
        transition(LogoutState::confirmed, transactionId);
        transition(LogoutState::failed, transactionId);
        return {LogoutRequestFailure::internal, transactionId};
    }
    transition(LogoutState::confirmed, transactionId);
    transition(LogoutState::stopping_shell, transactionId);
    return {LogoutRequestFailure::none, transactionId};
}

LogoutRequestResult LogoutTransaction::cancel(std::uint64_t transactionId) {
    const auto deadline = mutationDeadline();
    if (!deadline) {
        return {LogoutRequestFailure::timed_out, snapshot_.transactionId};
    }
    const auto failure = validateCancellable(transactionId);
    if (failure != LogoutRequestFailure::none) {
        return {failure, snapshot_.transactionId};
    }
    if (clock_->now() >= *deadline) {
        return {LogoutRequestFailure::timed_out, snapshot_.transactionId};
    }
    transition(LogoutState::cancelled, transactionId);
    snapshot_.transactionId = 0;
    transition(LogoutState::idle, transactionId);
    return {LogoutRequestFailure::none, transactionId};
}

LogoutRequestResult LogoutTransaction::observeShutdownEvent(std::uint64_t transactionId,
                                                            ExactChildShutdownEvent event) {
    if (transactionId == 0) {
        return {LogoutRequestFailure::no_transaction, snapshot_.transactionId};
    }
    if (transactionId != snapshot_.transactionId) {
        return {LogoutRequestFailure::transaction_mismatch, snapshot_.transactionId};
    }
    if (event == ExactChildShutdownEvent::failed &&
        (snapshot_.state == LogoutState::stopping_shell ||
         snapshot_.state == LogoutState::stopping_notifyd ||
         snapshot_.state == LogoutState::stopping_settingsd ||
         snapshot_.state == LogoutState::confirmed)) {
        transition(LogoutState::failed, transactionId);
        return {LogoutRequestFailure::none, transactionId};
    }

    LogoutState next = LogoutState::failed;
    bool expected = false;
    if (snapshot_.state == LogoutState::stopping_shell &&
        event == ExactChildShutdownEvent::shell_stopped) {
        next = LogoutState::stopping_notifyd;
        expected = true;
    } else if (snapshot_.state == LogoutState::stopping_notifyd &&
               event == ExactChildShutdownEvent::notifyd_stopped) {
        next = LogoutState::stopping_settingsd;
        expected = true;
    } else if (snapshot_.state == LogoutState::stopping_settingsd &&
               event == ExactChildShutdownEvent::settingsd_stopped) {
        next = LogoutState::complete;
        expected = true;
    }
    if (!expected) {
        return {LogoutRequestFailure::invalid_state, snapshot_.transactionId};
    }
    transition(next, transactionId);
    return {LogoutRequestFailure::none, transactionId};
}

void LogoutTransaction::beginServiceStop() noexcept { service_stopping_ = true; }

LogoutRequestFailure LogoutTransaction::validateCancellable(std::uint64_t transactionId) const {
    if (service_stopping_) {
        return LogoutRequestFailure::service_stopping;
    }
    if (transactionId == 0 || snapshot_.transactionId == 0) {
        return LogoutRequestFailure::no_transaction;
    }
    if (transactionId != snapshot_.transactionId) {
        return LogoutRequestFailure::transaction_mismatch;
    }
    if (snapshot_.state != LogoutState::confirmation_requested) {
        return LogoutRequestFailure::invalid_state;
    }
    return LogoutRequestFailure::none;
}

std::optional<foundation::MonotonicClock::TimePoint> LogoutTransaction::mutationDeadline() const {
    const auto now = clock_->now();
    if (mutation_budget_ <= foundation::MonotonicClock::Duration::zero() ||
        now > foundation::MonotonicClock::TimePoint::max() - mutation_budget_) {
        return std::nullopt;
    }
    return now + mutation_budget_;
}

void LogoutTransaction::transition(LogoutState state, std::uint64_t signalTransactionId) {
    snapshot_.state = state;
    if (observer_) {
        observer_({signalTransactionId, state});
    }
}

std::uint64_t LogoutTransaction::allocateTransactionId() noexcept {
    const auto allocated = next_transaction_id_;
    if (next_transaction_id_ == std::numeric_limits<std::uint64_t>::max()) {
        next_transaction_id_ = 1;
    } else {
        ++next_transaction_id_;
    }
    return allocated;
}

} // namespace prismdrake::session
