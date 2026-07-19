#pragma once

#include "Result.hpp"

#include <chrono>

#include <sys/types.h>

namespace prismdrake::session {

inline constexpr auto maximumNotificationReadinessTimeout = std::chrono::seconds{5};

/// Verifies that both notification service names belong to the exact notifyd child.
///
/// A different owner is reported as `busy` so the session can enter bounded
/// notification-degraded mode instead of restarting into a name-conflict loop.
[[nodiscard]] foundation::Result<void>
probeNotificationReadiness(pid_t expectedProcess, std::chrono::milliseconds timeout);

} // namespace prismdrake::session
