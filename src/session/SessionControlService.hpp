#pragma once

#include "LogoutTransaction.hpp"
#include "Result.hpp"
#include "SdBusApi.hpp"

namespace prismdrake::session {

/// Experimental same-user Session1 object. Name ownership remains with the
/// embedding session runtime so one process owns one explicit owner epoch.
class SessionControlService final {
  public:
    explicit SessionControlService(ExactChildShutdownPort &shutdownPort);

    [[nodiscard]] foundation::Result<void> attach(sd_bus *bus);
    [[nodiscard]] LogoutSnapshot snapshot() const noexcept;
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
    LogoutTransaction transaction_;
};

} // namespace prismdrake::session
