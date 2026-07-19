#pragma once

#include "NotificationAuthority.hpp"
#include "Result.hpp"

#include <csignal>
#include <cstdint>

namespace prismdrake::notifyd {

enum class ServiceEpochOutcome : std::uint8_t { stopped, disconnected, name_conflict };

/// Latches a failed transport operation that happened after authority state was
/// committed. Callers must return the committed domain result, then replace the
/// bus-owner epoch instead of inviting a duplicate mutation retry.
class PostCommitTransportGuard final {
  public:
    void observe(bool succeeded) noexcept { degraded_ = degraded_ || !succeeded; }
    [[nodiscard]] bool degraded() const noexcept { return degraded_; }

  private:
    bool degraded_{false};
};

/// Runs one session-bus owner epoch. A conflicting freedesktop or Prismdrake
/// name is a clean degraded outcome and must not be retried in a tight loop.
[[nodiscard]] foundation::Result<ServiceEpochOutcome>
runServiceEpoch(NotificationAuthority authority, const volatile std::sig_atomic_t &stopRequested);

} // namespace prismdrake::notifyd
