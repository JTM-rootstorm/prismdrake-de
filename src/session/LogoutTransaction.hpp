#pragma once

#include "MonotonicClock.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace prismdrake::session {

enum class LogoutState : std::uint8_t {
    idle,
    confirmation_requested,
    confirmed,
    stopping_shell,
    stopping_notifyd,
    stopping_settingsd,
    complete,
    cancelled,
    failed,
};

[[nodiscard]] std::string_view logoutStateName(LogoutState state) noexcept;

struct LogoutSnapshot final {
    std::uint64_t transactionId = 0;
    LogoutState state = LogoutState::idle;

    friend bool operator==(const LogoutSnapshot &, const LogoutSnapshot &) = default;
};

enum class LogoutRequestFailure : std::uint8_t {
    none,
    no_transaction,
    transaction_mismatch,
    invalid_state,
    busy,
    timed_out,
    service_stopping,
    internal,
};

struct LogoutRequestResult final {
    LogoutRequestFailure failure = LogoutRequestFailure::none;
    std::uint64_t transactionId = 0;

    [[nodiscard]] bool accepted() const noexcept { return failure == LogoutRequestFailure::none; }
};

enum class ExactChildShutdownEvent : std::uint8_t {
    shell_stopped,
    notifyd_stopped,
    settingsd_stopped,
    failed,
};

enum class ExactChildShutdownStart : std::uint8_t {
    started,
    timed_out,
    unavailable,
};

/// Begins the existing supervisor's bounded exact-child shutdown sequence.
///
/// Implementations must perform only a non-waiting owner-context admission step
/// and may queue shutdown for only the exact supervised shell, notifyd, and
/// settingsd children. The deadline is the latest admission point. A timed-out
/// result must not queue shutdown; once queued, the operation returns `started`.
/// Completion observations are reported back on the session dispatch context
/// through `observeShutdownEvent()`.
class ExactChildShutdownPort {
  public:
    virtual ~ExactChildShutdownPort() = default;
    [[nodiscard]] virtual ExactChildShutdownStart
    beginExactChildShutdown(std::uint64_t transactionId,
                            foundation::MonotonicClock::TimePoint deadline) = 0;
};

/// Authoritative, single-threaded PD2 logout transaction.
class LogoutTransaction final {
  public:
    using TransitionObserver = std::function<void(const LogoutSnapshot &)>;

    LogoutTransaction(ExactChildShutdownPort &shutdownPort, TransitionObserver observer,
                      std::uint64_t firstTransactionId = 1);
    LogoutTransaction(ExactChildShutdownPort &shutdownPort, TransitionObserver observer,
                      std::shared_ptr<const foundation::MonotonicClock> clock,
                      foundation::MonotonicClock::Duration mutationBudget = std::chrono::seconds{5},
                      std::uint64_t firstTransactionId = 1);

    [[nodiscard]] LogoutSnapshot snapshot() const noexcept;
    [[nodiscard]] LogoutRequestResult request();
    [[nodiscard]] LogoutRequestResult confirm(std::uint64_t transactionId);
    [[nodiscard]] LogoutRequestResult cancel(std::uint64_t transactionId);
    [[nodiscard]] LogoutRequestResult observeShutdownEvent(std::uint64_t transactionId,
                                                           ExactChildShutdownEvent event);
    void beginServiceStop() noexcept;

  private:
    [[nodiscard]] LogoutRequestFailure validateCancellable(std::uint64_t transactionId) const;
    [[nodiscard]] std::optional<foundation::MonotonicClock::TimePoint> mutationDeadline() const;
    void transition(LogoutState state, std::uint64_t signalTransactionId);
    [[nodiscard]] std::uint64_t allocateTransactionId() noexcept;

    ExactChildShutdownPort *shutdown_port_;
    TransitionObserver observer_;
    std::shared_ptr<const foundation::MonotonicClock> clock_;
    foundation::MonotonicClock::Duration mutation_budget_;
    LogoutSnapshot snapshot_;
    std::uint64_t next_transaction_id_ = 1;
    bool service_stopping_ = false;
};

} // namespace prismdrake::session
