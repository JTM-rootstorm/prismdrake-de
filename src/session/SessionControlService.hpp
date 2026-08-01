#pragma once

#include "LogoutTransaction.hpp"
#include "Result.hpp"
#include "SdBusApi.hpp"

#include <chrono>
#include <memory>

namespace prismdrake::session {

struct SessionStateResult final {
    LogoutRequestFailure failure = LogoutRequestFailure::none;
    LogoutSnapshot snapshot;

    [[nodiscard]] bool accepted() const noexcept { return failure == LogoutRequestFailure::none; }
};

/// Experimental same-user Session1 object. Name ownership remains with the
/// embedding session runtime so one process owns one explicit owner epoch.
class SessionControlService final {
  public:
    explicit SessionControlService(ExactChildShutdownPort &shutdownPort);
    SessionControlService(
        ExactChildShutdownPort &shutdownPort,
        std::shared_ptr<const foundation::MonotonicClock> clock,
        foundation::MonotonicClock::Duration getterBudget = std::chrono::seconds{1},
        foundation::MonotonicClock::Duration mutationBudget = std::chrono::seconds{5});

    [[nodiscard]] foundation::Result<void> attach(sd_bus *bus);
    [[nodiscard]] LogoutSnapshot snapshot() const noexcept;
    [[nodiscard]] SessionStateResult getSessionState() const;
    [[nodiscard]] LogoutRequestResult requestLogout();
    [[nodiscard]] LogoutRequestResult confirmLogout(std::uint64_t transactionId);
    [[nodiscard]] LogoutRequestResult cancelLogout(std::uint64_t transactionId);
    [[nodiscard]] LogoutRequestResult observeShutdownEvent(std::uint64_t transactionId,
                                                           ExactChildShutdownEvent event);
    void beginServiceStop() noexcept;

  private:
    void emitTransition(const LogoutSnapshot &snapshot) noexcept;

    sd_bus *bus_ = nullptr;
    ipc::sdbus::Slot slot_;
    std::shared_ptr<const foundation::MonotonicClock> clock_;
    foundation::MonotonicClock::Duration getter_budget_;
    LogoutTransaction transaction_;
};

} // namespace prismdrake::session
